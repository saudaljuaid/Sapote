<p align="center">
  <img src="assets/sapote-logo.png" alt="Sapote red S mark" width="170">
</p>

<h1 align="center">Sapote Redwood</h1>

<p align="center"><strong>An x86_64 operating system built from first principles.</strong></p>

<p align="center">
  <a href="https://github.com/saudaljuaid/Sapote/actions/workflows/verify.yml"><img src="https://github.com/saudaljuaid/Sapote/actions/workflows/verify.yml/badge.svg" alt="verification status"></a>
  <img src="https://img.shields.io/badge/release-Redwood-E31920" alt="Sapote Redwood">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--only-595976" alt="GPL-3.0-only"></a>
</p>

<p align="center">
  <img src="assets/sapote-redwood.png" alt="Sapote Redwood desktop" width="820">
</p>

<p align="center"><a href="assets/sapote-ui-redesign-25s.mp4"><strong>Watch the 25-second QEMU demo</strong></a></p>

## Main mission

Our mission is to provide a stable and truthful operating system to the modern world!

## About

Sapote is an operating system from scratch, its main release is currently
Redwood.

## Highlights

- A 64-bit kernel built from scratch — no Linux inside.
- Drivers for real hardware: NVMe, USB, audio, networking.
- Runs real outside software — Lua and SQLite, natively.
- 113 automated boots in QEMU, on every single change.

## Build and boot

Ubuntu 24.04 (or similar), with a few standard tools.

```sh
sudo apt-get install binutils gcc grub-common grub-pc-bin make mtools \
    qemu-system-x86 xorriso
rustup target add x86_64-unknown-none

make run
```

That's it — it boots straight into QEMU. Run `make verify` first if you
want the full test suite to pass before you trust it!

## Design

C and assembly handle anything that touches real hardware. Rust's job is
narrower: check any bytes the kernel didn't create itself — a file, a
network packet — before C ever touches them. That same Rust also runs
native applications.

There's a Boot Ledger too — it just keeps track of what's started and in
what order, so nothing boots blind.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Sapote Redwood](docs/REDWOOD.md)
- [Persistent FAT32](docs/FAT32.md)
- [Networking](docs/NETWORKING.md)
- [Processes](docs/MULTIPROCESS.md)
- [Drivers](docs/DRIVERS.md)
- [HD Audio](docs/AUDIO.md)
- [NVIDIA](docs/NVIDIA.md)
- [Linux syscall boundary](docs/LINUX_SYSCALL_ABI.md)
- [Rust boundary](docs/RUST.md)
- [Verification](docs/VERIFICATION.md)
- [Third-party assets](docs/THIRD_PARTY_ASSETS.md)

See [CONTRIBUTING.md](CONTRIBUTING.md) before sending changes. Sapote is
licensed under [GPL-3.0-only](LICENSE).
