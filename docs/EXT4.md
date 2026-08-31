<!-- SPDX-License-Identifier: GPL-3.0-only -->

# ext4 storage boundary

Sapote has one ext4 implementation: the audited, pinned `ext4plus` source under
`vendor/ext4plus`. It is built `no_std`, synchronously, and with default
features disabled. Cargo is locked and forced offline through `.cargo/config.toml`;
the exact registry closure is under `vendor/rust-crates`.

The integrated backend is deliberately read-only. VFS remains the sole owner of
mounts, vnodes, open-file descriptions, generations, and directory iterators.
The ext4 backend owns one opaque Rust `Ext4` object per admitted volume and
generation-authenticated C file/directory cookies. It implements root and nested
lookup, open, read, offset-preserving pread, 64-bit seek/stat, and directory
enumeration. Hard-linked paths report the same inode identity to the vnode
table. Symlinks are resolved by ext4plus for lookup and open.

C never leaves an NVMe filesystem session open. It acquires a read-only session
around one synchronous Rust operation, installs it in stable per-mount state,
and closes it on every success or failure exit. The Rust reader points to that
mount state and C refuses callbacks without an active lease. Every byte request
checks the namespace capacity before calculating or issuing an LBA read. A
failed mount drops any allocated Rust object and closes the session before the
mount becomes visible.

Admission accepts exactly the deterministic profile produced by
`tools/ext4_image.py`: 4096-byte blocks, 256-byte inodes, 64-byte group
descriptors, a zero first-data-block field, `has_journal`,
`ext_attr`, `dir_index`, `filetype`, extents, 64-bit block counts,
`metadata_csum` plus its seed, sparse/large/huge files, `dir_nlink`, and
`extra_isize`, with no additional feature bits. The declared block count must
fit the NVMe namespace and all free/total geometry is checked. Ext4plus then
validates the superblock, group descriptors, and existing journal. Sapote walks
the reachable namespace (at most 8,192 entries and 512 queued directories),
validating directory blocks,
inode metadata and timestamps, extended-attribute names and values, symlink
targets, and the first and last mapped byte of non-empty files. Remaining file
extent/data checks are lazy and occur on pread.

Sapote's VFS currently admits ASCII mount-relative paths shorter than 128 bytes
and at most 16 components. Directory names may be 255 bytes on disk; entries
that cannot fit the current VFS path contract can be enumerated but cannot be
opened through ABI v1. Indexed directory reads are intentionally bounded and
stateless at the Rust boundary, so advancing an iterator rescans to its checked
index; this is correct but quadratic for large directories.

## Ownership and lock order

The current kernel executes this path synchronously on one core. The required
order is VFS object/generation state, ext4 backend handle state, ext4plus's
internal synchronous read lock, native byte callback, active NVMe volume
session. Code must not call back into VFS while it owns an ext4plus object or an
NVMe session. Heap allocation may occur inside ext4plus, but no heap allocation
survives refused admission and no NVMe lease survives any operation. SMP work
must add an explicit VFS/backend lock before relying on this order; the present
single-core serialization is not an SMP locking claim.

## Read-write admission gate

Upstream reads an existing JBD2 journal but does not journal new mutations.
Consequently Sapote does not pass a writer into ext4plus. `sync`, create,
write, truncate, mkdir, rename, unlink, rmdir, and link all return `EROFS`.
Sapote does not advertise ext4 write or crash-recovery support. Read-write
admission requires all of the following
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

## Ordered transaction foundation

The vendored crate now contains a lower-level, no-std transaction primitive;
this does **not** open the read-write admission gate. `JournalTransaction`
accepts only complete 4 KiB block images, rejects duplicate or out-of-range
home blocks, bounds ordered-data and metadata sets to 64 blocks each, and
refuses metadata that would require the unsupported JBD2 magic-escape rule. A
transaction serializes one checksum-v3/64-bit descriptor, its checksummed
metadata images, and one checksummed commit block. The serializer feeds the
same descriptor-tag and commit validators used by the existing replay reader.

The caller must supply a distinct physical journal block for the descriptor,
each metadata image, and the commit. The resulting operation list has one legal
order:

```text
ordered file-data home writes
Flush(OrderedData)
descriptor and metadata journal writes
Flush(JournalPayload)
commit journal write
Flush(Commit)
metadata home-block checkpoint writes
Flush(Checkpoint)
```

There is no metadata home-block operation before `Flush(Commit)`. A complete,
checksummed commit is required before `replay_committed_transaction()` returns
any home image. Pure tests cut the operation list at every boundary and verify
that every pre-commit prefix is non-replayable and has no durable home metadata;
they also corrupt descriptor, data, and commit bytes independently.
`make ext4-tests` runs those vendored-crate controls in the admitted
`--no-default-features --features sync` profile as well as the existing hostile
image suite, using only the committed Cargo source mirror.

Sapote's NVMe layer already exposes the required `nvme_volume_flush()` fence,
but the ext4 backend deliberately does not bind the plan to it yet. The missing
work is not a small wrapper: it needs journal-inode ring allocation and wrap,
head/tail and sequence updates, revoke records for block reuse, checkpoint/log
reclamation, allocation rollback, a writable Rust/C volume lease, and VFS-level
mutation/handle coherency. Until all of those are integrated and power-cut in
QEMU, `ext4_backend_drive().read_only` remains true and create, write,
truncate, rename, unlink, and sync remain `EROFS`.
