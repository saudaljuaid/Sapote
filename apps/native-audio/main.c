/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/audio.h>
#include <sapote/event.h>
#include <sapote/runtime.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define OPERATION_NS UINT64_C(3000000000)

static int16_t first_pcm[SAPOTE_AUDIO_CHUNK_FRAMES * SAPOTE_AUDIO_CHANNELS];
static int16_t second_pcm[SAPOTE_AUDIO_CHUNK_FRAMES * SAPOTE_AUDIO_CHANNELS];
static int16_t canceled_pcm[SAPOTE_AUDIO_CHUNK_FRAMES * SAPOTE_AUDIO_CHANNELS];

static uint64_t deadline(void)
{
    return sapote_monotonic_ns() + OPERATION_NS;
}

static void fill_pcm(void)
{
    for (size_t frame = 0U; frame < SAPOTE_AUDIO_CHUNK_FRAMES; ++frame) {
        const int16_t first = (frame / 32U) % 2U == 0U ?
            INT16_C(8192) : -INT16_C(8192);
        const int16_t second = (frame / 64U) % 2U == 0U ?
            INT16_C(4096) : -INT16_C(4096);
        const size_t sample = frame * SAPOTE_AUDIO_CHANNELS;

        first_pcm[sample] = first;
        first_pcm[sample + 1U] = first;
        second_pcm[sample] = second;
        second_pcm[sample + 1U] = second;
        canceled_pcm[sample] = INT16_C(30000);
        canceled_pcm[sample + 1U] = -INT16_C(30000);
    }
}

static int wait_writable(sapote_handle_t first, sapote_handle_t second)
{
    struct sapote_wait_item items[2] = {
        {first, SAPOTE_WAIT_WRITABLE, 0U},
        {second, SAPOTE_WAIT_WRITABLE, 0U}
    };
    const long ready = sapote_wait(items, 2U, deadline());

    return ready == 2 && items[0].ready == SAPOTE_WAIT_WRITABLE &&
        items[1].ready == SAPOTE_WAIT_WRITABLE ? 0 : -1;
}

static int run_refusal(void)
{
    if (sapote_audio_open() != -SAPOTE_EACCES) {
        return 10;
    }
    puts("SAPOTE AUDIO REFUSAL PASS capability=EACCES");
    return 0;
}

static int run_proof(void)
{
    struct sapote_wait_item canceled;
    long first_opened;
    long second_opened;
    long leaked;
    sapote_handle_t first;
    sapote_handle_t second;

    fill_pcm();
    first_opened = sapote_audio_open();
    if (first_opened < 0) {
        return 20;
    }
    first = (sapote_handle_t)first_opened;
    second_opened = sapote_audio_open();
    if (second_opened < 0) {
        (void)sapote_audio_close(first);
        return 21;
    }
    second = (sapote_handle_t)second_opened;
    if (sapote_audio_open() != -SAPOTE_EBUSY ||
        sapote_audio_submit(first, first_pcm,
            SAPOTE_AUDIO_CHUNK_BYTES - SAPOTE_AUDIO_FRAME_BYTES) !=
                -SAPOTE_EINVAL ||
        wait_writable(first, second) != 0) {
        (void)sapote_audio_close(second);
        (void)sapote_audio_close(first);
        return 22;
    }
    puts("SAPOTE AUDIO PHASE open-limit-readiness PASS");

    if (sapote_audio_set_volume(first, SAPOTE_AUDIO_VOLUME_UNITY,
            SAPOTE_AUDIO_VOLUME_UNITY / 2U) != 0 ||
        sapote_audio_set_volume(second, SAPOTE_AUDIO_VOLUME_UNITY / 2U,
            SAPOTE_AUDIO_VOLUME_UNITY) != 0 ||
        sapote_audio_submit(first, first_pcm, sizeof(first_pcm)) !=
            (long)sizeof(first_pcm) ||
        sapote_audio_submit(second, second_pcm, sizeof(second_pcm)) !=
            (long)sizeof(second_pcm) ||
        sapote_audio_drain(first, deadline()) != 0 ||
        sapote_audio_drain(second, deadline()) != 0 ||
        wait_writable(first, second) != 0) {
        (void)sapote_audio_close(second);
        (void)sapote_audio_close(first);
        return 23;
    }
    puts("SAPOTE AUDIO PHASE two-stream-mix-drain PASS");

    if (sapote_audio_submit(first, canceled_pcm, sizeof(canceled_pcm)) !=
            (long)sizeof(canceled_pcm) || sapote_audio_cancel(first) != 0 ||
        sapote_audio_drain(first, deadline()) != -SAPOTE_ECANCELED) {
        (void)sapote_audio_close(second);
        (void)sapote_audio_close(first);
        return 24;
    }
    canceled = (struct sapote_wait_item){
        first, SAPOTE_WAIT_WRITABLE | SAPOTE_WAIT_CLOSED, 0U
    };
    if (sapote_wait(&canceled, 1U, deadline()) != 1 ||
        canceled.ready != (SAPOTE_WAIT_WRITABLE | SAPOTE_WAIT_CLOSED)) {
        (void)sapote_audio_close(second);
        (void)sapote_audio_close(first);
        return 25;
    }
    puts("SAPOTE AUDIO PHASE cancel-terminal-readiness PASS");

    if (sapote_audio_close(second) != 0 || sapote_audio_close(first) != 0 ||
        sapote_audio_close(first) != -SAPOTE_ESTALE) {
        return 26;
    }
    leaked = sapote_audio_open();
    if (leaked < 0) {
        return 27;
    }
    puts("SAPOTE AUDIO PASS frames=1024 format=48000/S16LE/2 close=stale teardown=process");
    /* The last typed handle intentionally exercises process-exit cleanup. */
    return 0;
}

int main(int argc, char **argv, char **environment)
{
    (void)environment;
    (void)setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "refusal") == 0) {
        return run_refusal();
    }
    if (argc == 2 && strcmp(argv[1], "proof") == 0) {
        return run_proof();
    }
    return 2;
}
