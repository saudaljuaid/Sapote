/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_ABI_PACKAGE_H
#define SAPOTE_ABI_PACKAGE_H

#include <sapote/abi/base.h>

#define SAPOTE_PACKAGE_UPLOAD_SHA256_BYTES UINT32_C(32)
#define SAPOTE_PACKAGE_UPLOAD_WRITE_MAX UINT32_C(4096)
#define SAPOTE_PACKAGE_UPLOAD_MAX_BYTES UINT32_C(16777216)

#define SAPOTE_PACKAGE_UPLOAD_SEALED UINT32_C(1)
#define SAPOTE_PACKAGE_UPLOAD_DURABLE UINT32_C(2)

struct sapote_package_upload_write_request {
    uint32_t size;
    uint32_t version;
    sapote_handle_t handle;
    uint64_t buffer;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

struct sapote_package_upload_seal_request {
    uint32_t size;
    uint32_t version;
    sapote_handle_t handle;
    uint64_t expected_bytes;
    uint8_t expected_sha256[SAPOTE_PACKAGE_UPLOAD_SHA256_BYTES];
    uint64_t actual_bytes;
    uint8_t actual_sha256[SAPOTE_PACKAGE_UPLOAD_SHA256_BYTES];
    uint32_t result_flags;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct sapote_package_upload_write_request) == 32U,
    "Sapote package-upload write ABI changed");
_Static_assert(sizeof(struct sapote_package_upload_seal_request) == 104U,
    "Sapote package-upload seal ABI changed");

#endif
