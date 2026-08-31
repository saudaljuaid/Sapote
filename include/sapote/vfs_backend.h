/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_VFS_BACKEND_H
#define SAPOTE_VFS_BACKEND_H

#include <sapote/fat32_fs.h>

/*
 * Concrete filesystem contract owned by the VFS. Backend handles and path
 * cookies never escape vfs.c; callers see only VFS file descriptions and
 * directory iterators.
 */
struct vfs_backend_ops {
    enum sapfs_status (*mount)(enum sapfs_volume volume);
    enum sapfs_status (*unmount)(enum sapfs_volume volume);
    enum sapfs_status (*sync)(enum sapfs_volume volume);
    struct sapfs_drive_info (*drive)(enum sapfs_volume volume);
    uint64_t (*completion_count)(enum sapfs_volume volume);
    enum sapfs_status (*open)(enum sapfs_volume volume, const char *path,
        enum sapfs_access access, sapfs_handle *handle);
    enum sapfs_status (*close)(sapfs_handle handle);
    enum sapfs_status (*read)(sapfs_handle handle, uint8_t *destination,
        size_t capacity, size_t *read_bytes);
    enum sapfs_status (*write)(sapfs_handle handle, const uint8_t *source,
        size_t source_bytes, size_t *written_bytes);
    enum sapfs_status (*seek)(sapfs_handle handle, int64_t offset,
        enum sapfs_seek_origin origin, uint32_t *position);
    enum sapfs_status (*stat_path)(enum sapfs_volume volume,
        const char *path, struct sapfs_stat *stat);
    enum sapfs_status (*list)(enum sapfs_volume volume, const char *path,
        struct sapfs_list_entry *entries, size_t capacity,
        size_t *entry_count);
    enum sapfs_status (*create)(enum sapfs_volume volume, const char *path);
    enum sapfs_status (*truncate)(enum sapfs_volume volume, const char *path,
        uint32_t size);
    enum sapfs_status (*mkdir)(enum sapfs_volume volume, const char *path);
    enum sapfs_status (*rename)(enum sapfs_volume volume, const char *source,
        const char *destination);
    enum sapfs_status (*unlink)(enum sapfs_volume volume, const char *path);
    enum sapfs_status (*rmdir)(enum sapfs_volume volume, const char *path);
};

#endif
