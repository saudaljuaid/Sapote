/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef PHIPIA_FAT32_BACKEND_H
#define PHIPIA_FAT32_BACKEND_H

#include <phipia/fat32_fs.h>

/* Private VFS backend contract; kernel services use the sapfs_* VFS API. */
bool fat32_backend_self_test(size_t *completed_tests);
void fat32_backend_initialize(void);
enum sapfs_status fat32_backend_mount(enum sapfs_volume volume);
enum sapfs_status fat32_backend_unmount(enum sapfs_volume volume);
enum sapfs_status fat32_backend_sync(enum sapfs_volume volume);
struct sapfs_drive_info fat32_backend_drive(enum sapfs_volume volume);
uint64_t fat32_backend_completion_count(enum sapfs_volume volume);
enum sapfs_status fat32_backend_open(
    enum sapfs_volume volume,
    const char *path,
    enum sapfs_access access,
    sapfs_handle *handle
);
enum sapfs_status fat32_backend_close(sapfs_handle handle);
enum sapfs_status fat32_backend_read(
    sapfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    size_t *read_bytes
);
enum sapfs_status fat32_backend_pread(
    sapfs_handle handle,
    uint8_t *destination,
    size_t capacity,
    uint64_t offset,
    size_t *read_bytes
);
enum sapfs_status fat32_backend_write(
    sapfs_handle handle,
    const uint8_t *source,
    size_t source_bytes,
    size_t *written_bytes
);
enum sapfs_status fat32_backend_seek(
    sapfs_handle handle,
    int64_t offset,
    enum sapfs_seek_origin origin,
    uint64_t *position
);
enum sapfs_status fat32_backend_stat_path(
    enum sapfs_volume volume,
    const char *path,
    struct sapfs_stat *stat
);
enum sapfs_status fat32_backend_list(
    enum sapfs_volume volume,
    const char *path,
    struct sapfs_list_entry *entries,
    size_t capacity,
    size_t *entry_count
);
enum sapfs_status fat32_backend_create(
    enum sapfs_volume volume,
    const char *path
);
enum sapfs_status fat32_backend_truncate(
    enum sapfs_volume volume,
    const char *path,
    uint64_t size
);
enum sapfs_status fat32_backend_mkdir(
    enum sapfs_volume volume,
    const char *path
);
enum sapfs_status fat32_backend_rename(
    enum sapfs_volume volume,
    const char *source,
    const char *destination
);
enum sapfs_status fat32_backend_unlink(
    enum sapfs_volume volume,
    const char *path
);
enum sapfs_status fat32_backend_rmdir(
    enum sapfs_volume volume,
    const char *path
);
enum sapfs_status fat32_backend_link(
    enum sapfs_volume volume,
    const char *source,
    const char *destination
);

#endif
