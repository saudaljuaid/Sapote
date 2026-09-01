/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_PACKAGE_SERVICE_H
#define SAPOTE_PACKAGE_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/fat32_fs.h>
#include <sapote/package_builder.h>
#include <sapote/package_state.h>

/* Deliberately 8.3-safe so recovery has one layout on FAT32 and ext4. */
#define PACKAGE_SERVICE_STATE_DIRECTORY "pkgstate"
#define PACKAGE_SERVICE_AUTHORITY_PATH "pkgstate/auth.bin"
#define PACKAGE_SERVICE_AUTHORITY_NEW_PATH "pkgstate/auth.new"
#define PACKAGE_SERVICE_AUTHORITY_OLD_PATH "pkgstate/auth.old"
#define PACKAGE_SERVICE_JOURNAL_PATH "pkgstate/txn.bin"
#define PACKAGE_SERVICE_JOURNAL_NEW_PATH "pkgstate/txn.new"

/* Two candidates and scratch state must fit Sapote's bounded 16 MiB heap. */
#define PACKAGE_SERVICE_MAX_DATABASE_BYTES (4U * 1024U * 1024U)
#define PACKAGE_SERVICE_IO_BYTES 4096U
#define PACKAGE_SERVICE_MAX_TREE_ENTRIES 8192U

enum package_service_status {
    PACKAGE_SERVICE_STATUS_OK = 0,
    PACKAGE_SERVICE_STATUS_NULL_ARGUMENT,
    PACKAGE_SERVICE_STATUS_BUSY,
    PACKAGE_SERVICE_STATUS_ABSENT,
    PACKAGE_SERVICE_STATUS_UNAVAILABLE,
    PACKAGE_SERVICE_STATUS_FILESYSTEM,
    PACKAGE_SERVICE_STATUS_RESOURCE,
    PACKAGE_SERVICE_STATUS_STATE,
    PACKAGE_SERVICE_STATUS_INCOMPLETE,
    PACKAGE_SERVICE_STATUS_IMMUTABLE_FILE,
    PACKAGE_SERVICE_STATUS_NAMESPACE,
    PACKAGE_SERVICE_STATUS_DURABILITY,
    PACKAGE_SERVICE_STATUS_CLEANUP,
    PACKAGE_SERVICE_STATUS_COUNT
};

struct package_service_report {
    enum package_service_status status;
    enum sapfs_status filesystem_status;
    enum package_state_status state_status;
    enum package_state_recovery_choice choice;
    uint64_t generation;
    uint64_t bytes_read;
    uint64_t bytes_written;
    uint32_t files_verified;
    uint32_t files_staged;
    uint32_t tree_entries;
    uint32_t sync_count;
    uint32_t rename_count;
    uint32_t peak_file_handles;
    uint32_t peak_allocations;
    uint32_t live_file_handles;
    uint32_t live_allocations;
    bool journal_present;
    bool prepared;
    bool committed;
    bool authority_replaced;
    bool cleanup_complete;
};

/*
 * Privileged recovery entry point. The caller must invoke it only after the
 * data volume and heap are online, before package-owned executables are used.
 */
enum package_service_status package_service_recover(
    struct package_service_report *report
);

struct package_service_prepare_request {
    const struct package_builder_workspace *builder;
    const uint8_t *database;
    size_t database_bytes;
};

/*
 * Writes and verifies a complete target generation, then durably publishes a
 * prepared journal while leaving the current authority unchanged.  Therefore
 * success is not installation success: reboot recovery rolls this generation
 * back until a separate authority-commit phase exists.  Fresh-store bootstrap
 * and repair are not admitted by this entry point.
 */
enum package_service_status package_service_prepare(
    const struct package_service_prepare_request *request,
    struct package_service_report *report
);

/*
 * Commits an already prepared transaction.  The service revalidates the
 * journal, both complete generations, and the current base authority before
 * durably selecting the target.  Once report.committed is true, recovery will
 * select the target even if best-effort cleanup returns an error.
 */
enum package_service_status package_service_commit(
    struct package_service_report *report
);

const char *package_service_status_string(enum package_service_status status);

#endif
