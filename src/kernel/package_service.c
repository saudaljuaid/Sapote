/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/fat32_fs.h>
#include <sapote/heap.h>
#include <sapote/package_service.h>
#include <sapote/package_state.h>

#define FILE_PATH_BYTES 128U
#define FILE_MODE_OFFSET 136U
#define FILE_LENGTH_OFFSET 144U
#define FILE_DIGEST_OFFSET 152U
#define GENERATION_PREFIX "pkgstate/gen/"
#define GENERATION_ROOT_SUFFIX "/root"
#define GENERATION_DATABASE_SUFFIX "/state.db"

struct service_context {
    struct package_service_report *report;
    struct sapfs_list_entry *entries;
    uint32_t walk_entries;
};

struct loaded_generation {
    struct package_state_generation candidate;
    struct package_state_database_view view;
    bool present;
};

static bool servicing;

static void zero_bytes(void *destination, size_t count)
{
    uint8_t *bytes = (uint8_t *)destination;

    for (size_t index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool equal_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;

    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
        (uint32_t)bytes[1] << 8U |
        (uint32_t)bytes[2] << 16U |
        (uint32_t)bytes[3] << 24U;
}

static uint64_t read_u64(const uint8_t *bytes)
{
    return (uint64_t)read_u32(bytes) |
        (uint64_t)read_u32(bytes + 4U) << 32U;
}

static size_t string_length(const char *text, size_t capacity)
{
    size_t length = 0U;

    if (text == NULL) {
        return capacity;
    }
    while (length < capacity && text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool append_text(char *path, size_t capacity, const char *text)
{
    size_t used = string_length(path, capacity);
    size_t added = string_length(text, capacity);

    if (used >= capacity || added >= capacity || added >= capacity - used) {
        return false;
    }
    for (size_t index = 0U; index <= added; ++index) {
        path[used + index] = text[index];
    }
    return true;
}

static void append_hex32(char *path, size_t capacity, uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    size_t used = string_length(path, capacity);

    if (used >= capacity || 8U >= capacity - used) {
        if (capacity != 0U) {
            path[capacity - 1U] = '\0';
        }
        return;
    }
    for (size_t index = 0U; index < 8U; ++index) {
        path[used + index] = digits[(value >> ((7U - index) * 4U)) & 0xfU];
    }
    path[used + 8U] = '\0';
}

static bool generation_path(
    uint64_t generation,
    const char *suffix,
    char path[SAPFS_MAX_PATH]
)
{
    zero_bytes(path, SAPFS_MAX_PATH);
    if (!append_text(path, SAPFS_MAX_PATH, GENERATION_PREFIX)) {
        return false;
    }
    append_hex32(path, SAPFS_MAX_PATH, (uint32_t)(generation >> 32U));
    if (string_length(path, SAPFS_MAX_PATH) >= SAPFS_MAX_PATH ||
        !append_text(path, SAPFS_MAX_PATH, "/")) {
        return false;
    }
    append_hex32(path, SAPFS_MAX_PATH, (uint32_t)generation);
    return string_length(path, SAPFS_MAX_PATH) < SAPFS_MAX_PATH &&
        append_text(path, SAPFS_MAX_PATH, suffix);
}

static bool append_span(
    char *path,
    size_t capacity,
    const uint8_t *bytes,
    size_t count
)
{
    size_t used = string_length(path, capacity);

    if ((bytes == NULL && count != 0U) || used >= capacity ||
        count >= capacity - used) {
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        path[used + index] = (char)bytes[index];
    }
    path[used + count] = '\0';
    return true;
}

static bool generation_file_path(
    uint64_t generation,
    const struct package_state_text *relative,
    char path[SAPFS_MAX_PATH]
)
{
    return relative != NULL &&
        generation_path(generation, GENERATION_ROOT_SUFFIX, path) &&
        append_text(path, SAPFS_MAX_PATH, "/") &&
        append_span(path, SAPFS_MAX_PATH, relative->bytes, relative->length);
}

static enum package_service_status filesystem_failure(
    struct service_context *context,
    enum sapfs_status status
)
{
    context->report->filesystem_status = status;
    return PACKAGE_SERVICE_STATUS_FILESYSTEM;
}

static void allocation_acquired(struct service_context *context)
{
    ++context->report->live_allocations;
    if (context->report->live_allocations >
            context->report->peak_allocations) {
        context->report->peak_allocations =
            context->report->live_allocations;
    }
}

static enum package_service_status allocate_bytes(
    struct service_context *context,
    uint64_t count,
    void **pointer
)
{
    if (heap_allocate(count, pointer) != HEAP_STATUS_OK) {
        return PACKAGE_SERVICE_STATUS_RESOURCE;
    }
    allocation_acquired(context);
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status release_bytes(
    struct service_context *context,
    void **pointer
)
{
    if (pointer == NULL || *pointer == NULL) {
        return PACKAGE_SERVICE_STATUS_OK;
    }
    if (heap_free(*pointer) != HEAP_STATUS_OK) {
        return PACKAGE_SERVICE_STATUS_RESOURCE;
    }
    *pointer = NULL;
    --context->report->live_allocations;
    return PACKAGE_SERVICE_STATUS_OK;
}

static void handle_acquired(struct service_context *context)
{
    ++context->report->live_file_handles;
    if (context->report->live_file_handles >
            context->report->peak_file_handles) {
        context->report->peak_file_handles =
            context->report->live_file_handles;
    }
}

static enum package_service_status close_file(
    struct service_context *context,
    sapfs_handle handle
)
{
    enum sapfs_status status = sapfs_close(handle);

    --context->report->live_file_handles;
    return status == SAPFS_STATUS_OK ? PACKAGE_SERVICE_STATUS_OK :
        filesystem_failure(context, status);
}

static enum package_service_status read_open_file(
    struct service_context *context,
    sapfs_handle handle,
    uint8_t *destination,
    size_t count
)
{
    size_t total = 0U;

    while (total < count) {
        size_t read_bytes = 0U;
        enum sapfs_status status = sapfs_read(handle, destination + total,
            count - total, &read_bytes);

        if (status != SAPFS_STATUS_OK) {
            return filesystem_failure(context, status);
        }
        if (read_bytes == 0U || read_bytes > count - total) {
            return PACKAGE_SERVICE_STATUS_STATE;
        }
        total += read_bytes;
        context->report->bytes_read += read_bytes;
    }
    uint8_t extra;
    size_t extra_bytes = 0U;
    enum sapfs_status status = sapfs_read(handle, &extra, 1U, &extra_bytes);

    if (status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, status);
    }
    if (extra_bytes != 0U) {
        return PACKAGE_SERVICE_STATUS_STATE;
    }
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status read_exact_path(
    struct service_context *context,
    const char *path,
    uint8_t *destination,
    size_t expected,
    bool optional,
    bool *present
)
{
    struct sapfs_stat stat;
    sapfs_handle handle;
    enum sapfs_status fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA, path,
        &stat);

    *present = false;
    if (fs_status == SAPFS_STATUS_NOT_FOUND && optional) {
        return PACKAGE_SERVICE_STATUS_OK;
    }
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    *present = true;
    if (stat.directory || stat.size != (uint64_t)expected) {
        return PACKAGE_SERVICE_STATUS_STATE;
    }
    fs_status = sapfs_open(SAPFS_VOLUME_DATA, path, SAPFS_ACCESS_READ, &handle);
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    handle_acquired(context);
    enum package_service_status status = read_open_file(context, handle,
        destination, expected);
    enum package_service_status close_status = close_file(context, handle);

    return status != PACKAGE_SERVICE_STATUS_OK ? status : close_status;
}

static bool entry_name_valid(const char *name)
{
    size_t length = string_length(name, SAPFS_MAX_COMPONENT_BYTES);

    if (length == 0U || length >= SAPFS_MAX_COMPONENT_BYTES ||
        (length == 1U && name[0] == '.') ||
        (length == 2U && name[0] == '.' && name[1] == '.')) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        uint8_t byte = (uint8_t)name[index];

        if (byte < UINT8_C(0x20) || byte > UINT8_C(0x7e) ||
            byte == (uint8_t)'/' || byte == (uint8_t)'\\') {
            return false;
        }
    }
    return true;
}

static bool child_path(
    const char *parent,
    const char *name,
    char result[SAPFS_MAX_PATH]
)
{
    zero_bytes(result, SAPFS_MAX_PATH);
    return append_text(result, SAPFS_MAX_PATH, parent) &&
        append_text(result, SAPFS_MAX_PATH, "/") &&
        append_text(result, SAPFS_MAX_PATH, name);
}

static enum package_service_status count_tree_files(
    struct service_context *context,
    const char *path,
    uint32_t depth,
    uint32_t *file_count
)
{
    size_t count = 0U;
    enum sapfs_status fs_status;

    if (depth >= SAPFS_MAX_DEPTH) {
        return PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    fs_status = sapfs_list(SAPFS_VOLUME_DATA, path, context->entries,
        SAPFS_MAX_LIST_ENTRIES, &count);
    if (fs_status == SAPFS_STATUS_DIRECTORY_FULL) {
        return PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    for (size_t index = 0U; index < count; ++index) {
        size_t refreshed = 0U;

        fs_status = sapfs_list(SAPFS_VOLUME_DATA, path, context->entries,
            SAPFS_MAX_LIST_ENTRIES, &refreshed);
        if (fs_status != SAPFS_STATUS_OK) {
            return fs_status == SAPFS_STATUS_DIRECTORY_FULL ?
                PACKAGE_SERVICE_STATUS_NAMESPACE :
                filesystem_failure(context, fs_status);
        }
        if (refreshed != count) {
            return PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
        }
        struct sapfs_list_entry entry = context->entries[index];
        char child[SAPFS_MAX_PATH];

        if (!entry_name_valid(entry.name) ||
            ++context->walk_entries > PACKAGE_SERVICE_MAX_TREE_ENTRIES ||
            !child_path(path, entry.name, child)) {
            return PACKAGE_SERVICE_STATUS_NAMESPACE;
        }
        if (entry.directory) {
            enum package_service_status status = count_tree_files(context,
                child, depth + 1U, file_count);

            if (status != PACKAGE_SERVICE_STATUS_OK) {
                return status;
            }
        } else {
            if (*file_count == UINT32_MAX) {
                return PACKAGE_SERVICE_STATUS_NAMESPACE;
            }
            ++*file_count;
        }
    }
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status verify_file(
    struct service_context *context,
    const char *path,
    const uint8_t *record
)
{
    struct sapfs_stat before;
    struct sapfs_stat after;
    struct package_state_sha256_context sha;
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    uint8_t buffer[PACKAGE_SERVICE_IO_BYTES];
    uint64_t remaining = read_u64(record + FILE_LENGTH_OFFSET);
    uint32_t mode = read_u32(record + FILE_MODE_OFFSET);
    sapfs_handle handle;
    enum sapfs_status fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA, path,
        &before);

    if (fs_status != SAPFS_STATUS_OK) {
        return fs_status == SAPFS_STATUS_NOT_FOUND ?
            PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE :
            filesystem_failure(context, fs_status);
    }
    if (before.directory || before.size != remaining || before.links > 1U ||
        (before.mode != 0U && (before.mode & UINT16_C(0777)) != mode)) {
        return PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
    }
    fs_status = sapfs_open(SAPFS_VOLUME_DATA, path, SAPFS_ACCESS_READ, &handle);
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    handle_acquired(context);
    enum package_state_status state_status =
        package_state_sha256_initialize(&sha);
    enum package_service_status status = PACKAGE_SERVICE_STATUS_OK;

    while (state_status == PACKAGE_STATE_STATUS_OK && remaining != 0U) {
        size_t requested = remaining < sizeof(buffer) ? (size_t)remaining :
            sizeof(buffer);
        size_t read_bytes = 0U;

        fs_status = sapfs_read(handle, buffer, requested, &read_bytes);
        if (fs_status != SAPFS_STATUS_OK) {
            status = filesystem_failure(context, fs_status);
            break;
        }
        if (read_bytes == 0U || read_bytes > requested) {
            status = PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
            break;
        }
        context->report->bytes_read += read_bytes;
        remaining -= read_bytes;
        state_status = package_state_sha256_update(&sha, buffer, read_bytes);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK &&
            state_status == PACKAGE_STATE_STATUS_OK) {
        size_t extra_bytes = 0U;

        fs_status = sapfs_read(handle, buffer, 1U, &extra_bytes);
        if (fs_status != SAPFS_STATUS_OK) {
            status = filesystem_failure(context, fs_status);
        } else if (extra_bytes != 0U) {
            status = PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
        }
    }
    enum package_service_status close_status = close_file(context, handle);
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = close_status;
    }
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return status;
    }
    if (state_status != PACKAGE_STATE_STATUS_OK || remaining != 0U ||
        package_state_sha256_finish(&sha, digest) != PACKAGE_STATE_STATUS_OK ||
        !equal_bytes(digest, record + FILE_DIGEST_OFFSET, sizeof(digest))) {
        return PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
    }
    fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA, path, &after);
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    if (after.directory || after.size != before.size ||
        after.object_id != before.object_id || after.links != before.links ||
        after.mode != before.mode) {
        return PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
    }
    ++context->report->files_verified;
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status verify_generation_files(
    struct service_context *context,
    const struct package_state_database_view *view
)
{
    char root[SAPFS_MAX_PATH];
    uint32_t actual_files = 0U;

    if (!generation_path(view->generation, GENERATION_ROOT_SUFFIX, root)) {
        return PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    context->walk_entries = 0U;
    enum package_service_status status = count_tree_files(context, root, 0U,
        &actual_files);
    context->report->tree_entries += context->walk_entries;
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return status == PACKAGE_SERVICE_STATUS_FILESYSTEM &&
            context->report->filesystem_status == SAPFS_STATUS_NOT_FOUND ?
            PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE : status;
    }
    if (actual_files != view->file_count) {
        return PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
    }
    for (uint32_t index = 0U; index < view->file_count; ++index) {
        const uint8_t *record = view->bytes + view->file_offset +
            (size_t)index * PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES;
        char path[SAPFS_MAX_PATH];
        char relative[FILE_PATH_BYTES + 1U];
        size_t length = 0U;

        while (length < FILE_PATH_BYTES && record[length] != 0U) {
            relative[length] = (char)record[length];
            ++length;
        }
        if (length == FILE_PATH_BYTES) {
            return PACKAGE_SERVICE_STATUS_STATE;
        }
        relative[length] = '\0';
        if (!generation_path(view->generation, GENERATION_ROOT_SUFFIX, path) ||
            !append_text(path, SAPFS_MAX_PATH, "/") ||
            !append_text(path, SAPFS_MAX_PATH, relative)) {
            return PACKAGE_SERVICE_STATUS_NAMESPACE;
        }
        status = verify_file(context, path, record);
        if (status != PACKAGE_SERVICE_STATUS_OK) {
            return status;
        }
    }
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status load_generation(
    struct service_context *context,
    uint64_t generation,
    uint64_t expected_bytes,
    struct loaded_generation *loaded
)
{
    char path[SAPFS_MAX_PATH];
    struct sapfs_stat stat;
    enum sapfs_status fs_status;

    zero_bytes(loaded, sizeof(*loaded));
    if (expected_bytes > PACKAGE_SERVICE_MAX_DATABASE_BYTES ||
        !generation_path(generation, GENERATION_DATABASE_SUFFIX, path)) {
        return PACKAGE_SERVICE_STATUS_RESOURCE;
    }
    fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA, path, &stat);
    if (fs_status == SAPFS_STATUS_NOT_FOUND) {
        return PACKAGE_SERVICE_STATUS_OK;
    }
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    loaded->present = true;
    if (stat.directory || stat.size != expected_bytes) {
        return PACKAGE_SERVICE_STATUS_OK;
    }
    void *database = NULL;
    enum package_service_status status = allocate_bytes(context,
        expected_bytes, &database);

    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return status;
    }
    bool present = false;
    status = read_exact_path(context, path, (uint8_t *)database,
        (size_t)expected_bytes, false, &present);
    (void)present;
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        enum package_service_status release_status = release_bytes(context,
            &database);
        return release_status == PACKAGE_SERVICE_STATUS_OK ? status :
            release_status;
    }
    loaded->candidate.database = (const uint8_t *)database;
    loaded->candidate.database_bytes = (size_t)expected_bytes;
    context->report->state_status = package_state_database_parse(
        loaded->candidate.database, loaded->candidate.database_bytes,
        &loaded->view);
    if (context->report->state_status != PACKAGE_STATE_STATUS_OK ||
        loaded->view.generation != generation) {
        return PACKAGE_SERVICE_STATUS_OK;
    }
    status = verify_generation_files(context, &loaded->view);
    if (status == PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE ||
        status == PACKAGE_SERVICE_STATUS_NAMESPACE) {
        return PACKAGE_SERVICE_STATUS_OK;
    }
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return status;
    }
    loaded->candidate.owned_files_complete = true;
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status release_generation(
    struct service_context *context,
    struct loaded_generation *loaded
)
{
    void *database = (void *)loaded->candidate.database;
    enum package_service_status status = release_bytes(context, &database);

    zero_bytes(loaded, sizeof(*loaded));
    return status;
}

