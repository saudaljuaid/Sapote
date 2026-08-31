/* SPDX-License-Identifier: GPL-3.0-only */
int package_state_core_host_test_main(void);

#define main package_state_core_host_test_main
#include "package-state-host-test.c"
#undef main

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sapote/fat32_fs.h>
#include <sapote/heap.h>
#include <sapote/package_service.h>

#define MOCK_MAX_NODES 96U
#define MOCK_MAX_FILE_BYTES 4096U
#define MOCK_MAX_HANDLES 16U
#define MOCK_MAX_EVENTS 128U

enum mock_event {
    MOCK_EVENT_WRITE_AUTHORITY = 1,
    MOCK_EVENT_SYNC,
    MOCK_EVENT_RENAME_OLD,
    MOCK_EVENT_RENAME_AUTHORITY,
    MOCK_EVENT_UNLINK_JOURNAL
};

struct mock_node {
    bool active;
    bool directory;
    char path[SAPFS_MAX_PATH];
    uint8_t bytes[MOCK_MAX_FILE_BYTES];
    size_t byte_count;
    uint16_t mode;
    uint64_t object_id;
};

struct mock_handle {
    bool active;
    size_t node;
    size_t offset;
};

static struct mock_node nodes[MOCK_MAX_NODES];
static struct mock_handle handles[MOCK_MAX_HANDLES];
static enum mock_event events[MOCK_MAX_EVENTS];
static size_t event_count;
static uint64_t next_object_id;
static bool fail_next_sync;

static void event(enum mock_event value)
{
    if (event_count < MOCK_MAX_EVENTS) {
        events[event_count++] = value;
    }
}

static size_t find_node(const char *path)
{
    for (size_t index = 0U; index < MOCK_MAX_NODES; ++index) {
        if (nodes[index].active && strcmp(nodes[index].path, path) == 0) {
            return index;
        }
    }
    return MOCK_MAX_NODES;
}

static size_t add_node(
    const char *path,
    bool directory,
    const uint8_t *bytes,
    size_t byte_count,
    uint16_t mode
)
{
    size_t existing = find_node(path);
    if (existing != MOCK_MAX_NODES) {
        nodes[existing].directory = directory;
        nodes[existing].byte_count = byte_count;
        nodes[existing].mode = mode;
        if (bytes != NULL && byte_count != 0U) {
            memcpy(nodes[existing].bytes, bytes, byte_count);
        }
        return existing;
    }
    for (size_t index = 0U; index < MOCK_MAX_NODES; ++index) {
        if (!nodes[index].active) {
            memset(&nodes[index], 0, sizeof(nodes[index]));
            nodes[index].active = true;
            nodes[index].directory = directory;
            nodes[index].byte_count = byte_count;
            nodes[index].mode = mode;
            nodes[index].object_id = next_object_id++;
            (void)snprintf(nodes[index].path, sizeof(nodes[index].path),
                "%s", path);
            if (bytes != NULL && byte_count != 0U) {
                memcpy(nodes[index].bytes, bytes, byte_count);
            }
            return index;
        }
    }
    return MOCK_MAX_NODES;
}

static void add_directory(const char *path)
{
    (void)add_node(path, true, NULL, 0U, UINT16_C(0555));
}

static void add_file(
    const char *path,
    const uint8_t *bytes,
    size_t byte_count,
    uint16_t mode
)
{
    (void)add_node(path, false, bytes, byte_count, mode);
}

static void reset_filesystem(void)
{
    memset(nodes, 0, sizeof(nodes));
    memset(handles, 0, sizeof(handles));
    memset(events, 0, sizeof(events));
    event_count = 0U;
    next_object_id = 1U;
    fail_next_sync = false;
    add_directory("pkgstate");
    add_directory("pkgstate/gen");
    add_directory("pkgstate/gen/00000000");
}

static void add_old_generation(const uint8_t old_database[OLD_DATABASE_BYTES])
{
    add_directory("pkgstate/gen/00000000/00000001");
    add_directory("pkgstate/gen/00000000/00000001/root");
    add_file("pkgstate/gen/00000000/00000001/state.db", old_database,
        OLD_DATABASE_BYTES, UINT16_C(0444));
}

