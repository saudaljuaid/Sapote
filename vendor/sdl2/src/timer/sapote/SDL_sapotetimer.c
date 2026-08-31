/*
  Simple DirectMedia Layer
  Copyright (C) 1997-2025 Sam Lantinga <slouken@libsdl.org>

  Sapote timer backend addition for SDL 2.32.10. This file is distributed
  under the same zlib license as SDL.
*/

#include "../../SDL_internal.h"

#ifdef SDL_TIMER_SAPOTE

#include "SDL_timer.h"

#include <sapote/runtime.h>

static Uint64 sapote_ticks_origin;
static SDL_bool sapote_ticks_started = SDL_FALSE;

void SDL_TicksInit(void)
{
    if (!sapote_ticks_started) {
        sapote_ticks_origin = sapote_monotonic_ns();
        sapote_ticks_started = SDL_TRUE;
    }
}

void SDL_TicksQuit(void)
{
    sapote_ticks_started = SDL_FALSE;
    sapote_ticks_origin = 0U;
}

Uint64 SDL_GetTicks64(void)
{
    Uint64 now;

    if (!sapote_ticks_started) {
        SDL_TicksInit();
    }
    now = sapote_monotonic_ns();
    return now >= sapote_ticks_origin ?
        (now - sapote_ticks_origin) / UINT64_C(1000000) : 0U;
}

Uint64 SDL_GetPerformanceCounter(void)
{
    return sapote_monotonic_ns();
}

Uint64 SDL_GetPerformanceFrequency(void)
{
    return UINT64_C(1000000000);
}

void SDL_Delay(Uint32 milliseconds)
{
    const Uint64 now = sapote_monotonic_ns();
    const Uint64 delta = (Uint64)milliseconds * UINT64_C(1000000);
    const Uint64 deadline = delta > UINT64_MAX - now ? UINT64_MAX :
        now + delta;

    (void)sapote_sleep_until(deadline);
}

#endif /* SDL_TIMER_SAPOTE */