static enum package_service_status ensure_entries(
    struct service_context *context
)
{
    if (context->entries != NULL) {
        return PACKAGE_SERVICE_STATUS_OK;
    }
    return allocate_bytes(context,
        sizeof(struct sapfs_list_entry) * SAPFS_MAX_LIST_ENTRIES,
        (void **)&context->entries);
}

static enum package_service_status sync_data(struct service_context *context)
{
    enum sapfs_status status = sapfs_sync(SAPFS_VOLUME_DATA);

    if (status != SAPFS_STATUS_OK) {
        context->report->filesystem_status = status;
        return PACKAGE_SERVICE_STATUS_DURABILITY;
    }
    ++context->report->sync_count;
    return PACKAGE_SERVICE_STATUS_OK;
}

static bool same_text(
    const struct package_state_text *left,
    const struct package_state_text *right
)
{
    return left->length == right->length &&
        (left->length == 0U || equal_bytes(left->bytes, right->bytes,
            left->length));
}

static enum package_service_status ensure_directory(
    struct service_context *context,
    const char *path
)
{
    struct sapfs_stat stat;
    enum sapfs_status fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA, path,
        &stat);

    if (fs_status == SAPFS_STATUS_OK) {
        return stat.directory ? PACKAGE_SERVICE_STATUS_OK :
            PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    if (fs_status != SAPFS_STATUS_NOT_FOUND) {
        return filesystem_failure(context, fs_status);
    }
    fs_status = sapfs_mkdir(SAPFS_VOLUME_DATA, path);
    return fs_status == SAPFS_STATUS_OK ? PACKAGE_SERVICE_STATUS_OK :
        filesystem_failure(context, fs_status);
}

