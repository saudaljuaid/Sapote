<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Third-party code inventory

Sapote's kernel build is self-contained and makes no registry or network access.
`src/rust/Cargo.lock`, every vendored `.cargo-checksum.json`, and
`.cargo/config.toml` jointly pin and authenticate the complete Rust source
closure. License texts distributed by each package remain in its vendor
directory.

## ext4 implementation

`ext4plus` 0.1.0-rc.2 is pinned to upstream commit
`ec7e8443e474376977bb752cde370762226a5a50` and is available under
MIT OR Apache-2.0. `vendor/ext4plus/UPSTREAM-COMMIT.txt` records the repository,
commit date, Git tree, and license; `vendor/ext4plus/SAPOTE-PORT.md` records the
Sapote delta and the read-write safety gate.

## Locked runtime closure

| Package | Version | License |
| --- | ---: | --- |
| async-lock | 3.4.2 | Apache-2.0 OR MIT |
| async-trait | 0.1.92 | MIT OR Apache-2.0 |
| bitflags | 2.13.1 | MIT OR Apache-2.0 |
| crc | 3.4.0 | MIT OR Apache-2.0 |
| crc-catalog | 2.5.0 | MIT OR Apache-2.0 |
| event-listener | 5.4.2 | Apache-2.0 OR MIT |
| event-listener-strategy | 0.5.4 | Apache-2.0 OR MIT |
| lock_api | 0.4.14 | MIT OR Apache-2.0 |
| maybe-async | 0.2.11 | MIT |
| pin-project-lite | 0.2.17 | Apache-2.0 OR MIT |
| proc-macro2 | 1.0.107 | MIT OR Apache-2.0 |
| quote | 1.0.47 | MIT OR Apache-2.0 |
| scopeguard | 1.2.0 | MIT OR Apache-2.0 |
| spin | 0.10.1 | MIT |
| syn | 2.0.119 and 3.0.4 | MIT OR Apache-2.0 |
| unicode-ident | 1.0.24 | (MIT OR Apache-2.0) AND Unicode-3.0 |

The async crates appear because they are manifest-level dependencies of the
single upstream implementation. Sapote disables ext4plus's async and
multi-threaded paths; Cargo still resolves the published dependency closure.
No upstream test images, hosted adapters, or task runner are included.

## Host signature implementation

Format-v3 package construction and inspection use the distribution-provided
Python `cryptography` Ed25519 implementation. It is a host build dependency,
not code linked into or distributed with Sapote. The Linux verification image
installs `python3-cryptography`, requires a real sign/verify round trip, and
also tests fail-closed behavior with Python site packages disabled.
