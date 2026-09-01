/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_USER_PACKAGE_CONTROL_H
#define SAPOTE_USER_PACKAGE_CONTROL_H

#include <stddef.h>
#include <stdint.h>

#include <sapote/abi.h>

struct sapote_package_control_report {
    uint64_t repository_version;
    uint64_t generation;
    uint32_t plan_count;
    uint32_t attached_count;
    uint32_t result_flags;
};

struct sapote_package_control_item {
    uint32_t index;
    uint32_t identifier_bytes;
    uint32_t version_bytes;
    uint32_t path_bytes;
    uint64_t package_bytes;
    uint8_t package_sha256[SAPOTE_PACKAGE_UPLOAD_SHA256_BYTES];
    char identifier[SAPOTE_PACKAGE_CONTROL_TEXT_BYTES];
    char version[SAPOTE_PACKAGE_CONTROL_TEXT_BYTES];
    char download_path[SAPOTE_PACKAGE_CONTROL_PATH_BYTES];
};

long sapote_package_control_open_install(
    sapote_handle_t repository_upload,
    const char *identifier,
    size_t identifier_bytes,
    struct sapote_package_control_report *report
);

long sapote_package_control_item(
    sapote_handle_t control,
    uint32_t index,
    struct sapote_package_control_item *item
);

long sapote_package_control_attach(
    sapote_handle_t control,
    uint32_t index,
    sapote_handle_t package_upload,
    struct sapote_package_control_report *report
);

long sapote_package_control_commit(
    sapote_handle_t control,
    struct sapote_package_control_report *report
);

long sapote_package_control_close(sapote_handle_t control);

#endif