static enum package_service_status ensure_generation_layout(
    struct service_context *context,
    uint64_t generation
)
{
    char high[SAPFS_MAX_PATH];
    char generation_directory[SAPFS_MAX_PATH];
    char root[SAPFS_MAX_PATH];
    struct sapfs_stat stat;

    zero_bytes(high, sizeof(high));
    if (!append_text(high, sizeof(high), GENERATION_PREFIX)) {
        return PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    append_hex32(high, sizeof(high), (uint32_t)(generation >> 32U));
    if (string_length(high, sizeof(high)) >= sizeof(high) ||
        !generation_path(generation, "", generation_directory) ||
        !generation_path(generation, GENERATION_ROOT_SUFFIX, root)) {
        return PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    enum sapfs_status fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA,
        generation_directory, &stat);
    if (fs_status == SAPFS_STATUS_OK) {
        return PACKAGE_SERVICE_STATUS_STATE;
    }
    if (fs_status != SAPFS_STATUS_NOT_FOUND) {
        return filesystem_failure(context, fs_status);
    }
    enum package_service_status status = ensure_directory(context,
        PACKAGE_SERVICE_STATE_DIRECTORY);
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = ensure_directory(context, "pkgstate/gen");
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = ensure_directory(context, high);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = ensure_directory(context, generation_directory);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = ensure_directory(context, root);
    }
    return status;
}

