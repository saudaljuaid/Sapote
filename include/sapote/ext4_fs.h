/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_EXT4_FS_H
#define SAPOTE_EXT4_FS_H

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

enum sapote_ext4_status sapote_ext4_probe_volume(
    uint32_t controller_index,
    struct sapote_ext4_identity *identity
);

#endif