static void add_new_generation(const uint8_t new_database[NEW_DATABASE_BYTES])
{
    add_directory("pkgstate/gen/00000000/00000002");
    add_directory("pkgstate/gen/00000000/00000002/root");
    add_directory("pkgstate/gen/00000000/00000002/root/bin");
    add_directory("pkgstate/gen/00000000/00000002/root/lib");
    add_file("pkgstate/gen/00000000/00000002/state.db", new_database,
        NEW_DATABASE_BYTES, UINT16_C(0444));
    add_file("pkgstate/gen/00000000/00000002/root/bin/app",
        (const uint8_t *)"app", 3U, UINT16_C(0555));
    add_file("pkgstate/gen/00000000/00000002/root/lib/libx.so.1",
        (const uint8_t *)"lib", 3U, UINT16_C(0444));
}

struct sapfs_drive_info sapfs_drive(enum sapfs_volume volume)
{
    struct sapfs_drive_info info;
    memset(&info, 0, sizeof(info));
    info.volume = volume;
    info.present = true;
    info.mounted = true;
    info.healthy = true;
    info.total_bytes = UINT64_C(64) * 1024U * 1024U;
    info.free_bytes = UINT64_C(32) * 1024U * 1024U;
    return info;
}

enum sapfs_status sapfs_sync(enum sapfs_volume volume)
{
    (void)volume;
    event(MOCK_EVENT_SYNC);
    if (fail_next_sync) {
        fail_next_sync = false;
        return SAPFS_STATUS_WRITEBACK;
    }
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_stat_path(
    enum sapfs_volume volume,
    const char *path,
    struct sapfs_stat *stat
)
{
    (void)volume;
    size_t index = find_node(path);
    if (index == MOCK_MAX_NODES) {
        return SAPFS_STATUS_NOT_FOUND;
    }
    memset(stat, 0, sizeof(*stat));
    stat->size = nodes[index].byte_count;
    stat->object_id = nodes[index].object_id;
    stat->mode = nodes[index].mode;
    stat->links = 1U;
    stat->directory = nodes[index].directory;
    stat->read_only = (nodes[index].mode & UINT16_C(0222)) == 0U;
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_open(
    enum sapfs_volume volume,
    const char *path,
    enum sapfs_access access,
    sapfs_handle *handle
)
{
    (void)volume;
    (void)access;
    size_t node = find_node(path);
    if (node == MOCK_MAX_NODES) {
        return SAPFS_STATUS_NOT_FOUND;
    }
    if (nodes[node].directory) {
        return SAPFS_STATUS_IS_DIRECTORY;
    }
    for (size_t index = 0U; index < MOCK_MAX_HANDLES; ++index) {
        if (!handles[index].active) {
            handles[index].active = true;
            handles[index].node = node;
            handles[index].offset = 0U;
            *handle = index + 1U;
            return SAPFS_STATUS_OK;
        }
    }
    return SAPFS_STATUS_NO_HANDLES;
}

static struct mock_handle *mock_handle(sapfs_handle handle)
{
    if (handle == 0U || handle > MOCK_MAX_HANDLES ||
        !handles[handle - 1U].active) {
        return NULL;
    }
    return &handles[handle - 1U];
}

enum sapfs_status sapfs_close(sapfs_handle handle)
{
    struct mock_handle *state = mock_handle(handle);
    if (state == NULL) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    state->active = false;
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_read(
    sapfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
)
{
    struct mock_handle *state = mock_handle(handle);
    if (state == NULL) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    struct mock_node *node = &nodes[state->node];
    size_t available = node->byte_count - state->offset;
    size_t count = capacity < available ? capacity : available;
    if (count != 0U) {
        memcpy(destination, node->bytes + state->offset, count);
    }
    state->offset += count;
    *read_bytes = count;
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_write(
    sapfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
)
{
    struct mock_handle *state = mock_handle(handle);
    if (state == NULL) {
        return SAPFS_STATUS_STALE_HANDLE;
    }
    if (source_bytes > MOCK_MAX_FILE_BYTES - state->offset) {
        return SAPFS_STATUS_FULL;
    }
    struct mock_node *node = &nodes[state->node];
    memcpy(node->bytes + state->offset, source, source_bytes);
    state->offset += source_bytes;
    if (state->offset > node->byte_count) {
        node->byte_count = state->offset;
    }
    *written_bytes = source_bytes;
    if (strcmp(node->path, PACKAGE_SERVICE_AUTHORITY_NEW_PATH) == 0) {
        event(MOCK_EVENT_WRITE_AUTHORITY);
    }
    return SAPFS_STATUS_OK;
}

static bool direct_child(
    const char *parent,
    const char *path,
    const char **name
)
{
    size_t length = strlen(parent);
    if (strncmp(parent, path, length) != 0 || path[length] != '/' ||
        path[length + 1U] == '\0') {
        return false;
    }
    const char *child = path + length + 1U;
    if (strchr(child, '/') != NULL) {
        return false;
    }
    *name = child;
    return true;
}

enum sapfs_status sapfs_list(
    enum sapfs_volume volume,
    const char *path,
    struct sapfs_list_entry *entries,
    size_t capacity,
    size_t *entry_count
)
{
    (void)volume;
    size_t parent = find_node(path);
    if (parent == MOCK_MAX_NODES) {
        return SAPFS_STATUS_NOT_FOUND;
    }
    if (!nodes[parent].directory) {
        return SAPFS_STATUS_NOT_DIRECTORY;
    }
    size_t count = 0U;
    for (size_t index = 0U; index < MOCK_MAX_NODES; ++index) {
        const char *name;
        if (!nodes[index].active || !direct_child(path, nodes[index].path,
                &name)) {
            continue;
        }
        if (count == capacity) {
            return SAPFS_STATUS_DIRECTORY_FULL;
        }
        memset(&entries[count], 0, sizeof(entries[count]));
        (void)snprintf(entries[count].name, sizeof(entries[count].name),
            "%s", name);
        entries[count].size = nodes[index].byte_count;
        entries[count].object_id = nodes[index].object_id;
        entries[count].mode = nodes[index].mode;
        entries[count].directory = nodes[index].directory;
        ++count;
    }
    *entry_count = count;
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_create(enum sapfs_volume volume, const char *path)
{
    (void)volume;
    if (find_node(path) != MOCK_MAX_NODES) {
        return SAPFS_STATUS_EXISTS;
    }
    return add_node(path, false, NULL, 0U, UINT16_C(0644)) ==
        MOCK_MAX_NODES ? SAPFS_STATUS_FULL : SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_rename(
    enum sapfs_volume volume,
    const char *source,
    const char *destination
)
{
    (void)volume;
    size_t node = find_node(source);
    if (node == MOCK_MAX_NODES) {
        return SAPFS_STATUS_NOT_FOUND;
    }
    if (find_node(destination) != MOCK_MAX_NODES) {
        return SAPFS_STATUS_EXISTS;
    }
    (void)snprintf(nodes[node].path, sizeof(nodes[node].path), "%s",
        destination);
    if (strcmp(destination, PACKAGE_SERVICE_AUTHORITY_OLD_PATH) == 0) {
        event(MOCK_EVENT_RENAME_OLD);
    } else if (strcmp(destination, PACKAGE_SERVICE_AUTHORITY_PATH) == 0) {
        event(MOCK_EVENT_RENAME_AUTHORITY);
    }
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_unlink(enum sapfs_volume volume, const char *path)
{
    (void)volume;
    size_t node = find_node(path);
    if (node == MOCK_MAX_NODES) {
        return SAPFS_STATUS_NOT_FOUND;
    }
    if (nodes[node].directory) {
        return SAPFS_STATUS_IS_DIRECTORY;
    }
    if (strcmp(path, PACKAGE_SERVICE_JOURNAL_PATH) == 0) {
        event(MOCK_EVENT_UNLINK_JOURNAL);
    }
    nodes[node].active = false;
    return SAPFS_STATUS_OK;
}

enum sapfs_status sapfs_rmdir(enum sapfs_volume volume, const char *path)
{
    (void)volume;
    size_t node = find_node(path);
    if (node == MOCK_MAX_NODES) {
        return SAPFS_STATUS_NOT_FOUND;
    }
    if (!nodes[node].directory) {
        return SAPFS_STATUS_NOT_DIRECTORY;
    }
    for (size_t index = 0U; index < MOCK_MAX_NODES; ++index) {
        const char *ignored;
        if (nodes[index].active && direct_child(path, nodes[index].path,
                &ignored)) {
            return SAPFS_STATUS_NOT_EMPTY;
        }
    }
    nodes[node].active = false;
    return SAPFS_STATUS_OK;
}

enum heap_status heap_allocate(uint64_t size, void **pointer)
{
    if (pointer == NULL || size == 0U || size > SIZE_MAX) {
        return HEAP_STATUS_ZERO_SIZE;
    }
    *pointer = malloc((size_t)size);
    return *pointer == NULL ? HEAP_STATUS_OUT_OF_MEMORY : HEAP_STATUS_OK;
}

enum heap_status heap_free(void *pointer)
{
    if (pointer == NULL) {
        return HEAP_STATUS_BAD_POINTER;
    }
    free(pointer);
    return HEAP_STATUS_OK;
}

bool heap_is_active(void)
{
    return true;
}

static bool report_clean(const struct package_service_report *report)
{
    return report->live_file_handles == 0U &&
        report->live_allocations == 0U &&
        report->peak_file_handles == 1U &&
        report->peak_allocations >= 2U;
}

static size_t first_event(enum mock_event wanted)
{
    for (size_t index = 0U; index < event_count; ++index) {
        if (events[index] == wanted) {
            return index;
        }
    }
    return MOCK_MAX_EVENTS;
}

static int test_absent_state_is_distinct(void)
{
    struct package_service_report report;
    reset_filesystem();
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_ABSENT,
        100);
    CHECK(report.live_file_handles == 0U &&
        report.live_allocations == 0U && !report.journal_present &&
        !report.cleanup_complete, 99);
    return 0;
}

static int test_selected_generation_without_journal(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 101);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.generation == 1U && !report.journal_present &&
        report.cleanup_complete && !report.authority_replaced, 102);
    CHECK(report_clean(&report), 103);
    return 0;
}

static int test_backup_authority_is_restored(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_OLD_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 104);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.authority_replaced && report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_AUTHORITY_PATH) != MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_AUTHORITY_OLD_PATH) == MOCK_MAX_NODES,
        105);
    CHECK(report_clean(&report), 106);
    return 0;
}