static enum package_service_status ensure_file_parents(
    struct service_context *context,
    uint64_t generation,
    const struct package_state_text *relative
)
{
    char root[SAPFS_MAX_PATH];

    if (relative == NULL ||
        !generation_path(generation, GENERATION_ROOT_SUFFIX, root)) {
        return PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    for (size_t index = 0U; index < relative->length; ++index) {
        if (relative->bytes[index] == (uint8_t)'/') {
            char parent[SAPFS_MAX_PATH];

            zero_bytes(parent, sizeof(parent));
            if (!append_text(parent, sizeof(parent), root) ||
                !append_text(parent, sizeof(parent), "/") ||
                !append_span(parent, sizeof(parent), relative->bytes, index)) {
                return PACKAGE_SERVICE_STATUS_NAMESPACE;
            }
            enum package_service_status status = ensure_directory(context,
                parent);
            if (status != PACKAGE_SERVICE_STATUS_OK) {
                return status;
            }
        }
    }
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status write_handle_bytes(
    struct service_context *context,
    sapfs_handle handle,
    const uint8_t *bytes,
    size_t count
)
{
    size_t total = 0U;

    while (total < count) {
        size_t chunk = count - total < PACKAGE_SERVICE_IO_BYTES ?
            count - total : PACKAGE_SERVICE_IO_BYTES;
        size_t written = 0U;
        enum sapfs_status fs_status = sapfs_write(handle, bytes + total,
            chunk, &written);

        if (fs_status != SAPFS_STATUS_OK) {
            return filesystem_failure(context, fs_status);
        }
        if (written == 0U || written > chunk) {
            return PACKAGE_SERVICE_STATUS_FILESYSTEM;
        }
        total += written;
        context->report->bytes_written += written;
    }
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status write_new_file(
    struct service_context *context,
    const char *path,
    const uint8_t *bytes,
    size_t count
)
{
    sapfs_handle handle;
    enum sapfs_status fs_status;

    if (bytes == NULL && count != 0U) {
        return PACKAGE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    fs_status = sapfs_create(SAPFS_VOLUME_DATA, path);
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    fs_status = sapfs_open(SAPFS_VOLUME_DATA, path, SAPFS_ACCESS_WRITE,
        &handle);
    if (fs_status != SAPFS_STATUS_OK) {
        (void)sapfs_unlink(SAPFS_VOLUME_DATA, path);
        return filesystem_failure(context, fs_status);
    }
    handle_acquired(context);
    enum package_service_status status = write_handle_bytes(context, handle,
        bytes, count);
    enum package_service_status close_status = close_file(context, handle);

    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = close_status;
    }
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        (void)sapfs_unlink(SAPFS_VOLUME_DATA, path);
    }
    return status;
}

static bool same_file_metadata(
    const struct package_state_file_view *source,
    const struct package_generation_file *target
)
{
    return same_text(&source->path, &target->path) &&
        source->kind == target->kind && source->mode == target->mode &&
        source->length == target->length &&
        equal_bytes(source->sha256, target->sha256,
            PACKAGE_STATE_SHA256_BYTES) &&
        same_text(&source->soname, &target->soname);
}

static bool path_components(
    const struct package_state_text *path,
    uint8_t starts[SAPFS_MAX_DEPTH],
    uint8_t lengths[SAPFS_MAX_DEPTH],
    uint32_t *count
)
{
    size_t start = 0U;

    *count = 0U;
    for (size_t index = 0U; index <= path->length; ++index) {
        if (index != path->length && path->bytes[index] != (uint8_t)'/') {
            continue;
        }
        if (index == start || index - start > UINT8_MAX ||
            *count == SAPFS_MAX_DEPTH) {
            return false;
        }
        starts[*count] = (uint8_t)start;
        lengths[*count] = (uint8_t)(index - start);
        ++*count;
        start = index + 1U;
    }
    return *count != 0U;
}

static bool staging_namespace_bounded(
    const struct package_generation_spec *spec
)
{
    uint16_t child_counts[SAPFS_MAX_DEPTH] = { 0U };
    uint8_t previous_starts[SAPFS_MAX_DEPTH];
    uint8_t previous_lengths[SAPFS_MAX_DEPTH];
    uint32_t previous_count = 0U;

    for (uint32_t index = 0U; index < spec->file_count; ++index) {
        uint8_t starts[SAPFS_MAX_DEPTH];
        uint8_t lengths[SAPFS_MAX_DEPTH];
        char full_path[SAPFS_MAX_PATH];
        uint32_t count;
        uint32_t common = 0U;

        if (!path_components(&spec->files[index].path, starts, lengths,
                &count) || count > SAPFS_MAX_DEPTH - 5U ||
            !generation_file_path(spec->generation, &spec->files[index].path,
                full_path)) {
            return false;
        }
        if (index != 0U) {
            const struct package_state_text *previous =
                &spec->files[index - 1U].path;

            while (common < previous_count && common < count &&
                previous_lengths[common] == lengths[common] &&
                equal_bytes(previous->bytes + previous_starts[common],
                    spec->files[index].path.bytes + starts[common],
                    lengths[common])) {
                ++common;
            }
            if (common == previous_count || common == count) {
                return false;
            }
        }
        if (index == 0U) {
            for (uint32_t depth = 0U; depth < count; ++depth) {
                child_counts[depth] = 1U;
            }
        } else {
            if (++child_counts[common] > SAPFS_MAX_LIST_ENTRIES) {
                return false;
            }
            for (uint32_t depth = common + 1U; depth < count; ++depth) {
                child_counts[depth] = 1U;
            }
        }
        for (uint32_t depth = 0U; depth < count; ++depth) {
            previous_starts[depth] = starts[depth];
            previous_lengths[depth] = lengths[depth];
        }
        previous_count = count;
    }
    return true;
}

static enum package_service_status write_generation_file(
    struct service_context *context,
    const struct package_builder_workspace *builder,
    const struct package_state_database_view *base,
    uint32_t index
)
{
    const struct package_generation_file *file = &builder->files[index];
    const struct package_builder_file_source *source =
        &builder->file_sources[index];
    struct package_state_sha256_context sha;
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    char destination[SAPFS_MAX_PATH];
    sapfs_handle output;
    sapfs_handle input = 0U;
    uint64_t remaining = file->length;
    enum package_service_status status;
    enum sapfs_status fs_status;

    status = ensure_file_parents(context, builder->spec.generation,
        &file->path);
    if (status != PACKAGE_SERVICE_STATUS_OK || !generation_file_path(
            builder->spec.generation, &file->path, destination)) {
        return status != PACKAGE_SERVICE_STATUS_OK ? status :
            PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    if (source->kind == PACKAGE_BUILDER_FILE_SOURCE_PAYLOAD) {
        if (source->payload == NULL || source->payload_bytes != file->length) {
            return PACKAGE_SERVICE_STATUS_STATE;
        }
    } else if (source->kind == PACKAGE_BUILDER_FILE_SOURCE_INSTALLED) {
        struct package_state_file_view old_file;
        char old_path[SAPFS_MAX_PATH];

        if (base == NULL || package_state_database_file(base,
                source->file_index, &old_file) != PACKAGE_STATE_STATUS_OK ||
            old_file.owner_index != source->package_index ||
            !same_file_metadata(&old_file, file) ||
            !generation_file_path(base->generation,
                &old_file.path, old_path)) {
            return PACKAGE_SERVICE_STATUS_STATE;
        }
        fs_status = sapfs_open(SAPFS_VOLUME_DATA, old_path, SAPFS_ACCESS_READ,
            &input);
        if (fs_status != SAPFS_STATUS_OK) {
            return filesystem_failure(context, fs_status);
        }
        handle_acquired(context);
    } else {
        return PACKAGE_SERVICE_STATUS_STATE;
    }
    fs_status = sapfs_create(SAPFS_VOLUME_DATA, destination);
    if (fs_status != SAPFS_STATUS_OK) {
        status = filesystem_failure(context, fs_status);
        goto close_input;
    }
    fs_status = sapfs_open(SAPFS_VOLUME_DATA, destination,
        SAPFS_ACCESS_WRITE, &output);
    if (fs_status != SAPFS_STATUS_OK) {
        (void)sapfs_unlink(SAPFS_VOLUME_DATA, destination);
        status = filesystem_failure(context, fs_status);
        goto close_input;
    }
    handle_acquired(context);
    if (package_state_sha256_initialize(&sha) != PACKAGE_STATE_STATUS_OK) {
        status = PACKAGE_SERVICE_STATUS_STATE;
        goto close_output;
    }
    status = PACKAGE_SERVICE_STATUS_OK;
    while (remaining != 0U) {
        uint8_t buffer[PACKAGE_SERVICE_IO_BYTES];
        size_t chunk = remaining < sizeof(buffer) ? (size_t)remaining :
            sizeof(buffer);
        const uint8_t *bytes = source->payload == NULL ? buffer :
            source->payload + (file->length - remaining);

        if (source->payload == NULL) {
            size_t read_bytes = 0U;

            fs_status = sapfs_read(input, buffer, chunk, &read_bytes);
            if (fs_status != SAPFS_STATUS_OK) {
                status = filesystem_failure(context, fs_status);
                break;
            }
            if (read_bytes != chunk) {
                status = PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
                break;
            }
            context->report->bytes_read += read_bytes;
        }
        if (package_state_sha256_update(&sha, bytes, chunk) !=
                PACKAGE_STATE_STATUS_OK) {
            status = PACKAGE_SERVICE_STATUS_STATE;
            break;
        }
        status = write_handle_bytes(context, output, bytes, chunk);
        if (status != PACKAGE_SERVICE_STATUS_OK) {
            break;
        }
        remaining -= chunk;
    }
    if (status == PACKAGE_SERVICE_STATUS_OK && source->payload == NULL) {
        uint8_t extra;
        size_t extra_bytes = 0U;

        fs_status = sapfs_read(input, &extra, 1U, &extra_bytes);
        if (fs_status != SAPFS_STATUS_OK) {
            status = filesystem_failure(context, fs_status);
        } else if (extra_bytes != 0U) {
            status = PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
        }
    }
    if (status == PACKAGE_SERVICE_STATUS_OK &&
        package_state_sha256_finish(&sha, digest) != PACKAGE_STATE_STATUS_OK) {
        status = PACKAGE_SERVICE_STATUS_STATE;
    }
    if (status == PACKAGE_SERVICE_STATUS_OK &&
        !equal_bytes(digest, file->sha256, sizeof(digest))) {
        status = PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE;
    }
close_output:
    {
        enum package_service_status close_status = close_file(context, output);

        if (status == PACKAGE_SERVICE_STATUS_OK) {
            status = close_status;
        }
    }
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        (void)sapfs_unlink(SAPFS_VOLUME_DATA, destination);
    }
close_input:
    if (input != 0U) {
        enum package_service_status close_status = close_file(context, input);

        if (status == PACKAGE_SERVICE_STATUS_OK) {
            status = close_status;
        }
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        ++context->report->files_staged;
    }
    return status;
}

static enum package_service_status write_authority_file(
    struct service_context *context,
    const uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    enum sapfs_status fs_status = sapfs_unlink(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_AUTHORITY_NEW_PATH);

    if (fs_status != SAPFS_STATUS_OK && fs_status != SAPFS_STATUS_NOT_FOUND) {
        return filesystem_failure(context, fs_status);
    }
    fs_status = sapfs_create(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_AUTHORITY_NEW_PATH);
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    sapfs_handle handle;
    fs_status = sapfs_open(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_AUTHORITY_NEW_PATH, SAPFS_ACCESS_WRITE, &handle);
    if (fs_status != SAPFS_STATUS_OK) {
        (void)sapfs_unlink(SAPFS_VOLUME_DATA,
            PACKAGE_SERVICE_AUTHORITY_NEW_PATH);
        return filesystem_failure(context, fs_status);
    }
    handle_acquired(context);
    size_t total = 0U;
    enum package_service_status status = PACKAGE_SERVICE_STATUS_OK;

    while (total < PACKAGE_STATE_AUTHORITY_BYTES) {
        size_t written = 0U;

        fs_status = sapfs_write(handle, authority + total,
            PACKAGE_STATE_AUTHORITY_BYTES - total, &written);
        if (fs_status != SAPFS_STATUS_OK) {
            status = filesystem_failure(context, fs_status);
            break;
        }
        if (written == 0U ||
            written > PACKAGE_STATE_AUTHORITY_BYTES - total) {
            status = PACKAGE_SERVICE_STATUS_FILESYSTEM;
            break;
        }
        total += written;
    }
    enum package_service_status close_status = close_file(context, handle);
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = close_status;
    }
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        (void)sapfs_unlink(SAPFS_VOLUME_DATA,
            PACKAGE_SERVICE_AUTHORITY_NEW_PATH);
        return status;
    }
    return sync_data(context);
}

