<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Verification

Sapote checks the kernel on the host and in QEMU. Hardware coverage is limited
to the documented emulated configurations.

## Commands

```sh
make lint
make verify
make smoke
make qemu-tests
```

`make verify` performs a clean build, host tests, ELF and linker inspection,
asset validation, filesystem reconstruction checks, and Rust parser tests. It
rejects warnings, unresolved symbols, unexpected sections, W+X mappings,
floating-point or SIMD instructions in the kernel, modified pinned assets, and
non-reproducible filesystem images.

`make qemu-tests` runs the complete 114-scenario guest suite. The Makefile is
the source of truth for scenario names and expected results.

The `native-canvas` scenario captures `canvas.png` and `canvas.mp4` directly
from QEMU's guest framebuffer while two independently loaded native Canvas
processes are alive. The same run injects hardware keyboard and pointer input;
the serial proof requires both applications to report focus and partial-damage
activity before their resource census is checked.

The `native-sdl` scenario launches the SDL 2 proof application twice. It
captures a screenshot and video, injects keyboard and pointer input into the
first launch, validates non-silent PCM output when QEMU provides its WAV
backend, checks synchronized preference state on the second launch, and then
requires clean process, window, audio, and handle censuses.

[`NATIVE_SCENARIOS.md`](NATIVE_SCENARIOS.md) maps each required native
application proof to its Ring 3 action, exact QEMU scenario, and retained
evidence.

## Coverage

The QEMU suite covers:

- boot, exception entry, APIC routing, clocks, paging, heap, and threads;
- framebuffer, cached surfaces, keyboard, pointer, shell, and Redwood;
- PCI, MSI-X, DMA, xHCI, NVMe, FAT32, and filesystem recovery;
- ELF64 loading, isolated processes, user faults, and BusyBox profiles;
- virtio-net, DHCP, DNS, ICMP, UDP, TCP, HTTP, and downloads to FAT32;
- PCI driver binding, HD Audio codec transport, and NVIDIA device-model
  validation.

Each scenario has an expected exit status and required serial output. Writable
storage tests receive private image copies except for tests that deliberately
reboot with the same synchronized image.

## Focused checks

The repository also contains focused workflows for BusyBox reproduction,
storage, networking, drivers, SapStudio, and Redwood capture. BusyBox binaries
are built twice and compared, then checked against their ELF shape and syscall
profiles. Network tests use an offline peer and retain packet captures when
protocol-level inspection is needed.

## Failure checks

Tests for critical boundaries should also be exercised with an isolated bad
input. Useful examples include a missing boot dependency, a modified fixture,
an invalid user range, an RWX mapping, a wrong guest exit value, or a changed
reference pixel. These temporary mutations are never committed.

## Visual captures

```sh
make capture-redwood-proof
make screenshot-proof
make capture-boot-video
python3 tools/capture-networking.py --iso build/sapote.iso \
    --system build/userspace/sapote-system-fat32.raw \
    --data build/userspace/sapote-data-fat32.raw \
    --output build/networking-capture
```

Screenshots and videos confirm the visible guest interaction. Serial output,
packet captures, retained disk images, and structural checks remain the source
for behavior that cannot be established from a picture.

## Pull requests

A pull request should list the commands run, any relevant QEMU or hardware
limits, and the important behavior not covered by its tests. The required
`build-and-boot` check must pass on the latest commit before merge.