static int test_precommit_rolls_back(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 110);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.generation == 1U && report.files_verified == 2U &&
        report.cleanup_complete && !report.authority_replaced, 111);
    CHECK(find_node("pkgstate/gen/00000000/00000002") == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES, 112);
    CHECK(report_clean(&report), 113);
    return 0;
}

static int test_postcommit_completes(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 120);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_NEW &&
        report.generation == 2U && report.files_verified == 2U &&
        report.cleanup_complete && !report.authority_replaced, 121);
    CHECK(find_node("pkgstate/gen/00000000/00000001") == MOCK_MAX_NODES &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) == MOCK_MAX_NODES, 122);
    CHECK(report_clean(&report), 123);
    return 0;
}

static int test_tamper_repairs_authority_with_ordering(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    struct package_state_authority_view authority;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    size_t application = find_node(
        "pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[application].bytes[0] ^= UINT8_C(0x20);
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 130);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.authority_replaced && report.cleanup_complete &&
        report.rename_count == 2U && report.sync_count == 4U, 131);
    size_t selected = find_node(PACKAGE_SERVICE_AUTHORITY_PATH);
    CHECK(selected != MOCK_MAX_NODES &&
        package_state_authority_parse(nodes[selected].bytes,
            nodes[selected].byte_count, &authority) == PACKAGE_STATE_STATUS_OK &&
        authority.generation == 1U, 132);
    size_t write = first_event(MOCK_EVENT_WRITE_AUTHORITY);
    size_t old = first_event(MOCK_EVENT_RENAME_OLD);
    size_t current = first_event(MOCK_EVENT_RENAME_AUTHORITY);
    size_t journal_unlink = first_event(MOCK_EVENT_UNLINK_JOURNAL);
    CHECK(write < old && old < current && current < journal_unlink, 133);
    CHECK(events[write + 1U] == MOCK_EVENT_SYNC &&
        events[old + 1U] == MOCK_EVENT_SYNC &&
        events[current + 1U] == MOCK_EVENT_SYNC, 134);
    CHECK(report_clean(&report), 135);
    return 0;
}

