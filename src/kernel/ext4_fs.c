/* SPDX-License-Identifier: GPL-3.0-only */
/* Read-only ext4 VFS backend over the checked Rust ext4plus adapter. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/ext4_fs.h>
#include <sapote/nvme.h>

#define EXT4_MAX_HANDLES SAPFS_MAX_HANDLES
#define EXT4_CONTROLLER_SYSTEM 0U
#define EXT4_CONTROLLER_DATA 1U

struct ext4_mount_state {
    struct nvme_volume_session session;
    struct sapote_ext4_identity identity;
    uintptr_t rust_mount;
    uint64_t generation;
    uint64_t media_bytes;
    uint64_t admitted_media_bytes;
    uint64_t completion_count;
    uint32_t controller_index;
    bool active;
    bool mounting;
    bool operation_active;
    bool healthy;
};

struct ext4_handle_state {
    uint64_t generation;
    uint64_t mount_generation;
    uint64_t offset;
    uint64_t size;
    enum sapfs_volume volume;
    char path[SAPFS_MAX_PATH];
    bool directory;
    bool active;
};

static struct ext4_mount_state ext4_mounts[SAPFS_VOLUME_COUNT];
static struct ext4_handle_state ext4_handles[EXT4_MAX_HANDLES];
static uint64_t next_mount_generation = UINT64_C(1);
static uint64_t next_handle_generation = UINT64_C(1);

extern int32_t sapote_ext4_mount(uintptr_t context, uint64_t media_bytes,
    struct sapote_ext4_identity *identity, uintptr_t *mounted_out);
extern int32_t sapote_ext4_unmount(uintptr_t mounted);
extern int32_t sapote_ext4_stat(uintptr_t mounted, const uint8_t *path,
    size_t path_length, struct sapote_ext4_metadata *metadata);
extern int32_t sapote_ext4_pread(uintptr_t mounted, const uint8_t *path,
    size_t path_length, uint64_t offset, uint8_t *destination,
    size_t capacity, size_t *read_out);
extern int32_t sapote_ext4_directory_entry(uintptr_t mounted,
    const uint8_t *path, size_t path_length, uint64_t index,
    struct sapote_ext4_directory_entry *entry, bool *present);

_Static_assert(sizeof(struct sapote_ext4_metadata) == 40U,
    "ext4 metadata C/Rust ABI drift");
_Static_assert(offsetof(struct sapote_ext4_metadata, file_type) == 28U,
    "ext4 metadata C/Rust ABI offset drift");
_Static_assert(sizeof(struct sapote_ext4_directory_entry) == 304U,
    "ext4 directory C/Rust ABI drift");

static void zero_bytes(void *pointer, size_t length)
{
    uint8_t *bytes = pointer;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(void *destination, const void *source, size_t length)
{
    uint8_t *to = destination;
    const uint8_t *from = source;

    for (size_t index = 0U; index < length; ++index) {
        to[index] = from[index];
    }
}

static size_t path_length(const char *path)
{
    size_t length = 0U;

    if (path == NULL) {
        return SAPFS_MAX_PATH;
    }
    while (length < SAPFS_MAX_PATH && path[length] != '\0') {
        ++length;
    }
    return length;
}

static bool valid_volume(enum sapfs_volume volume)
{
    return volume >= SAPFS_VOLUME_SYSTEM && volume < SAPFS_VOLUME_COUNT;
}

static uint64_t generation(uint64_t *next)
{
    uint64_t value = *next;

    ++*next;
    if (value == 0U || value > (UINT64_MAX >> 8U)) {
        value = 1U;
        *next = 2U;
    }
    return value;
}

static enum sapfs_status map_status(int32_t status)
{
    switch (status) {
    case SAPOTE_EXT4_STATUS_OK:
        return SAPFS_STATUS_OK;
    case SAPOTE_EXT4_STATUS_NULL_ARGUMENT:
        return SAPFS_STATUS_INVALID_ARGUMENT;
    case SAPOTE_EXT4_STATUS_VOLUME:
        return SAPFS_STATUS_NOT_MOUNTED;
    case SAPOTE_EXT4_STATUS_IO:
        return SAPFS_STATUS_IO;
    case SAPOTE_EXT4_STATUS_INVALID:
        return SAPFS_STATUS_CORRUPT;
    case SAPOTE_EXT4_STATUS_NOT_FOUND:
        return SAPFS_STATUS_NOT_FOUND;
    case SAPOTE_EXT4_STATUS_NOT_DIRECTORY:
        return SAPFS_STATUS_NOT_DIRECTORY;
    case SAPOTE_EXT4_STATUS_IS_DIRECTORY:
        return SAPFS_STATUS_IS_DIRECTORY;
    case SAPOTE_EXT4_STATUS_RANGE:
        return SAPFS_STATUS_RANGE;
    case SAPOTE_EXT4_STATUS_SPECIAL:
        return SAPFS_STATUS_ACCESS;
    default:
        return SAPFS_STATUS_CORRUPT;
    }
}

static enum sapfs_status begin_operation(struct ext4_mount_state *mount)
{
    enum nvme_status status;

    if (mount == NULL || (!mount->active && !mount->mounting)) {
        return SAPFS_STATUS_NOT_MOUNTED;
    }
    if (mount->active && !mount->healthy) {
        return SAPFS_STATUS_IO;
    }
    if (mount->operation_active) {
        return SAPFS_STATUS_BUSY;
    }
    status = nvme_volume_open(&mount->session, mount->controller_index, false);
    if (status != NVME_STATUS_OK) {
        return SAPFS_STATUS_IO;
    }
    if (mount->session.logical_block_bytes == 0U ||
        mount->session.namespace_blocks >
            UINT64_MAX / mount->session.logical_block_bytes) {
        (void)nvme_volume_close(&mount->session);
        zero_bytes(&mount->session, sizeof(mount->session));
        return SAPFS_STATUS_RANGE;
    }
    mount->media_bytes = mount->session.namespace_blocks *
        mount->session.logical_block_bytes;
    if (mount->active && mount->admitted_media_bytes != 0U &&
        mount->media_bytes != mount->admitted_media_bytes) {
        (void)nvme_volume_close(&mount->session);
        zero_bytes(&mount->session, sizeof(mount->session));
        mount->healthy = false;
        return SAPFS_STATUS_IO;
    }
    mount->operation_active = true;
    return SAPFS_STATUS_OK;
}

static enum sapfs_status end_operation(struct ext4_mount_state *mount)
{
    enum nvme_status status;

    if (mount == NULL || !mount->operation_active) {
        return SAPFS_STATUS_CORRUPT;
    }
    status = nvme_volume_close(&mount->session);
    mount->operation_active = false;
    zero_bytes(&mount->session, sizeof(mount->session));
    if (status != NVME_STATUS_OK) {
        mount->healthy = false;
        return SAPFS_STATUS_IO;
    }
    ++mount->completion_count;
    return SAPFS_STATUS_OK;
}

/* Rust may read only through the lease installed by begin_operation(). */
int32_t sapote_ext4_block_read(
    uintptr_t context,
    uint64_t start_byte,
    uint8_t *destination,
    size_t length
)
{
    struct ext4_mount_state *mount = (struct ext4_mount_state *)context;
    struct nvme_volume_session *session;
    uint8_t block[NVME_BLOCK_BYTES];
    uint64_t position = start_byte;
    size_t remaining = length;

    if (mount == NULL || destination == NULL || !mount->operation_active) {
        return -1;
    }
    session = &mount->session;
    if (!session->active || session->logical_block_bytes == 0U ||
        session->logical_block_bytes > sizeof(block) ||
        start_byte > mount->media_bytes ||
        length > mount->media_bytes - start_byte) {
        return -1;
    }
    while (remaining != 0U) {
        const uint64_t lba = position / session->logical_block_bytes;
        const size_t within = (size_t)(position % session->logical_block_bytes);
        size_t chunk = session->logical_block_bytes - within;

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (nvme_volume_read(session, lba, block,
                session->logical_block_bytes) != NVME_STATUS_OK) {
            return -1;
        }
        copy_bytes(destination, &block[within], chunk);
        destination += chunk;
        position += chunk;
        remaining -= chunk;
    }
    return 0;
}