static enum package_service_status replace_authority(
    struct service_context *context,
    const uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct sapfs_stat stat;
    enum package_service_status status = write_authority_file(context,
        authority);
    bool had_current;

    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return status;
    }
    enum sapfs_status fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_AUTHORITY_PATH, &stat);
    had_current = fs_status == SAPFS_STATUS_OK;
    if ((had_current && stat.directory) ||
        (fs_status != SAPFS_STATUS_OK && fs_status != SAPFS_STATUS_NOT_FOUND)) {
        return fs_status == SAPFS_STATUS_OK ? PACKAGE_SERVICE_STATUS_STATE :
            filesystem_failure(context, fs_status);
    }
    if (had_current) {
        fs_status = sapfs_unlink(SAPFS_VOLUME_DATA,
            PACKAGE_SERVICE_AUTHORITY_OLD_PATH);
        if (fs_status != SAPFS_STATUS_OK && fs_status != SAPFS_STATUS_NOT_FOUND) {
            return filesystem_failure(context, fs_status);
        }
        fs_status = sapfs_rename(SAPFS_VOLUME_DATA,
            PACKAGE_SERVICE_AUTHORITY_PATH,
            PACKAGE_SERVICE_AUTHORITY_OLD_PATH);
        if (fs_status != SAPFS_STATUS_OK) {
            return filesystem_failure(context, fs_status);
        }
        ++context->report->rename_count;
        status = sync_data(context);
        if (status != PACKAGE_SERVICE_STATUS_OK) {
            (void)sapfs_rename(SAPFS_VOLUME_DATA,
                PACKAGE_SERVICE_AUTHORITY_OLD_PATH,
                PACKAGE_SERVICE_AUTHORITY_PATH);
            (void)sapfs_sync(SAPFS_VOLUME_DATA);
            return status;
        }
    }
    fs_status = sapfs_rename(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_AUTHORITY_NEW_PATH,
        PACKAGE_SERVICE_AUTHORITY_PATH);
    if (fs_status != SAPFS_STATUS_OK) {
        if (had_current) {
            (void)sapfs_rename(SAPFS_VOLUME_DATA,
                PACKAGE_SERVICE_AUTHORITY_OLD_PATH,
                PACKAGE_SERVICE_AUTHORITY_PATH);
            (void)sapfs_sync(SAPFS_VOLUME_DATA);
        }
        return filesystem_failure(context, fs_status);
    }
    ++context->report->rename_count;
    status = sync_data(context);
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        if (had_current) {
            (void)sapfs_rename(SAPFS_VOLUME_DATA,
                PACKAGE_SERVICE_AUTHORITY_PATH,
                PACKAGE_SERVICE_AUTHORITY_NEW_PATH);
            (void)sapfs_rename(SAPFS_VOLUME_DATA,
                PACKAGE_SERVICE_AUTHORITY_OLD_PATH,
                PACKAGE_SERVICE_AUTHORITY_PATH);
            (void)sapfs_sync(SAPFS_VOLUME_DATA);
        }
        return status;
    }
    context->report->authority_replaced = true;
    return PACKAGE_SERVICE_STATUS_OK;
}

static enum package_service_status remove_tree(
    struct service_context *context,
    const char *path,
    uint32_t depth
)
{
    struct sapfs_stat stat;
    enum sapfs_status fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA, path,
        &stat);

    if (fs_status == SAPFS_STATUS_NOT_FOUND) {
        return PACKAGE_SERVICE_STATUS_OK;
    }
    if (fs_status != SAPFS_STATUS_OK) {
        return filesystem_failure(context, fs_status);
    }
    if (!stat.directory) {
        fs_status = sapfs_unlink(SAPFS_VOLUME_DATA, path);
        return fs_status == SAPFS_STATUS_OK ? PACKAGE_SERVICE_STATUS_OK :
            filesystem_failure(context, fs_status);
    }
    if (depth >= SAPFS_MAX_DEPTH) {
        return PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    for (;;) {
        size_t count = 0U;
        fs_status = sapfs_list(SAPFS_VOLUME_DATA, path, context->entries,
            SAPFS_MAX_LIST_ENTRIES, &count);
        if (fs_status == SAPFS_STATUS_DIRECTORY_FULL) {
            return PACKAGE_SERVICE_STATUS_NAMESPACE;
        }
        if (fs_status != SAPFS_STATUS_OK) {
            return filesystem_failure(context, fs_status);
        }
        if (count == 0U) {
            break;
        }
        struct sapfs_list_entry entry = context->entries[0];
        char child[SAPFS_MAX_PATH];

        if (!entry_name_valid(entry.name) ||
            ++context->walk_entries > PACKAGE_SERVICE_MAX_TREE_ENTRIES ||
            !child_path(path, entry.name, child)) {
            return PACKAGE_SERVICE_STATUS_NAMESPACE;
        }
        enum package_service_status status = remove_tree(context, child,
            depth + 1U);

        if (status != PACKAGE_SERVICE_STATUS_OK) {
            return status;
        }
    }
    fs_status = sapfs_rmdir(SAPFS_VOLUME_DATA, path);
    return fs_status == SAPFS_STATUS_OK ? PACKAGE_SERVICE_STATUS_OK :
        filesystem_failure(context, fs_status);
}

