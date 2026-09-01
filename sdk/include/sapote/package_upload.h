/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_USER_PACKAGE_UPLOAD_H
#define SAPOTE_USER_PACKAGE_UPLOAD_H

#include <stddef.h>
#include <stdint.h>

#include <sapote/abi.h>

struct sapote_package_upload_report {
    uint64_t actual_bytes;
    uint8_t actual_sha256[SAPOTE_PACKAGE_UPLOAD_SHA256_BYTES];
    uint32_t result_flags;
};

long sapote_package_upload_open(void);
long sapote_package_upload_write(
    sapote_handle_t upload,
    const void *bytes,
    size_t byte_count
);
long sapote_package_upload_seal(
    sapote_handle_t upload,
    uint64_t expected_bytes,
    const uint8_t expected_sha256[SAPOTE_PACKAGE_UPLOAD_SHA256_BYTES],
    struct sapote_package_upload_report *report
);
long sapote_package_upload_close(sapote_handle_t upload);

#endif