static enum sapfs_status checked_stat(
    struct ext4_mount_state *mount,
    const char *path,
    struct sapote_ext4_metadata *metadata
)
{
    const size_t length = path_length(path);
    enum sapfs_status status;
    enum sapfs_status close_status;

    if (metadata == NULL || length == 0U || length >= SAPFS_MAX_PATH) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(metadata, sizeof(*metadata));
    status = begin_operation(mount);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    status = map_status(sapote_ext4_stat(mount->rust_mount,
        (const uint8_t *)path, length, metadata));
    close_status = end_operation(mount);
    return status != SAPFS_STATUS_OK ? status : close_status;
}

static void fill_stat(
    const struct sapote_ext4_metadata *source,
    struct sapfs_stat *destination
)
{
    zero_bytes(destination, sizeof(*destination));
    destination->size = source->size;
    destination->object_id = source->inode;
    destination->uid = source->uid;
    destination->gid = source->gid;
    destination->mode = source->mode;
    destination->links = source->links;
    destination->directory = source->file_type == SAPOTE_EXT4_FILE_DIRECTORY;
    destination->read_only = true;
}

static enum sapfs_status handle_state(
    sapfs_handle handle,
    struct ext4_handle_state **state
)
{
    const uint64_t encoded = handle & UINT64_C(0xff);
    const uint64_t encoded_generation = handle >> 8U;
    size_t index;

    if (state == NULL || encoded == 0U || encoded > EXT4_MAX_HANDLES ||
        encoded_generation == 0U) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    index = (size_t)(encoded - 1U);
    if (!ext4_handles[index].active ||
        ext4_handles[index].generation != encoded_generation ||
        !valid_volume(ext4_handles[index].volume) ||
        !ext4_mounts[ext4_handles[index].volume].active ||
        ext4_handles[index].mount_generation !=
            ext4_mounts[ext4_handles[index].volume].generation) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    *state = &ext4_handles[index];
    return SAPFS_STATUS_OK;
}