static enum package_service_status cleanup_transaction(
    struct service_context *context,
    uint64_t discarded_generation
)
{
    char path[SAPFS_MAX_PATH];

    if (!generation_path(discarded_generation, "", path)) {
        return PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    context->walk_entries = 0U;
    enum package_service_status status = remove_tree(context, path, 0U);
    context->report->tree_entries += context->walk_entries;
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return PACKAGE_SERVICE_STATUS_CLEANUP;
    }
    enum sapfs_status fs_status = sapfs_unlink(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_AUTHORITY_OLD_PATH);
    if (fs_status != SAPFS_STATUS_OK && fs_status != SAPFS_STATUS_NOT_FOUND) {
        context->report->filesystem_status = fs_status;
        return PACKAGE_SERVICE_STATUS_CLEANUP;
    }
    fs_status = sapfs_unlink(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_AUTHORITY_NEW_PATH);
    if (fs_status != SAPFS_STATUS_OK && fs_status != SAPFS_STATUS_NOT_FOUND) {
        context->report->filesystem_status = fs_status;
        return PACKAGE_SERVICE_STATUS_CLEANUP;
    }
    fs_status = sapfs_unlink(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_JOURNAL_PATH);
    if (fs_status != SAPFS_STATUS_OK && fs_status != SAPFS_STATUS_NOT_FOUND) {
        context->report->filesystem_status = fs_status;
        return PACKAGE_SERVICE_STATUS_CLEANUP;
    }
    status = sync_data(context);
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return PACKAGE_SERVICE_STATUS_CLEANUP;
    }
    context->report->cleanup_complete = true;
    return PACKAGE_SERVICE_STATUS_OK;
}

static bool authority_selects(
    const uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const struct package_state_database_view *database
)
{
    struct package_state_authority_view view;
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

    return package_state_authority_parse(authority,
        PACKAGE_STATE_AUTHORITY_BYTES, &view) == PACKAGE_STATE_STATUS_OK &&
        view.generation == database->generation &&
        view.database_bytes == database->byte_count &&
        package_state_sha256(database->bytes, database->byte_count, digest) ==
            PACKAGE_STATE_STATUS_OK &&
        equal_bytes(view.database_sha256, digest, sizeof(digest));
}

static bool fixed_path_absent(
    struct service_context *context,
    const char *path,
    enum package_service_status *status
)
{
    struct sapfs_stat stat;
    enum sapfs_status fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA, path,
        &stat);

    if (fs_status == SAPFS_STATUS_NOT_FOUND) {
        return true;
    }
    *status = fs_status == SAPFS_STATUS_OK ? PACKAGE_SERVICE_STATUS_STATE :
        filesystem_failure(context, fs_status);
    return false;
}

static enum package_state_operation plan_operation(
    enum package_manager_plan_operation operation
)
{
    switch (operation) {
    case PACKAGE_MANAGER_PLAN_INSTALL:
        return PACKAGE_STATE_OPERATION_INSTALL;
    case PACKAGE_MANAGER_PLAN_UPDATE:
        return PACKAGE_STATE_OPERATION_UPDATE;
    case PACKAGE_MANAGER_PLAN_REMOVE:
        return PACKAGE_STATE_OPERATION_REMOVE;
    default:
        return PACKAGE_STATE_OPERATION_INVALID;
    }
}

static enum package_service_status cleanup_unpublished_prepare(
    struct service_context *context,
    uint64_t generation
)
{
    char path[SAPFS_MAX_PATH];
    enum package_service_status status = ensure_entries(context);

    if (status != PACKAGE_SERVICE_STATUS_OK ||
        !generation_path(generation, "", path)) {
        return PACKAGE_SERVICE_STATUS_CLEANUP;
    }
    context->walk_entries = 0U;
    status = remove_tree(context, path, 0U);
    context->report->tree_entries += context->walk_entries;
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return PACKAGE_SERVICE_STATUS_CLEANUP;
    }
    enum sapfs_status fs_status = sapfs_unlink(SAPFS_VOLUME_DATA,
        PACKAGE_SERVICE_JOURNAL_NEW_PATH);
    if (fs_status != SAPFS_STATUS_OK && fs_status != SAPFS_STATUS_NOT_FOUND) {
        context->report->filesystem_status = fs_status;
        return PACKAGE_SERVICE_STATUS_CLEANUP;
    }
    status = sync_data(context);
    return status == PACKAGE_SERVICE_STATUS_OK ? PACKAGE_SERVICE_STATUS_OK :
        PACKAGE_SERVICE_STATUS_CLEANUP;
}

