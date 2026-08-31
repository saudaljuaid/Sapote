<!-- SPDX-License-Identifier: GPL-3.0-only -->

# High Definition Audio

Sapote has one deliberately narrow, kernel-owned PCM playback proof for the
QEMU ICH9 controller and `hda-duplex` codec. It is a real converter route and a
real cyclic output DMA stream, but it is not yet an application audio service.

The implementation follows the Intel High Definition Audio 1.0a controller
contract and derives its codec profile from QEMU's published `hda-codec.c` and
`hda-codec-common.h` descriptions. It does not select node numbers by identity.
It walks the audio function group's bounded widget range, finds a stereo audio
output converter that advertises 48 kHz and 16-bit PCM, finds an output-capable
pin, and proves that the pin's direct connection list names that converter. The
current QEMU codec exposes function group 1, DAC 2, and output pin 3; those
numbers are evidence recorded after discovery, not configuration constants.

## Fixed PCM profile

The only accepted profile is:

- signed little-endian 16-bit PCM;
- two interleaved channels;
- 48,000 frames per second;
- stream format `0x0011`;
- stream tag 1, starting at channel 0;
- 1,024 frames / 4,096 bytes in one page;
- two 2,048-byte BDL periods, each with interrupt-on-completion status;
- a deterministic 750 Hz square wave at amplitude +/-8192, identical on both
  channels.

The conservative format is inside QEMU's advertised 16-96 kHz signed-16-bit
profile. The waveform is generated with integer operations; kernel code does
not use floating point, MMX, or SIMD.

The codec route is explicitly put in D0, its pin is set to output, and the DAC
is assigned the same tag and format as the controller stream. Read-back verbs
must return the selected pin control, converter tag, and converter format before
DMA starts.

## DMA ownership and playback evidence

Four typed below-4-GiB DMA allocations exist during the proof:

1. CORB command ring;
2. RIRB response ring;
3. BDL page;
4. immutable PCM page.

All four are initialized while CPU-owned and named in one bus-master request.
The first enable attempt, before ownership transfer, must fail with
`DMA_NOT_PREPARED`. Only after all four allocations are device-owned may PCI bus
mastering be enabled. The BDL and PCM page are never modified while the device
owns them.

The first output stream descriptor is located after all input descriptors, as
required by GCAP. Sapote stops and resets it, programs BDL base, cyclic buffer
length, LVI 1, format and tag, then sets RUN. The proof accepts playback only
after both forms of independent controller evidence appear within one second:

- LPIB changes to another in-range byte position;
- a BDL buffer-completion status arrives and is acknowledged.

The existing QEMU scenario uses `-audiodev none`. Therefore this proves that the
emulated codec consumed the programmed PCM stream, not that a host speaker or
WAV backend emitted audible sound. The deterministic PCM hash proves what bytes
were made available to the device; it is not an acoustic-capture claim.

## Bounded service and underrun recovery

This increment intentionally polls one stream. Each service step performs a
fixed number of MMIO reads/writes and the proof is bounded by both one second
and 1,000,000 service steps. No stream or global interrupt is enabled.

A buffer completion is acknowledged and counted. A FIFO error is acknowledged,
then the descriptor is stopped, reset, completely reprogrammed and restarted.
At most three such recoveries are allowed. A descriptor error, an out-of-range
LPIB, a fourth FIFO error, or failure of any stop/reset/start handshake fails the
proof. Pure controls independently exercise completion/underrun/fatal status
classification; QEMU is not required to manufacture an underrun in a healthy
run.

## Teardown and lock model

`audio_active` is the one-controller lock. `audio_prove()` is synchronous and
one-shot, and the PCI resource layer requires maskable interrupts disabled
while the claim owns bus mastering. There is no background worker, callback,
user mapping, shared mutable stream, or lock ordering with another subsystem.

Teardown has one allowed order:

```text
clear output-stream RUN and observe it clear
reset the output stream and observe reset set, then clear
stop RIRB and CORB
put the whole controller in reset and observe it
withdraw PCI bus mastering
transfer PCM, BDL, RIRB and CORB ownership back to the CPU
release all four DMA allocations
unmap BAR and release the PCI claim
```

Any partial bring-up follows the same release function. The scenario requires
the frame, paging, DMA, PCI, vector, MSI-X and interrupt-state census to equal
the pre-proof census, with no claim, allocation, mapping or bus master left.

## Supported and refused

Supported now:

- one QEMU ICH9 HDA controller and one discovered fixed direct output route;
- one cyclic kernel-owned 48 kHz / S16LE / stereo playback stream;
- topology/format/read-back validation, polled completion service, bounded FIFO
  recovery, and complete resource teardown.

Still explicitly refused:

- any native/userspace audio handle or syscall;
- application PCM writes, mixer, per-stream or master volume API;
- more than one route or stream, input capture, hotplug or power management;
- codec connection ranges, selectable multi-input pins, unsolicited responses,
  and physical-codec compatibility claims;
- interrupt-driven persistent playback, host WAV/acoustic validation, and
  production daily-driver audio.

Those require a persistent controller owner, native readiness/cancellation,
bounded per-process buffers, a fixed-point mixer, process-death cleanup, and a
separate QEMU audio artifact. None is implied by this kernel proof.