static enum sapfs_status allocate_handle(enum sapfs_volume volume,
    const char *path, uint64_t size, bool directory, sapfs_handle *handle)
{
    const size_t length = path_length(path);
    size_t slot = EXT4_MAX_HANDLES;

    if (handle == NULL || length == 0U || length >= SAPFS_MAX_PATH) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        if (!ext4_handles[index].active) {
            slot = index;
            break;
        }
    }
    if (slot == EXT4_MAX_HANDLES) {
        return SAPFS_STATUS_NO_HANDLES;
    }
    zero_bytes(&ext4_handles[slot], sizeof(ext4_handles[slot]));
    ext4_handles[slot].generation = generation(&next_handle_generation);
    ext4_handles[slot].mount_generation = ext4_mounts[volume].generation;
    ext4_handles[slot].size = size;
    ext4_handles[slot].volume = volume;
    ext4_handles[slot].directory = directory;
    copy_bytes(ext4_handles[slot].path, path, length + 1U);
    ext4_handles[slot].active = true;
    *handle = ext4_handles[slot].generation << 8U | (uint64_t)(slot + 1U);
    return SAPFS_STATUS_OK;
}

void ext4_backend_initialize(void)
{
    zero_bytes(ext4_mounts, sizeof(ext4_mounts));
    zero_bytes(ext4_handles, sizeof(ext4_handles));
}

enum sapfs_status ext4_backend_mount(enum sapfs_volume volume)
{
    struct ext4_mount_state *mount;
    enum sapfs_status status;
    enum sapfs_status close_status;
    int32_t rust_status;

    if (!valid_volume(volume)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    if (mount->active) {
        return SAPFS_STATUS_ALREADY_MOUNTED;
    }
    zero_bytes(mount, sizeof(*mount));
    mount->controller_index = volume == SAPFS_VOLUME_SYSTEM ?
        EXT4_CONTROLLER_SYSTEM : EXT4_CONTROLLER_DATA;
    mount->mounting = true;
    mount->healthy = true;
    status = begin_operation(mount);
    if (status != SAPFS_STATUS_OK) {
        zero_bytes(mount, sizeof(*mount));
        return status;
    }
    rust_status = sapote_ext4_mount((uintptr_t)mount, mount->media_bytes,
        &mount->identity, &mount->rust_mount);
    close_status = end_operation(mount);
    status = map_status(rust_status);
    if (status != SAPFS_STATUS_OK || close_status != SAPFS_STATUS_OK) {
        if (mount->rust_mount != 0U) {
            (void)sapote_ext4_unmount(mount->rust_mount);
        }
        zero_bytes(mount, sizeof(*mount));
        return status != SAPFS_STATUS_OK ? status : close_status;
    }
    mount->generation = generation(&next_mount_generation);
    mount->admitted_media_bytes = mount->media_bytes;
    mount->mounting = false;
    mount->active = true;
    return SAPFS_STATUS_OK;
}

enum sapfs_status ext4_backend_unmount(enum sapfs_volume volume)
{
    struct ext4_mount_state *mount;

