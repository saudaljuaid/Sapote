/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_EXT4_FS_H
#define SAPOTE_EXT4_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/fat32_fs.h>

#define SAPOTE_EXT4_FILE_REGULAR 1U
#define SAPOTE_EXT4_FILE_DIRECTORY 2U
#define SAPOTE_EXT4_FILE_SYMLINK 3U

enum sapote_ext4_status {
    SAPOTE_EXT4_STATUS_OK = 0,
    SAPOTE_EXT4_STATUS_NULL_ARGUMENT,
    SAPOTE_EXT4_STATUS_VOLUME,
    SAPOTE_EXT4_STATUS_IO,
    SAPOTE_EXT4_STATUS_INVALID,
    SAPOTE_EXT4_STATUS_NOT_FOUND,
    SAPOTE_EXT4_STATUS_NOT_DIRECTORY,
    SAPOTE_EXT4_STATUS_IS_DIRECTORY,
    SAPOTE_EXT4_STATUS_RANGE,
    SAPOTE_EXT4_STATUS_SPECIAL,
    SAPOTE_EXT4_STATUS_COUNT
};

struct sapote_ext4_metadata {
    uint64_t inode;
    uint64_t size;
    uint32_t uid;
    uint32_t gid;
    uint16_t mode;
    uint16_t links;
    uint8_t file_type;
    uint8_t reserved[7];
};

struct sapote_ext4_directory_entry {
    struct sapote_ext4_metadata metadata;
    uint16_t name_length;
    uint8_t name[255];
    uint8_t reserved;
};

struct sapote_ext4_identity {
    uint8_t label[16];
    uint8_t uuid[16];
    uint32_t recovered_transactions;
    uint32_t replayed_blocks;
    uint32_t consumed_slots;
    uint8_t recovery_performed;
    uint8_t reserved[3];
};

struct sapote_ext4_recovery_report {
    uint32_t transactions;
    uint32_t replayed_blocks;
    uint32_t consumed_slots;
    bool performed;
};

struct sapote_ext4_mount_diagnostic {
    enum sapfs_status begin_status;
    int32_t rust_status;
    enum sapfs_status close_status;
    int32_t nvme_close_status;
    int32_t nvme_teardown_status;
    uint32_t nvme_resource_mismatches;
};

enum sapote_ext4_flush_boundary {
    SAPOTE_EXT4_FLUSH_FILESYSTEM_STATE = 0,
    SAPOTE_EXT4_FLUSH_ORDERED_DATA,
    SAPOTE_EXT4_FLUSH_JOURNAL_PAYLOAD,
    SAPOTE_EXT4_FLUSH_COMMIT,
    SAPOTE_EXT4_FLUSH_CHECKPOINT,
    SAPOTE_EXT4_FLUSH_JOURNAL_STATE,
    SAPOTE_EXT4_FLUSH_COUNT
};

enum sapote_ext4_test_storage_kind {
    SAPOTE_EXT4_TEST_STORAGE_WRITE = 0,
    SAPOTE_EXT4_TEST_STORAGE_FLUSH,
    SAPOTE_EXT4_TEST_STORAGE_KIND_COUNT
};

/* Private Rust/C storage callbacks; valid only during a backend operation. */
int32_t sapote_ext4_block_read(
    uintptr_t context,
    uint64_t start_byte,
    uint8_t *destination,
    size_t length
);
int32_t sapote_ext4_block_write(
    uintptr_t context,
    uint64_t start_byte,
    const uint8_t *source,
    size_t length
);
int32_t sapote_ext4_block_flush(uintptr_t context, uint32_t boundary);

void ext4_backend_initialize(void);
enum sapfs_status ext4_backend_mount(enum sapfs_volume volume);
enum sapfs_status ext4_backend_last_mount_status(enum sapfs_volume volume);
bool ext4_backend_mount_diagnostic(enum sapfs_volume volume,
    struct sapote_ext4_mount_diagnostic *diagnostic);
enum sapfs_status ext4_backend_unmount(enum sapfs_volume volume);
enum sapfs_status ext4_backend_sync(enum sapfs_volume volume);
struct sapfs_drive_info ext4_backend_drive(enum sapfs_volume volume);
uint64_t ext4_backend_completion_count(enum sapfs_volume volume);
bool ext4_backend_recovery_report(enum sapfs_volume volume,
    struct sapote_ext4_recovery_report *report);
enum sapfs_status ext4_backend_open(enum sapfs_volume volume,
    const char *path, enum sapfs_access access, sapfs_handle *handle);
enum sapfs_status ext4_backend_close(sapfs_handle handle);
enum sapfs_status ext4_backend_read(sapfs_handle handle,
    uint8_t *destination, size_t capacity, size_t *read_bytes);
enum sapfs_status ext4_backend_pread(sapfs_handle handle,
    uint8_t *destination, size_t capacity, uint64_t offset,
    size_t *read_bytes);
/* Private kernel acceptance probe; this is not installed in the VFS table. */
enum sapfs_status ext4_backend_transaction_probe(enum sapfs_volume volume,
    const char *path, uint64_t offset, const uint8_t *source,
    size_t source_bytes, size_t *written_bytes);
enum sapfs_status ext4_backend_truncate_probe(enum sapfs_volume volume,
    const char *path, uint64_t size);
/* Private ext4-recovery scenario controls; never installed in the VFS table. */
bool ext4_backend_test_configure_power_cut(const char *command_line,
    size_t command_line_length);
bool ext4_backend_test_power_cut_configured(void);
bool ext4_backend_test_fail_storage_once(uint32_t operation_ordinal);
bool ext4_backend_test_storage_failure_observed(
    enum sapote_ext4_test_storage_kind expected_kind);
enum sapfs_status ext4_backend_write(sapfs_handle handle,
    const uint8_t *source, size_t source_bytes, size_t *written_bytes);
enum sapfs_status ext4_backend_seek(sapfs_handle handle, int64_t offset,
    enum sapfs_seek_origin origin, uint64_t *position);
enum sapfs_status ext4_backend_stat_path(enum sapfs_volume volume,
    const char *path, struct sapfs_stat *stat);
enum sapfs_status ext4_backend_list(enum sapfs_volume volume,
    const char *path, struct sapfs_list_entry *entries, size_t capacity,
    size_t *entry_count);
enum sapfs_status ext4_backend_directory_open(enum sapfs_volume volume,
    const char *path, sapfs_handle *handle);
enum sapfs_status ext4_backend_directory_read(sapfs_handle handle,
    struct sapfs_list_entry *entry, bool *present);
enum sapfs_status ext4_backend_directory_close(sapfs_handle handle);
enum sapfs_status ext4_backend_create(enum sapfs_volume volume,
    const char *path);
enum sapfs_status ext4_backend_truncate(enum sapfs_volume volume,
    const char *path, uint64_t size);
enum sapfs_status ext4_backend_mkdir(enum sapfs_volume volume,
    const char *path);
enum sapfs_status ext4_backend_rename(enum sapfs_volume volume,
    const char *source, const char *destination);
enum sapfs_status ext4_backend_unlink(enum sapfs_volume volume,
    const char *path);
enum sapfs_status ext4_backend_rmdir(enum sapfs_volume volume,
    const char *path);
enum sapfs_status ext4_backend_link(enum sapfs_volume volume,
    const char *source, const char *destination);

#endif