static enum package_service_status prepare_internal(
    struct service_context *context,
    const struct package_service_prepare_request *request
)
{
    const struct package_builder_workspace *builder = request->builder;
    struct package_state_database_view base;
    struct package_state_database_view target;
    struct package_state_journal_spec journal_spec;
    uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES];
    uint8_t journal_check[PACKAGE_STATE_JOURNAL_BYTES];
    char database_path[SAPFS_MAX_PATH];
    uint64_t required_space;
    bool authority_present = false;
    bool published = false;
    enum package_service_status status;

    if (builder == NULL || request->database == NULL ||
        !builder->has_installed || builder->installed.bytes == NULL ||
        request->database_bytes > PACKAGE_SERVICE_MAX_DATABASE_BYTES ||
        plan_operation(builder->verified_plan.operation) ==
            PACKAGE_STATE_OPERATION_INVALID) {
        return PACKAGE_SERVICE_STATUS_STATE;
    }
    context->report->state_status = package_state_database_parse(
        builder->installed.bytes, builder->installed.byte_count, &base);
    if (context->report->state_status != PACKAGE_STATE_STATUS_OK) {
        return PACKAGE_SERVICE_STATUS_STATE;
    }
    context->report->state_status = package_generation_verify(&builder->spec,
        request->database, request->database_bytes, &target);
    if (context->report->state_status != PACKAGE_STATE_STATUS_OK ||
        target.generation != base.generation + 1U ||
        !staging_namespace_bounded(&builder->spec)) {
        return PACKAGE_SERVICE_STATUS_STATE;
    }
    status = read_exact_path(context, PACKAGE_SERVICE_AUTHORITY_PATH,
        authority, sizeof(authority), false, &authority_present);
    if (status != PACKAGE_SERVICE_STATUS_OK || !authority_present ||
        !authority_selects(authority, &base)) {
        return status != PACKAGE_SERVICE_STATUS_OK ? status :
            PACKAGE_SERVICE_STATUS_STATE;
    }
    if (!fixed_path_absent(context, PACKAGE_SERVICE_JOURNAL_PATH, &status) ||
        !fixed_path_absent(context, PACKAGE_SERVICE_JOURNAL_NEW_PATH, &status) ||
        !fixed_path_absent(context, PACKAGE_SERVICE_AUTHORITY_NEW_PATH, &status) ||
        !fixed_path_absent(context, PACKAGE_SERVICE_AUTHORITY_OLD_PATH, &status)) {
        return status;
    }
    status = ensure_entries(context);
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = verify_generation_files(context, &base);
    }
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return status;
    }
    required_space = request->database_bytes +
        PACKAGE_STATE_JOURNAL_BYTES + PACKAGE_STATE_AUTHORITY_BYTES;
    if (required_space < request->database_bytes) {
        return PACKAGE_SERVICE_STATUS_RESOURCE;
    }
    for (uint32_t index = 0U; index < builder->spec.file_count; ++index) {
        if (builder->files[index].length > UINT64_MAX - required_space) {
            return PACKAGE_SERVICE_STATUS_RESOURCE;
        }
        required_space += builder->files[index].length;
    }
    if (required_space > sapfs_drive(SAPFS_VOLUME_DATA).free_bytes) {
        return PACKAGE_SERVICE_STATUS_RESOURCE;
    }
    journal_spec = (struct package_state_journal_spec){
        plan_operation(builder->verified_plan.operation),
        &base,
        &target,
        required_space,
        builder->verified_plan.target.bytes,
        builder->verified_plan.target.length
    };
    context->report->state_status = package_state_journal_encode(&journal_spec,
        journal);
    if (context->report->state_status != PACKAGE_STATE_STATUS_OK) {
        return PACKAGE_SERVICE_STATUS_STATE;
    }

    status = write_new_file(context, PACKAGE_SERVICE_JOURNAL_NEW_PATH,
        journal, sizeof(journal));
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = sync_data(context);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        bool present = false;

        status = read_exact_path(context, PACKAGE_SERVICE_JOURNAL_NEW_PATH,
            journal_check, sizeof(journal_check), false, &present);
        if (status == PACKAGE_SERVICE_STATUS_OK &&
            (!present || !equal_bytes(journal, journal_check,
                sizeof(journal)))) {
            status = PACKAGE_SERVICE_STATUS_STATE;
        }
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = ensure_generation_layout(context, target.generation);
    }
    for (uint32_t index = 0U;
        status == PACKAGE_SERVICE_STATUS_OK && index < builder->spec.file_count;
        ++index) {
        status = write_generation_file(context, builder, &base, index);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK &&
        !generation_path(target.generation, GENERATION_DATABASE_SUFFIX,
            database_path)) {
        status = PACKAGE_SERVICE_STATUS_NAMESPACE;
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = write_new_file(context, database_path, request->database,
            request->database_bytes);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = ensure_entries(context);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = verify_generation_files(context, &target);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        status = sync_data(context);
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        enum sapfs_status fs_status = sapfs_rename(SAPFS_VOLUME_DATA,
            PACKAGE_SERVICE_JOURNAL_NEW_PATH,
            PACKAGE_SERVICE_JOURNAL_PATH);

        if (fs_status != SAPFS_STATUS_OK) {
            status = filesystem_failure(context, fs_status);
        } else {
            ++context->report->rename_count;
            context->report->journal_present = true;
            published = true;
            status = sync_data(context);
        }
    }
    if (status == PACKAGE_SERVICE_STATUS_OK) {
        context->report->generation = target.generation;
        context->report->prepared = true;
        return PACKAGE_SERVICE_STATUS_OK;
    }
    if (!published) {
        enum package_service_status cleanup = cleanup_unpublished_prepare(
            context, target.generation);

        if (cleanup != PACKAGE_SERVICE_STATUS_OK) {
            return cleanup;
        }
    }
    return status;
}

static enum package_service_status recover_internal(
    struct service_context *context
)
{
    uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t authority_old[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES];
    struct package_state_authority_view authority_view;
    struct package_state_journal_view journal_view;
    struct loaded_generation old_generation;
    struct loaded_generation new_generation;
    struct package_state_recovery_result recovery;
    bool authority_present = false;
    bool old_authority_present = false;
    bool journal_present = false;
    bool journal_new_present = false;
    bool used_old_authority = false;
    enum package_service_status status;

    zero_bytes(authority, sizeof(authority));
    zero_bytes(authority_old, sizeof(authority_old));
    zero_bytes(journal, sizeof(journal));
    zero_bytes(&old_generation, sizeof(old_generation));
    zero_bytes(&new_generation, sizeof(new_generation));
    status = ensure_entries(context);
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return status;
    }
    status = read_exact_path(context, PACKAGE_SERVICE_AUTHORITY_PATH,
        authority, sizeof(authority), true, &authority_present);
    if (status == PACKAGE_SERVICE_STATUS_STATE) {
        authority_present = true;
        zero_bytes(authority, sizeof(authority));
        status = PACKAGE_SERVICE_STATUS_OK;
    }
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        return status;
    }
    if (!authority_present) {
        status = read_exact_path(context, PACKAGE_SERVICE_AUTHORITY_OLD_PATH,
            authority_old, sizeof(authority_old), true,
            &old_authority_present);
        if (status != PACKAGE_SERVICE_STATUS_OK &&
            status != PACKAGE_SERVICE_STATUS_STATE) {
            return status;
        }
        if (status == PACKAGE_SERVICE_STATUS_OK && old_authority_present &&
            package_state_authority_parse(authority_old,
                sizeof(authority_old), &authority_view) ==
                PACKAGE_STATE_STATUS_OK) {
            copy_bytes(authority, authority_old, sizeof(authority));
            authority_present = true;
            used_old_authority = true;
        }
    }
    status = read_exact_path(context, PACKAGE_SERVICE_JOURNAL_PATH, journal,
        sizeof(journal), true, &journal_present);
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        context->report->state_status = PACKAGE_STATE_STATUS_LENGTH;
        return status == PACKAGE_SERVICE_STATUS_STATE ?
            PACKAGE_SERVICE_STATUS_STATE : status;
    }
    context->report->journal_present = journal_present;
    {
        struct sapfs_stat stat;
        enum sapfs_status fs_status = sapfs_stat_path(SAPFS_VOLUME_DATA,
            PACKAGE_SERVICE_JOURNAL_NEW_PATH, &stat);

        if (fs_status == SAPFS_STATUS_OK) {
            journal_new_present = true;
            if (stat.directory) {
                status = PACKAGE_SERVICE_STATUS_STATE;
                goto release;
            }
        } else if (fs_status != SAPFS_STATUS_NOT_FOUND) {
            status = filesystem_failure(context, fs_status);
            goto release;
        }
    }
    if (journal_present && journal_new_present) {
        status = PACKAGE_SERVICE_STATUS_STATE;
        goto release;
    }
    if (!journal_present && journal_new_present) {
        context->report->state_status = package_state_authority_parse(authority,
            sizeof(authority), &authority_view);
        if (!authority_present || context->report->state_status !=
                PACKAGE_STATE_STATUS_OK ||
            authority_view.generation == UINT64_MAX) {
            status = PACKAGE_SERVICE_STATUS_STATE;
            goto release;
        }
        status = cleanup_unpublished_prepare(context,
            authority_view.generation + 1U);
        if (status != PACKAGE_SERVICE_STATUS_OK) {
            goto release;
        }
    }

    if (!journal_present) {
        if (!authority_present && !old_authority_present) {
            status = PACKAGE_SERVICE_STATUS_ABSENT;
            goto release;
        }
        context->report->state_status = package_state_authority_parse(authority,
            sizeof(authority), &authority_view);
        if (!authority_present || context->report->state_status !=
                PACKAGE_STATE_STATUS_OK) {
            return PACKAGE_SERVICE_STATUS_STATE;
        }
        status = load_generation(context, authority_view.generation,
            authority_view.database_bytes, &old_generation);
        if (status != PACKAGE_SERVICE_STATUS_OK) {
            goto release;
        }
        context->report->state_status = package_state_recovery_decide(authority,
            sizeof(authority), NULL, 0U, &old_generation.candidate, NULL,
            &recovery);
        if (context->report->state_status != PACKAGE_STATE_STATUS_OK) {
            status = context->report->state_status ==
                PACKAGE_STATE_STATUS_INCOMPLETE ?
                PACKAGE_SERVICE_STATUS_INCOMPLETE :
                PACKAGE_SERVICE_STATUS_STATE;
            goto release;
        }
        context->report->choice = recovery.choice;
        context->report->generation = recovery.generation;
        if (used_old_authority) {
            uint8_t replacement[PACKAGE_STATE_AUTHORITY_BYTES];
            context->report->state_status = package_state_authority_encode(
                &recovery.database, replacement);
            if (context->report->state_status != PACKAGE_STATE_STATUS_OK) {
                status = PACKAGE_SERVICE_STATUS_STATE;
                goto release;
            }
            status = replace_authority(context, replacement);
            if (status != PACKAGE_SERVICE_STATUS_OK) {
                goto release;
            }
            enum sapfs_status fs_status = sapfs_unlink(SAPFS_VOLUME_DATA,
                PACKAGE_SERVICE_AUTHORITY_OLD_PATH);
            if (fs_status != SAPFS_STATUS_OK &&
                fs_status != SAPFS_STATUS_NOT_FOUND) {
                context->report->filesystem_status = fs_status;
                status = PACKAGE_SERVICE_STATUS_CLEANUP;
                goto release;
            }
            status = sync_data(context);
            if (status != PACKAGE_SERVICE_STATUS_OK) {
                status = PACKAGE_SERVICE_STATUS_CLEANUP;
                goto release;
            }
        }
        context->report->cleanup_complete = true;
        status = PACKAGE_SERVICE_STATUS_OK;
        goto release;
    }

    context->report->state_status = package_state_journal_parse(journal,
        sizeof(journal), &journal_view);
    if (context->report->state_status != PACKAGE_STATE_STATUS_OK) {
        status = PACKAGE_SERVICE_STATUS_STATE;
        goto release;
    }
    status = load_generation(context, journal_view.base_generation,
        journal_view.base_database_bytes, &old_generation);
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        goto release;
    }
    status = load_generation(context, journal_view.target_generation,
        journal_view.target_database_bytes, &new_generation);
    if (status != PACKAGE_SERVICE_STATUS_OK) {
        goto release;
    }
    context->report->state_status = package_state_recovery_decide(authority,
        sizeof(authority), journal, sizeof(journal),
        &old_generation.candidate, &new_generation.candidate, &recovery);
    if (context->report->state_status != PACKAGE_STATE_STATUS_OK) {
        status = context->report->state_status ==
            PACKAGE_STATE_STATUS_INCOMPLETE ?
            PACKAGE_SERVICE_STATUS_INCOMPLETE : PACKAGE_SERVICE_STATUS_STATE;
        goto release;
    }
    context->report->choice = recovery.choice;
    context->report->generation = recovery.generation;
    if (!authority_selects(authority, &recovery.database) ||
        used_old_authority) {
        uint8_t replacement[PACKAGE_STATE_AUTHORITY_BYTES];
        context->report->state_status = package_state_authority_encode(
            &recovery.database, replacement);
        if (context->report->state_status != PACKAGE_STATE_STATUS_OK) {
            status = PACKAGE_SERVICE_STATUS_STATE;
            goto release;
        }
        status = replace_authority(context, replacement);
        if (status != PACKAGE_SERVICE_STATUS_OK) {
            goto release;
        }
    }
    status = cleanup_transaction(context,
        recovery.choice == PACKAGE_STATE_RECOVERY_OLD ?
            journal_view.target_generation : journal_view.base_generation);

