/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_USER_AUDIO_H
#define SAPOTE_USER_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include <sapote/abi.h>

long sapote_audio_open(void);
long sapote_audio_submit(sapote_handle_t output, const int16_t *samples,
    size_t byte_length);
long sapote_audio_set_volume(sapote_handle_t output, uint32_t left_q15,
    uint32_t right_q15);
long sapote_audio_drain(sapote_handle_t output, uint64_t deadline_ns);
long sapote_audio_cancel(sapote_handle_t output);
long sapote_audio_close(sapote_handle_t output);

#endif
