/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  Sapote audio backend addition for SDL 2.32.10. This file is distributed
  under the same zlib license as SDL.
*/

#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_SAPOTE

#include "../SDL_audio_c.h"
#include "../SDL_sysaudio.h"

#include <sapote/audio.h>
#include <sapote/event.h>
#include <sapote/runtime.h>

struct SDL_PrivateAudioData
{
    sapote_handle_t output;
    Uint8 buffer[SAPOTE_AUDIO_CHUNK_BYTES];
    SDL_bool failed;
};

static uint64_t SAPOTEAUDIO_Deadline(void)
{
    const uint64_t now = sapote_monotonic_ns();
    const uint64_t allowance = UINT64_C(1000000000);
    return now > UINT64_MAX - allowance ? UINT64_MAX : now + allowance;
}

static void SAPOTEAUDIO_WaitDevice(_THIS)
{
    struct sapote_wait_item item;
    long result;

    if (_this->hidden->failed == SDL_TRUE) {
        return;
    }
    item.handle = _this->hidden->output;
    item.interests = SAPOTE_WAIT_WRITABLE | SAPOTE_WAIT_CLOSED;
    item.ready = 0U;
    result = sapote_wait(&item, 1U, SAPOTEAUDIO_Deadline());
    if (result != 1 || item.ready != SAPOTE_WAIT_WRITABLE) {
        _this->hidden->failed = SDL_TRUE;
        SDL_OpenedAudioDeviceDisconnected(_this);
    }
}

static void SAPOTEAUDIO_PlayDevice(_THIS)
{
    long result;

    if (_this->hidden->failed == SDL_TRUE) {
        return;
    }
    result = sapote_audio_submit(_this->hidden->output,
        (const int16_t *)(const void *)_this->hidden->buffer,
        sizeof(_this->hidden->buffer));
    if (result >= 0) {
        result = sapote_audio_drain(_this->hidden->output,
            SAPOTEAUDIO_Deadline());
    }
    if (result < 0) {
        _this->hidden->failed = SDL_TRUE;
        SDL_OpenedAudioDeviceDisconnected(_this);
    }
}

static Uint8 *SAPOTEAUDIO_GetDeviceBuf(_THIS)
{
    return _this->hidden->buffer;
}

static void SAPOTEAUDIO_CloseDevice(_THIS)
{
    if (_this->hidden != NULL) {
        (void)sapote_audio_cancel(_this->hidden->output);
        (void)sapote_audio_close(_this->hidden->output);
        SDL_free(_this->hidden);
        _this->hidden = NULL;
    }
}

static int SAPOTEAUDIO_OpenDevice(_THIS, const char *devname)
{
    struct SDL_PrivateAudioData *hidden;
    long result;
    (void)devname;

    if (_this->iscapture) {
        return SDL_SetError("Sapote SDL audio capture is unsupported");
    }
    hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*hidden));
    if (hidden == NULL) {
        return SDL_OutOfMemory();
    }
    result = sapote_audio_open();
    if (result < 0) {
        SDL_free(hidden);
        return SDL_SetError("Sapote audio open failed: %ld", result);
    }
    hidden->output = (sapote_handle_t)result;
    _this->hidden = hidden;
    _this->spec.freq = (int)SAPOTE_AUDIO_SAMPLE_RATE;
    _this->spec.format = AUDIO_S16SYS;
    _this->spec.channels = (Uint8)SAPOTE_AUDIO_CHANNELS;
    _this->spec.samples = (Uint16)SAPOTE_AUDIO_CHUNK_FRAMES;
    SDL_CalculateAudioSpec(&_this->spec);
    if (_this->spec.size != SAPOTE_AUDIO_CHUNK_BYTES) {
        SAPOTEAUDIO_CloseDevice(_this);
        return SDL_SetError("Sapote SDL audio format calculation changed");
    }
    return 0;
}

static SDL_bool SAPOTEAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = SAPOTEAUDIO_OpenDevice;
    impl->WaitDevice = SAPOTEAUDIO_WaitDevice;
    impl->PlayDevice = SAPOTEAUDIO_PlayDevice;
    impl->GetDeviceBuf = SAPOTEAUDIO_GetDeviceBuf;
    impl->CloseDevice = SAPOTEAUDIO_CloseDevice;
    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    impl->HasCaptureSupport = SDL_FALSE;
    impl->SupportsNonPow2Samples = SDL_FALSE;
    return SDL_TRUE;
}

AudioBootStrap SAPOTEAUDIO_bootstrap = {
    "sapote",
    "Sapote native mixed PCM output",
    SAPOTEAUDIO_Init,
    SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_SAPOTE */
