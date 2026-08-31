<!-- SPDX-License-Identifier: GPL-3.0-only -->

# ext4 storage boundary

Sapote has one ext4 implementation: the audited, pinned `ext4plus` source under
`vendor/ext4plus`. It is built `no_std`, synchronously, and with default
features disabled. Cargo is locked and forced offline through `.cargo/config.toml`;
the exact registry closure is under `vendor/rust-crates`.

The first integrated boundary is deliberately read-only. C owns a generation-
authenticated NVMe session and services checked, possibly unaligned byte reads.
Rust owns ext4 metadata parsing and returns only a pointer-free 16-byte label and
UUID after the superblock, block-group descriptors, and existing journal have
validated. The session is closed on both acceptance and refusal. This is mount
admission, not a second filesystem stack: VFS remains the owner of mounts,
vnodes, open-file descriptions, and directory iterators.

## Read-write admission gate

Upstream reads an existing JBD2 journal but does not journal new mutations.
Consequently Sapote does not pass a writer into ext4plus and does not advertise
ext4 read-write support yet. Read-write admission requires all of the following
in the same implementation:

1. ordered-data JBD2 descriptor, data, revoke, and checksummed commit records;
2. an NVMe Flush after journal data and before acknowledging the commit;
3. replay with sequence, checksum, and revoke validation;
4. deliberate power cuts at every journal commit point, followed by replay and
   namespace/resource census checks; and
5. refusal tests for unsupported feature combinations and corrupt metadata.

This gate prevents a home-block writer from being mistaken for crash-consistent
ext4. The vendored port record in `vendor/ext4plus/SAPOTE-PORT.md` tracks the
delta from the exact upstream commit.
