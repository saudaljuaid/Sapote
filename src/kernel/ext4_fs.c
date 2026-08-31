/* SPDX-License-Identifier: GPL-3.0-only */
/*
 * Native block boundary for the checked Rust ext4 parser. Mount ownership will
 * live in the VFS; this first boundary deliberately admits media read-only.
 */
#include <stddef.h>
#include <stdint.h>

#include <sapote/ext4_fs.h>
#include <sapote/nvme.h>

extern int32_t sapote_ext4_probe(
    uintptr_t context,
    struct sapote_ext4_identity *identity
);

static void copy_bytes(
    uint8_t *destination,
    const uint8_t *source,
    size_t length
)
{
    for (size_t index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
}

/* Called only by Rust while sapote_ext4_probe_volume owns this session. */
int32_t sapote_ext4_block_read(
    uintptr_t context,
    uint64_t start_byte,
    uint8_t *destination,
    size_t length
)
{
    struct nvme_volume_session *session =
        (struct nvme_volume_session *)context;
    uint8_t block[NVME_BLOCK_BYTES];
    uint64_t capacity;
    uint64_t position = start_byte;
    size_t remaining = length;

    if (session == NULL || destination == NULL || !session->active ||
        session->logical_block_bytes == 0U ||
        session->logical_block_bytes > sizeof(block) ||
        session->namespace_blocks >
            UINT64_MAX / session->logical_block_bytes) {
        return -1;
    }
    capacity = session->namespace_blocks * session->logical_block_bytes;
    if (start_byte > capacity || length > capacity - start_byte) {
        return -1;
    }
    while (remaining != 0U) {
        const uint64_t lba = position / session->logical_block_bytes;
        const size_t within =
            (size_t)(position % session->logical_block_bytes);
        size_t chunk = session->logical_block_bytes - within;

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (nvme_volume_read(session, lba, block,
                session->logical_block_bytes) != NVME_STATUS_OK) {
            return -1;
        }
        copy_bytes(destination, &block[within], chunk);
        destination += chunk;
        position += chunk;
        remaining -= chunk;
    }
    return 0;
}

enum sapote_ext4_status sapote_ext4_probe_volume(
    uint32_t controller_index,
    struct sapote_ext4_identity *identity
)
{
    struct nvme_volume_session session = {0};
    int32_t result;

    if (identity == NULL) {
        return SAPOTE_EXT4_STATUS_NULL_ARGUMENT;
    }
    if (nvme_volume_open(&session, controller_index, false) !=
        NVME_STATUS_OK) {
        return SAPOTE_EXT4_STATUS_VOLUME;
    }
    result = sapote_ext4_probe((uintptr_t)&session, identity);
    if (nvme_volume_close(&session) != NVME_STATUS_OK) {
        return SAPOTE_EXT4_STATUS_VOLUME;
    }
    if (result < SAPOTE_EXT4_STATUS_OK ||
        result >= SAPOTE_EXT4_STATUS_COUNT) {
        return SAPOTE_EXT4_STATUS_INVALID;
    }
    return (enum sapote_ext4_status)result;
}
