<!-- SPDX-License-Identifier: GPL-3.0-only -->

# SDL 2 on Sapote

Sapote carries the official SDL 2.32.10 source and a first-class `__SAPOTE__`
platform backend. It is built reproducibly into the SDK as `libSDL2.a`; public
headers are installed under `include/SDL2`. Applications include `SDL.h` with
that directory on their include path and explicitly link the static library.

The backend is not a Linux compatibility shim and does not use SDL's dummy
video or audio drivers. Redwood owns the window surface and receives bounded
damage rectangles through the public native window ABI. Input becomes normal
SDL keyboard, text, pointer, focus, and quit events. Event waits use Sapote's
public multi-handle wait call, while a native timer handle implements SDL's
wakeup hook without polling.

Audio opens Sapote's public PCM service and submits ordinary SDL mixed buffers.
The accepted profile is 48 kHz, stereo S16 with a bounded 1,024-frame period.
Capture audio is not supported. Threads use SDK pthreads plus the public futex
syscalls for SDL synchronization. The monotonic clock backs ticks, performance
counters, delays, and wait deadlines.

`SDL_GetBasePath()` returns `System:`. `SDL_GetPrefPath()` creates an 8.3-safe,
deterministic per-application directory below `Data:SDL`; organization and app
strings are hashed rather than copied into a path. This prevents traversal and
keeps the interface usable on the current FAT data volume. The hash is a
namespace mapping, not an authentication primitive.

## Deliberate exclusions

The initial port disables dynamic object loading, HIDAPI, haptics, joysticks,
sensors, capture audio, OpenGL, Vulkan, and hardware render drivers. Thread
detach blocks until join because Sapote ABI v1 cannot reclaim a detached native
thread safely. These are explicit compatibility gaps, not silent dummy-driver
fallbacks.

The repository does not call the SDL application layer complete until its QEMU
proof exercises window creation/presentation, injected input, non-silent PCM,
persistent state across reboot, clean exit, and resource census from Ring 3.