static int test_extra_file_is_not_complete(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file("pkgstate/gen/00000000/00000002/root/extra",
        (const uint8_t *)"x", 1U, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    CHECK(package_service_recover(&report) == PACKAGE_SERVICE_STATUS_OK, 140);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_OLD &&
        report.authority_replaced && report.cleanup_complete, 141);
    CHECK(report_clean(&report), 142);
    return 0;
}

static int test_no_complete_generation_refuses(
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    size_t application = find_node(
        "pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[application].bytes[1] ^= UINT8_C(1);
    CHECK(package_service_recover(&report) ==
        PACKAGE_SERVICE_STATUS_INCOMPLETE, 150);
    CHECK(report.choice == PACKAGE_STATE_RECOVERY_NONE &&
        !report.authority_replaced && !report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES, 151);
    CHECK(report_clean(&report), 152);
    return 0;
}

static int test_sync_failure_keeps_journal(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_service_report report;
    reset_filesystem();
    add_old_generation(old_database);
    add_new_generation(new_database);
    add_file(PACKAGE_SERVICE_AUTHORITY_PATH, new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, UINT16_C(0444));
    add_file(PACKAGE_SERVICE_JOURNAL_PATH, journal,
        PACKAGE_STATE_JOURNAL_BYTES, UINT16_C(0444));
    size_t application = find_node(
        "pkgstate/gen/00000000/00000002/root/bin/app");
    nodes[application].bytes[0] ^= UINT8_C(1);
    fail_next_sync = true;
    CHECK(package_service_recover(&report) ==
        PACKAGE_SERVICE_STATUS_DURABILITY, 160);
    CHECK(!report.authority_replaced && !report.cleanup_complete &&
        find_node(PACKAGE_SERVICE_JOURNAL_PATH) != MOCK_MAX_NODES &&
        find_node("pkgstate/gen/00000000/00000001") != MOCK_MAX_NODES,
        161);
    CHECK(report.live_file_handles == 0U &&
        report.live_allocations == 0U, 162);
    return 0;
}

int main(void)
{
    static uint8_t old_database[OLD_DATABASE_BYTES];
    static uint8_t new_database[NEW_DATABASE_BYTES];
    uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES];
    int result = package_state_core_host_test_main();

    if (result != 0) {
        return result;
    }
    build_old_database(old_database);
    build_new_database(new_database);
    build_authority(old_authority, old_database, OLD_DATABASE_BYTES);
    build_authority(new_authority, new_database, NEW_DATABASE_BYTES);
    build_journal(journal, old_database, new_database);
    result = test_absent_state_is_distinct();
    if (result == 0) {
        result = test_selected_generation_without_journal(old_database,
            old_authority);
    }
    if (result == 0) {
        result = test_backup_authority_is_restored(old_database,
            old_authority);
    }
    if (result == 0) {
        result = test_precommit_rolls_back(old_database, new_database,
            old_authority, journal);
    }
    if (result == 0) {
        result = test_postcommit_completes(old_database, new_database,
            new_authority, journal);
    }
    if (result == 0) {
        result = test_tamper_repairs_authority_with_ordering(old_database,
            new_database, new_authority, journal);
    }
    if (result == 0) {
        result = test_extra_file_is_not_complete(old_database, new_database,
            new_authority, journal);
    }
    if (result == 0) {
        result = test_no_complete_generation_refuses(new_database,
            new_authority, journal);
    }
    if (result == 0) {
        result = test_sync_failure_keeps_journal(old_database, new_database,
            new_authority, journal);
    }
    if (result != 0) {
        (void)fprintf(stderr, "package service host test failed: %d\n", result);
        return result;
    }
    (void)printf("package service host tests passed\n");
    return 0;
}
