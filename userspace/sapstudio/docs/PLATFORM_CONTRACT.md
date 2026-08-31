<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Platform contract

SapStudio uses Sapote's public native ABI through `sapstudio-abi`. Host seams
exercise the same editor logic but do not define a shipping platform.

## Available Sapote services

Sapote provides:

- checked static and bounded dynamic ELF64 loading with private address spaces;
- growable mappings, generation-checked handles, threads, futexes, TLS, and
  saved x87/SSE state;
- Redwood windows, presentation surfaces, keyboard and pointer events;
- monotonic and UTC clocks, deadlines, waits, and entropy;
- read-only System storage and writable application-rooted Data storage;
- bounded PCM output and native networking interfaces.

Redwood also includes a small integrated SapStudio workspace for BMP import,
clip trimming, project save, and BMP export. The freestanding SapStudio image
uses the platform seams listed below.

## Required services

### Application loading

The versioned native ABI authenticates application images and defines launch,
exit, fault, and cleanup behavior.

### Memory

Growable anonymous mappings hold frames, audio, caches, project data, and
decoder state. Allocation failures return to the process.

### Floating point and SIMD

Context switches, interrupts, faults, and syscalls preserve each thread's
x87/SSE state.

### Display and input

SapStudio requires owned presentation buffers, damage submission, keyboard and
pointer events, focus, capture, and window lifecycle notifications.

### Storage

The storage seam provides bounded open, read, write, seek, sync, rename,
replace, enumerate, and metadata operations over the application Data root.
Project saves use synchronized temporary-file replacement.

### Time and scheduling

Playback uses monotonic timestamps, deadlines, waits, readiness notifications,
and threads. Render results do not depend on scheduling order.

### Audio

The audio seam exposes the output format, submission, playback position,
underrun reporting, and teardown. The real-time path avoids filesystem and heap
work.

### Entropy and faults

Entropy supplies identifiers. Process-visible fault reports remain isolated
from the kernel and other processes.

## Contract scope

The software renderer does not require GPU acceleration or an IOMMU. Cloud
storage, collaboration, and plugin hosting are outside this platform contract.
Each service has explicit resource limits, cleanup behavior, and QEMU coverage
through its public ABI.