release:
    {
        enum package_service_status old_release = release_generation(context,
            &old_generation);
        enum package_service_status new_release = release_generation(context,
            &new_generation);
        if (old_release != PACKAGE_SERVICE_STATUS_OK) {
            status = old_release;
        }
        if (new_release != PACKAGE_SERVICE_STATUS_OK) {
            status = new_release;
        }
    }
    return status;
}

enum package_service_status package_service_recover(
    struct package_service_report *report
)
{
    struct service_context context;
    enum package_service_status status;

    if (report == NULL) {
        return PACKAGE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(report, sizeof(*report));
    report->filesystem_status = SAPFS_STATUS_OK;
    report->state_status = PACKAGE_STATE_STATUS_OK;
    if (servicing) {
        report->status = PACKAGE_SERVICE_STATUS_BUSY;
        return report->status;
    }
    struct sapfs_drive_info drive = sapfs_drive(SAPFS_VOLUME_DATA);
    if (!drive.present || !drive.mounted || !drive.healthy ||
        !heap_is_active()) {
        report->status = PACKAGE_SERVICE_STATUS_UNAVAILABLE;
        return report->status;
    }
    zero_bytes(&context, sizeof(context));
    context.report = report;
    servicing = true;
    status = recover_internal(&context);
    enum package_service_status entries_release = release_bytes(&context,
        (void **)&context.entries);
    if (entries_release != PACKAGE_SERVICE_STATUS_OK) {
        status = entries_release;
    }
    if (report->live_file_handles != 0U ||
        report->live_allocations != 0U) {
        status = PACKAGE_SERVICE_STATUS_RESOURCE;
    }
    servicing = false;
    report->status = status;
    return status;
}

enum package_service_status package_service_prepare(
    const struct package_service_prepare_request *request,
    struct package_service_report *report
)
{
    struct service_context context;
    enum package_service_status status;

    if (request == NULL || report == NULL) {
        return PACKAGE_SERVICE_STATUS_NULL_ARGUMENT;
    }
    zero_bytes(report, sizeof(*report));
    report->filesystem_status = SAPFS_STATUS_OK;
    report->state_status = PACKAGE_STATE_STATUS_OK;
    if (servicing) {
        report->status = PACKAGE_SERVICE_STATUS_BUSY;
        return report->status;
    }
    struct sapfs_drive_info drive = sapfs_drive(SAPFS_VOLUME_DATA);
    if (!drive.present || !drive.mounted || !drive.healthy || drive.read_only ||
        !heap_is_active()) {
        report->status = PACKAGE_SERVICE_STATUS_UNAVAILABLE;
        return report->status;
    }
    zero_bytes(&context, sizeof(context));
    context.report = report;
    servicing = true;
    status = prepare_internal(&context, request);
    enum package_service_status entries_release = release_bytes(&context,
        (void **)&context.entries);
    if (entries_release != PACKAGE_SERVICE_STATUS_OK) {
        status = entries_release;
    }
    if (report->live_file_handles != 0U || report->live_allocations != 0U) {
        status = PACKAGE_SERVICE_STATUS_RESOURCE;
    }
    servicing = false;
    report->status = status;
    return status;
}

const char *package_service_status_string(enum package_service_status status)
{
    static const char *const names[] = {
        "ok",
        "null argument",
        "service busy",
        "package state absent",
        "service unavailable",
        "filesystem failure",
        "resource bound",
        "invalid package state",
        "no complete generation",
        "immutable file mismatch",
        "namespace bound",
        "durability failure",
        "cleanup incomplete"
    };

    _Static_assert(sizeof(names) / sizeof(names[0]) ==
        PACKAGE_SERVICE_STATUS_COUNT,
        "package-service status table is incomplete");
    if (status < PACKAGE_SERVICE_STATUS_OK ||
        status >= PACKAGE_SERVICE_STATUS_COUNT) {
        return "unknown package-service status";
    }
    return names[status];
}
