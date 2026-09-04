/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_FAT32_FS_H
#define PHIPIA_FAT32_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <phipia/fat32.h>

#define SAPFS_MAX_MOUNTS 2U
#define SAPFS_MAX_HANDLES 64U
#define SAPFS_CACHE_ENTRIES 4U
#define SAPFS_MAX_PATH FAT32_PATH_BYTES
#define SAPFS_MAX_COMPONENT_BYTES 256U
#define SAPFS_MAX_DEPTH 16U
#define SAPFS_MAX_FILE_BYTES UINT32_C(16777216)
#define SAPFS_MAX_LIST_ENTRIES 64U

enum sapfs_volume {
    SAPFS_VOLUME_SYSTEM = 0,
    SAPFS_VOLUME_DATA,
    SAPFS_VOLUME_COUNT
};

enum sapfs_access {
    SAPFS_ACCESS_READ = 1U,
    SAPFS_ACCESS_WRITE = 2U,
    SAPFS_ACCESS_READ_WRITE = 3U
};

enum sapfs_seek_origin {
    SAPFS_SEEK_START = 0,
    SAPFS_SEEK_CURRENT,
    SAPFS_SEEK_END
};

enum sapfs_status {
    SAPFS_STATUS_OK = 0,
    SAPFS_STATUS_ABSENT,
    SAPFS_STATUS_CORRUPT,
    SAPFS_STATUS_IO,
    SAPFS_STATUS_INVALID_ARGUMENT,
    SAPFS_STATUS_NOT_MOUNTED,
    SAPFS_STATUS_ALREADY_MOUNTED,
    SAPFS_STATUS_READ_ONLY,
    SAPFS_STATUS_NOT_FOUND,
    SAPFS_STATUS_EXISTS,
    SAPFS_STATUS_NOT_DIRECTORY,
    SAPFS_STATUS_IS_DIRECTORY,
    SAPFS_STATUS_NOT_EMPTY,
    SAPFS_STATUS_BUSY,
    SAPFS_STATUS_NO_HANDLES,
    SAPFS_STATUS_STALE_HANDLE,
    SAPFS_STATUS_ACCESS,
    SAPFS_STATUS_RANGE,
    SAPFS_STATUS_FULL,
    SAPFS_STATUS_DIRECTORY_FULL,
    SAPFS_STATUS_NAME,
    SAPFS_STATUS_PATH,
    SAPFS_STATUS_WRITEBACK,
    SAPFS_STATUS_RESET,
    SAPFS_STATUS_COUNT
};

typedef uint64_t sapfs_handle;
typedef uint64_t sapfs_directory_handle;

struct sapfs_stat {
    uint64_t size;
    uint64_t object_id;
    uint32_t first_cluster;
    uint32_t cluster_count;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t links;
    uint8_t attributes;
    bool directory;
    bool read_only;
};

struct sapfs_list_entry {
    char name[SAPFS_MAX_COMPONENT_BYTES];
    uint64_t size;
    uint64_t object_id;
    uint16_t mode;
    uint8_t attributes;
    bool directory;
};

struct sapfs_drive_info {
    enum sapfs_volume volume;
    uint32_t volume_id;
    uint64_t total_bytes;
    uint64_t free_bytes;
    bool present;
    bool mounted;
    bool read_only;
    bool healthy;
};

bool sapfs_self_test(size_t *completed_tests);
void sapfs_initialize(void);
enum sapfs_status sapfs_mount(enum sapfs_volume volume);
enum sapfs_status sapfs_unmount(enum sapfs_volume volume);
enum sapfs_status sapfs_sync(enum sapfs_volume volume);
struct sapfs_drive_info sapfs_drive(enum sapfs_volume volume);
uint64_t sapfs_completion_count(enum sapfs_volume volume);
enum sapfs_status sapfs_open(
    enum sapfs_volume volume,
    const char *path,
    enum sapfs_access access,
    sapfs_handle *handle
);
enum sapfs_status sapfs_close(sapfs_handle handle);
enum sapfs_status sapfs_read(
    sapfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
);
enum sapfs_status sapfs_pread(
    sapfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    uint64_t offset,
    size_t *read_bytes
);
enum sapfs_status sapfs_write(
    sapfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
);
enum sapfs_status sapfs_seek(
    sapfs_handle handle,
    int64_t offset,
    enum sapfs_seek_origin origin,
    uint64_t *position
);
enum sapfs_status sapfs_stat_path(
    enum sapfs_volume volume,
    const char *path,
    struct sapfs_stat *stat
);
enum sapfs_status sapfs_list(
    enum sapfs_volume volume,
    const char *path,
    struct sapfs_list_entry *entries,
    size_t capacity,
    size_t *entry_count
);
enum sapfs_status sapfs_directory_open(
    enum sapfs_volume volume,
    const char *path,
    sapfs_directory_handle *handle
);
enum sapfs_status sapfs_directory_read(
    sapfs_directory_handle handle,
    struct sapfs_list_entry *entry,
    bool *present
);
enum sapfs_status sapfs_directory_close(sapfs_directory_handle handle);
enum sapfs_status sapfs_create(enum sapfs_volume volume, const char *path);
enum sapfs_status sapfs_truncate(
    enum sapfs_volume volume,
    const char *path,
    uint64_t size
);
enum sapfs_status sapfs_mkdir(enum sapfs_volume volume, const char *path);
enum sapfs_status sapfs_rename(
    enum sapfs_volume volume,
    const char *source,
    const char *destination
);
enum sapfs_status sapfs_unlink(enum sapfs_volume volume, const char *path);
enum sapfs_status sapfs_rmdir(enum sapfs_volume volume, const char *path);
enum sapfs_status sapfs_link(
    enum sapfs_volume volume,
    const char *source,
    const char *destination
);
const char *sapfs_status_string(enum sapfs_status status);

#endif
