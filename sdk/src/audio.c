/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/audio.h>
#include <sapote/runtime.h>

long sapote_audio_open(void)
{
    return sapote_syscall0(SAPOTE_SYS_AUDIO_OPEN);
}

long sapote_audio_submit(
    sapote_handle_t output,
    const int16_t *samples,
    size_t byte_length
)
{
    const struct sapote_audio_submit_request request = {
        sizeof(request), SAPOTE_ABI_VERSION, output,
        (uint64_t)(uintptr_t)samples, (uint32_t)byte_length, 0U
    };

    if (samples == NULL || byte_length != SAPOTE_AUDIO_CHUNK_BYTES) {
        return -SAPOTE_EINVAL;
    }
    return sapote_syscall1(SAPOTE_SYS_AUDIO_SUBMIT,
        (uint64_t)(uintptr_t)&request);
}

long sapote_audio_set_volume(
    sapote_handle_t output,
    uint32_t left_q15,
    uint32_t right_q15
)
{
    const struct sapote_audio_volume_request request = {
        sizeof(request), SAPOTE_ABI_VERSION, output, left_q15, right_q15,
        0U, 0U
    };

    if (left_q15 > SAPOTE_AUDIO_VOLUME_MAX ||
        right_q15 > SAPOTE_AUDIO_VOLUME_MAX) {
        return -SAPOTE_EINVAL;
    }
    return sapote_syscall1(SAPOTE_SYS_AUDIO_VOLUME,
        (uint64_t)(uintptr_t)&request);
}

long sapote_audio_drain(sapote_handle_t output, uint64_t deadline_ns)
{
    return sapote_syscall2(SAPOTE_SYS_AUDIO_DRAIN, output, deadline_ns);
}

long sapote_audio_cancel(sapote_handle_t output)
{
    return sapote_syscall1(SAPOTE_SYS_CANCEL, output);
}

long sapote_audio_close(sapote_handle_t output)
{
    return sapote_handle_close(output);
}
