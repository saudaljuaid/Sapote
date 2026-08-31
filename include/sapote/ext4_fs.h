/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_EXT4_FS_H
#define SAPOTE_EXT4_FS_H

#include <stddef.h>
#include <stdint.h>

enum sapote_ext4_status {
    SAPOTE_EXT4_STATUS_OK = 0,
    SAPOTE_EXT4_STATUS_NULL_ARGUMENT,
    SAPOTE_EXT4_STATUS_VOLUME,
    SAPOTE_EXT4_STATUS_IO,
    SAPOTE_EXT4_STATUS_INVALID,
    SAPOTE_EXT4_STATUS_COUNT
};

struct sapote_ext4_identity {
    uint8_t label[16];
    uint8_t uuid[16];
};

/* Private Rust/C reader callback; valid only during a synchronous probe. */
int32_t sapote_ext4_block_read(
    uintptr_t context,
    uint64_t start_byte,
    uint8_t *destination,
    size_t length
);

enum sapote_ext4_status sapote_ext4_probe_volume(
    uint32_t controller_index,
    struct sapote_ext4_identity *identity
);

#endif