    if (!valid_volume(volume)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    mount = &ext4_mounts[volume];
    if (!mount->active) {
        return SAPFS_STATUS_NOT_MOUNTED;
    }
    for (size_t index = 0U; index < EXT4_MAX_HANDLES; ++index) {
        if (ext4_handles[index].active &&
            ext4_handles[index].volume == volume) {
            return SAPFS_STATUS_BUSY;
        }
    }
    if (sapote_ext4_unmount(mount->rust_mount) != SAPOTE_EXT4_STATUS_OK) {
        return SAPFS_STATUS_CORRUPT;
    }
    zero_bytes(mount, sizeof(*mount));
    return SAPFS_STATUS_OK;
}

enum sapfs_status ext4_backend_sync(enum sapfs_volume volume)
{
    return valid_volume(volume) && ext4_mounts[volume].active ?
        SAPFS_STATUS_READ_ONLY : SAPFS_STATUS_NOT_MOUNTED;
}

struct sapfs_drive_info ext4_backend_drive(enum sapfs_volume volume)
{
    struct sapfs_drive_info drive = {0};
    struct ext4_mount_state *mount;

    if (!valid_volume(volume)) {
        return drive;
    }
    mount = &ext4_mounts[volume];
    drive.volume = volume;
    drive.volume_id = (uint32_t)mount->identity.uuid[0] |
        (uint32_t)mount->identity.uuid[1] << 8U |
        (uint32_t)mount->identity.uuid[2] << 16U |
        (uint32_t)mount->identity.uuid[3] << 24U;
    drive.total_bytes = mount->media_bytes;
    drive.present = mount->active;
    drive.mounted = mount->active;
    drive.read_only = true;
    drive.healthy = mount->healthy;
    return drive;
}

uint64_t ext4_backend_completion_count(enum sapfs_volume volume)
{
    return valid_volume(volume) ? ext4_mounts[volume].completion_count : 0U;
}

enum sapfs_status ext4_backend_open(enum sapfs_volume volume,
    const char *path, enum sapfs_access access, sapfs_handle *handle)
{
    struct sapote_ext4_metadata metadata;
    enum sapfs_status status;

    if (handle == NULL || !valid_volume(volume)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    if (access != SAPFS_ACCESS_READ) {
        return SAPFS_STATUS_READ_ONLY;
    }
    status = checked_stat(&ext4_mounts[volume], path, &metadata);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (metadata.file_type == SAPOTE_EXT4_FILE_DIRECTORY) {
        return SAPFS_STATUS_IS_DIRECTORY;
    }
    return allocate_handle(volume, path, metadata.size, false, handle);
}

enum sapfs_status ext4_backend_close(sapfs_handle handle)
{
    struct ext4_handle_state *state;
    enum sapfs_status status = handle_state(handle, &state);

    if (status == SAPFS_STATUS_OK) {
        zero_bytes(state, sizeof(*state));
    }
    return status;
}

enum sapfs_status ext4_backend_pread(sapfs_handle handle,
    uint8_t *destination, size_t capacity, uint64_t offset,
    size_t *read_bytes)
{
    struct ext4_handle_state *state;
    struct ext4_mount_state *mount;
    enum sapfs_status status;
    enum sapfs_status close_status;

    if (read_bytes == NULL || (capacity != 0U && destination == NULL)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    *read_bytes = 0U;
    status = handle_state(handle, &state);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (state->directory) {
        return SAPFS_STATUS_IS_DIRECTORY;
    }
    if (capacity == 0U || offset >= state->size) {
        return SAPFS_STATUS_OK;
    }
    mount = &ext4_mounts[state->volume];
    status = begin_operation(mount);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    status = map_status(sapote_ext4_pread(mount->rust_mount,
        (const uint8_t *)state->path, path_length(state->path), offset,
        destination, capacity, read_bytes));
    close_status = end_operation(mount);
    return status != SAPFS_STATUS_OK ? status : close_status;
}

enum sapfs_status ext4_backend_read(sapfs_handle handle,
    uint8_t *destination, size_t capacity, size_t *read_bytes)
{
    struct ext4_handle_state *state;
    enum sapfs_status status = handle_state(handle, &state);

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    status = ext4_backend_pread(handle, destination, capacity, state->offset,
        read_bytes);
    if (status == SAPFS_STATUS_OK && read_bytes != NULL) {
        if (*read_bytes > UINT64_MAX - state->offset) {
            return SAPFS_STATUS_RANGE;
        }
        state->offset += *read_bytes;
    }
    return status;
}

enum sapfs_status ext4_backend_write(sapfs_handle handle,
    const uint8_t *source, size_t source_bytes, size_t *written_bytes)
{
    (void)handle;
    (void)source;
    (void)source_bytes;
    if (written_bytes != NULL) {
        *written_bytes = 0U;
    }
    return SAPFS_STATUS_READ_ONLY;
}

enum sapfs_status ext4_backend_seek(sapfs_handle handle, int64_t offset,
    enum sapfs_seek_origin origin, uint64_t *position)
{
    struct ext4_handle_state *state;
    uint64_t base;
    uint64_t target;
    enum sapfs_status status;

    if (position == NULL) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    *position = 0U;
    status = handle_state(handle, &state);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    base = origin == SAPFS_SEEK_START ? 0U :
        (origin == SAPFS_SEEK_CURRENT ? state->offset :
            (origin == SAPFS_SEEK_END ? state->size : UINT64_MAX));
    if (base == UINT64_MAX) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    if (offset < 0) {
        const uint64_t magnitude = (uint64_t)(-(offset + 1)) + 1U;
        if (magnitude > base) {
            return SAPFS_STATUS_RANGE;
        }
        target = base - magnitude;
    } else {
        if ((uint64_t)offset > UINT64_MAX - base) {
            return SAPFS_STATUS_RANGE;
        }
        target = base + (uint64_t)offset;
    }
    state->offset = target;
    *position = target;
    return SAPFS_STATUS_OK;
}

enum sapfs_status ext4_backend_stat_path(enum sapfs_volume volume,
    const char *path, struct sapfs_stat *stat)
{
    struct sapote_ext4_metadata metadata;
    enum sapfs_status status;

    if (stat == NULL || !valid_volume(volume)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(stat, sizeof(*stat));
    status = checked_stat(&ext4_mounts[volume], path, &metadata);
    if (status == SAPFS_STATUS_OK) {
        fill_stat(&metadata, stat);
    }
    return status;
}

static void fill_entry(const struct sapote_ext4_directory_entry *source,
    struct sapfs_list_entry *destination)
{
    zero_bytes(destination, sizeof(*destination));
    copy_bytes(destination->name, source->name, source->name_length);
    destination->name[source->name_length] = '\0';
    destination->size = source->metadata.size;
    destination->object_id = source->metadata.inode;
    destination->mode = source->metadata.mode;
    destination->directory =
        source->metadata.file_type == SAPOTE_EXT4_FILE_DIRECTORY;
}

static enum sapfs_status indexed_entry(struct ext4_handle_state *state,
    uint64_t index, struct sapfs_list_entry *entry, bool *present)
{
    struct sapote_ext4_directory_entry raw;
    struct ext4_mount_state *mount = &ext4_mounts[state->volume];
    enum sapfs_status status = begin_operation(mount);
    enum sapfs_status close_status;

    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    zero_bytes(&raw, sizeof(raw));
    status = map_status(sapote_ext4_directory_entry(mount->rust_mount,
        (const uint8_t *)state->path, path_length(state->path), index, &raw,
        present));
    close_status = end_operation(mount);
    if (status == SAPFS_STATUS_OK && *present) {
        if (raw.name_length == 0U || raw.name_length >=
                SAPFS_MAX_COMPONENT_BYTES) {
            status = SAPFS_STATUS_NAME;
        } else {
            fill_entry(&raw, entry);
        }
    }
    return status != SAPFS_STATUS_OK ? status : close_status;
}

enum sapfs_status ext4_backend_directory_open(enum sapfs_volume volume,
    const char *path, sapfs_handle *handle)
{
    struct sapote_ext4_metadata metadata;
    enum sapfs_status status;

    if (!valid_volume(volume) || handle == NULL) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    *handle = 0U;
    status = checked_stat(&ext4_mounts[volume], path, &metadata);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (metadata.file_type != SAPOTE_EXT4_FILE_DIRECTORY) {
        return SAPFS_STATUS_NOT_DIRECTORY;
    }
    return allocate_handle(volume, path, metadata.size, true, handle);
}

enum sapfs_status ext4_backend_directory_read(sapfs_handle handle,
    struct sapfs_list_entry *entry, bool *present)
{
    struct ext4_handle_state *state;
    enum sapfs_status status;

    if (entry == NULL || present == NULL) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    zero_bytes(entry, sizeof(*entry));
    *present = false;
    status = handle_state(handle, &state);
    if (status != SAPFS_STATUS_OK) {
        return status;
    }
    if (!state->directory) {
        return SAPFS_STATUS_NOT_DIRECTORY;
    }
    status = indexed_entry(state, state->offset, entry, present);
    if (status == SAPFS_STATUS_OK && *present) {
        ++state->offset;
    }
    return status;
}

enum sapfs_status ext4_backend_directory_close(sapfs_handle handle)
{
    return ext4_backend_close(handle);
}

enum sapfs_status ext4_backend_list(enum sapfs_volume volume,
    const char *path, struct sapfs_list_entry *entries, size_t capacity,
    size_t *entry_count)
{
    sapfs_handle handle = 0U;
    size_t count = 0U;
    enum sapfs_status status;

    if (entry_count == NULL || (capacity != 0U && entries == NULL)) {
        return SAPFS_STATUS_INVALID_ARGUMENT;
    }
    *entry_count = 0U;
    status = ext4_backend_directory_open(volume, path, &handle);
    while (status == SAPFS_STATUS_OK && count < capacity) {
        bool present = false;

        status = ext4_backend_directory_read(handle, &entries[count], &present);
        if (status != SAPFS_STATUS_OK || !present) {
            break;
        }
        ++count;
    }
    if (status == SAPFS_STATUS_OK && count == capacity) {
        struct sapfs_list_entry ignored;
        bool present = false;

        status = ext4_backend_directory_read(handle, &ignored, &present);
        if (status == SAPFS_STATUS_OK && present) {
            status = SAPFS_STATUS_RANGE;
        }
    }
    if (handle != 0U) {
        enum sapfs_status close_status = ext4_backend_directory_close(handle);

        if (status == SAPFS_STATUS_OK) {
            status = close_status;
        }
    }
    *entry_count = count;
    return status;
}

/* All mutation and durability requests remain refused until JBD2 exists. */
enum sapfs_status ext4_backend_create(enum sapfs_volume volume,
    const char *path)
{
    (void)volume;
    (void)path;
    return SAPFS_STATUS_READ_ONLY;
}

enum sapfs_status ext4_backend_truncate(enum sapfs_volume volume,
    const char *path, uint64_t size)
{
    (void)volume;
    (void)path;
    (void)size;
    return SAPFS_STATUS_READ_ONLY;
}

enum sapfs_status ext4_backend_mkdir(enum sapfs_volume volume,
    const char *path)
{
    (void)volume;
    (void)path;
    return SAPFS_STATUS_READ_ONLY;
}

enum sapfs_status ext4_backend_rename(enum sapfs_volume volume,
    const char *source, const char *destination)
{
    (void)volume;
    (void)source;
    (void)destination;
    return SAPFS_STATUS_READ_ONLY;
}

enum sapfs_status ext4_backend_unlink(enum sapfs_volume volume,
    const char *path)
{
    (void)volume;
    (void)path;
    return SAPFS_STATUS_READ_ONLY;
}

enum sapfs_status ext4_backend_rmdir(enum sapfs_volume volume,
    const char *path)
{
    (void)volume;
    (void)path;
    return SAPFS_STATUS_READ_ONLY;
}

enum sapfs_status ext4_backend_link(enum sapfs_volume volume,
    const char *source, const char *destination)
{
    (void)volume;
    (void)source;
    (void)destination;
    return SAPFS_STATUS_READ_ONLY;
}
