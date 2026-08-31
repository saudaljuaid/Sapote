/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_ABI_AUDIO_H
#define SAPOTE_ABI_AUDIO_H

#include <sapote/abi/base.h>

#define SAPOTE_AUDIO_SAMPLE_RATE UINT32_C(48000)
#define SAPOTE_AUDIO_CHANNELS UINT32_C(2)
#define SAPOTE_AUDIO_BITS_PER_SAMPLE UINT32_C(16)
#define SAPOTE_AUDIO_FRAME_BYTES UINT32_C(4)
#define SAPOTE_AUDIO_CHUNK_FRAMES UINT32_C(1024)
#define SAPOTE_AUDIO_CHUNK_BYTES \
    (SAPOTE_AUDIO_CHUNK_FRAMES * SAPOTE_AUDIO_FRAME_BYTES)
#define SAPOTE_AUDIO_MAX_STREAMS UINT32_C(2)

/* Per-channel unsigned Q15 gain: zero is silent and 32768 is unity. */
#define SAPOTE_AUDIO_VOLUME_SILENT UINT32_C(0)
#define SAPOTE_AUDIO_VOLUME_UNITY UINT32_C(32768)
#define SAPOTE_AUDIO_VOLUME_MAX SAPOTE_AUDIO_VOLUME_UNITY

struct sapote_audio_submit_request {
    uint32_t size;
    uint32_t version;
    sapote_handle_t handle;
    uint64_t buffer;
    uint32_t length;
    uint32_t flags;
} __attribute__((packed));

struct sapote_audio_volume_request {
    uint32_t size;
    uint32_t version;
    sapote_handle_t handle;
    uint32_t left_q15;
    uint32_t right_q15;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct sapote_audio_submit_request) == 32U,
    "Sapote audio-submit ABI changed");
_Static_assert(sizeof(struct sapote_audio_volume_request) == 32U,
    "Sapote audio-volume ABI changed");

#endif
