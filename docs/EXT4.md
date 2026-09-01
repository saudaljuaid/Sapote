<!-- SPDX-License-Identifier: GPL-3.0-only -->

# ext4 storage boundary

Sapote has one ext4 implementation: the audited, pinned `ext4plus` source under
`vendor/ext4plus`. It is built `no_std`, synchronously, and with default
features disabled. Cargo is locked and forced offline through `.cargo/config.toml`;
the exact registry closure is under `vendor/rust-crates`.

The integrated backend remains read-only to VFS callers. VFS remains the sole owner of
mounts, vnodes, open-file descriptions, generations, and directory iterators.
The ext4 backend owns one opaque Rust `Ext4` object per admitted volume and
generation-authenticated C file/directory cookies. It implements root and nested
lookup, open, read, offset-preserving pread, 64-bit seek/stat, and directory
enumeration. Hard-linked paths report the same inode identity to the vnode
table. Symlinks are resolved by ext4plus for lookup and open.

C never leaves an NVMe filesystem session open. Ordinary VFS calls acquire a
read-only session around one synchronous Rust operation. Mount acquires a
writable session solely so validated JBD2 recovery can checkpoint before the
volume becomes visible; no VFS mutation reaches that lease. Rust points to the
stable per-mount state and C refuses read, write, or flush callbacks without an
active lease, and refuses writes and flushes without the writable mount lease.
Every byte request checks the namespace capacity before calculating or issuing
an LBA operation. A failed mount drops any allocated Rust object and closes the
session before the mount becomes visible.

Admission accepts exactly the deterministic profile produced by
`tools/ext4_image.py`: 4096-byte blocks, 256-byte inodes, 64-byte group
descriptors, a zero first-data-block field, `has_journal`,
`ext_attr`, `dir_index`, `filetype`, extents, 64-bit block counts,
`metadata_csum` plus its seed, sparse/large/huge files, `dir_nlink`, and
`extra_isize`, with no additional feature bits other than the transient ext4
incompat-recovery marker. The declared block count must
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

The kernel executes this path synchronously on one core. The required
order is VFS object/generation state, ext4 backend handle state, ext4plus's
internal synchronous read lock, native byte callback, active NVMe volume
session. Code must not call back into VFS while it owns an ext4plus object or an
NVMe session. Heap allocation may occur inside ext4plus, but no allocation
survives a rejected mount and no NVMe lease survives an operation. This lock
order applies to the current single-core execution model.

## Read-write admission gate

Upstream reads an existing JBD2 journal but does not journal new mutations, so
Sapote does not pass a writer into ext4plus. `sync`, create, write, truncate,
mkdir, rename, unlink, rmdir, and link return `EROFS`. Enabling the write path
requires all of the following in one implementation:

1. ordered-data JBD2 descriptor, data, revoke, and checksummed commit records;
2. an NVMe Flush after journal data and before acknowledging the commit;
3. replay with sequence, checksum, and revoke validation;
4. deliberate power cuts at every journal commit point, followed by replay and
   namespace/resource census checks; and
5. refusal tests for unsupported feature combinations and corrupt metadata.

The vendored port record in `vendor/ext4plus/SAPOTE-PORT.md` tracks the delta
from the pinned upstream commit.

## Ordered transaction foundation

The vendored crate contains a lower-level `no_std` transaction primitive.
`JournalTransaction`
accepts only complete 4 KiB block images, rejects duplicate or out-of-range
home blocks, bounds ordered-data, metadata, and revocation sets to 64 blocks
each, and
refuses metadata that would require the unsupported JBD2 magic-escape rule. A
transaction serializes one checksum-v3/64-bit descriptor, its checksummed
metadata images, an optional checksummed 64-bit revoke record, and one
checksummed commit block. The serializer feeds the same descriptor-tag,
revocation, and commit validators used by the existing replay reader.

`JournalRing` admits a distinct, bounded physical data-slot map for a clean
journal and assigns those records without collision across wrap. It refuses
more than 8,192 slots, duplicate/out-of-range slots, stale transaction
sequences, and reservations that would overrun uncheckpointed data. Commit
durability is sequence ordered. A mapped ring starts each durable plan with the
checksummed nonzero live-tail superblock, and only the oldest committed
reservation can build a clean/advanced-tail update after its home checkpoint
flush. Slots are reclaimed only after that final journal-state flush is
acknowledged. The newest reservation can be aborted before its live plan starts
without leaving a sequence gap. Public tests cover wrap, full-ring refusal,
early/out-of-order reclamation refusal, abort, revoke corruption, and replay
suppression.

`JournalSuperblockImage` validates
the exact 1,024-byte JBD2 v2 superblock header, checksum-v3/64-bit feature set,
CRC32C checksum, 4 KiB block size, sequence, clean/live start, and a bounded
logical journal length. Header, feature, checksum-type, and stored/calculated
checksum refusals remain distinct at the public boundary. Deterministic tooling
can build the canonical clean image or derive checksummed clean/live
sequence-and-start images without changing other admitted bytes. Clean
admission requires a complete, distinct, in-range physical map of the journal
inode (including its superblock), and the mapped ring refuses home metadata
that aliases that superblock or revoke records when the corresponding
incompatible-feature bit is absent.

`load_journal_inode_map` follows the admitted ext4 journal inode through the
same bounded extent/block-map iterator used by ext4plus, refuses holes,
duplicates, truncation, excess blocks, and superblock-length disagreement, and
reads logical block zero without consulting the replay overlay. The public host
suite passes the deterministic e2fsprogs image from the Python profile test into
Rust and proves that its real journal inode maps into the clean ring. Libext2fs
creates a feature-zero journal and normally upgrades it on mount. The fixture
generator performs that deterministic upgrade without mounting the image. It
installs the admitted
revoke/64-bit/checksum-v3 bits and CRC32C, then both e2fsck and the independent
Python/Rust parsers revalidate the result. The fixture verifier pins the clean
start, 4 MiB journal length, UUID match, feature masks, checksum type, and
stored checksum.

The Rust mount path performs the same journal-inode discovery and JBD2 admission
before exposing the filesystem to VFS. A clean filesystem must map into a
complete clean ring. For a filesystem carrying ext4's recovery bit, Sapote
reads the bounded physical ring, independently validates and collapses every
committed transaction, checkpoints the returned home images, flushes them,
persists and flushes the returned clean JBD2 superblock, and only then clears
and flushes the checksummed ext4 recovery marker. It reloads and re-admits the
clean filesystem before walking the namespace or exposing the mount.

`recover_committed_ring` implements the bounded live-ring reader for Sapote's
single-descriptor transaction profile. It starts at the admitted JBD2 sequence
and live block, follows consecutive committed records across one wrap, validates
every descriptor, data tag, optional revoke, and commit checksum, discards an
uncommitted tail, and collapses later images and revocations into the final
checkpoint set. A corrupt record that claims the expected transaction is
refused. Recovery also requires the caller to provide ext4's incompat-recovery
feature state. A clear ext4 recovery bit with nonzero JBD2 `s_start` is refused.
A set ext4 recovery bit with zero `s_start` is the valid marker-only crash state
before the first live journal-superblock write; it replays nothing but must pass
the same ordered cleanup before the marker clears. Tests cover wrap, cross-
transaction revocation, an uncommitted tail, a corrupt committed record, and
ext4/JBD2 state disagreement.

The caller must supply a distinct physical journal block for the descriptor,
each metadata image, and the commit. The resulting operation list has one legal
order:

```text
checksummed ext4 primary-superblock write (set incompat-recovery)
Flush(FilesystemState)
live JBD2 superblock write (nonzero s_start)
ordered file-data home writes
Flush(OrderedData)
descriptor and metadata journal writes
Flush(JournalPayload)
commit journal write
Flush(Commit)
metadata home-block checkpoint writes
Flush(Checkpoint)
clean or advanced-tail JBD2 superblock write
Flush(JournalState)
```

Mount recovery has its own terminal ordering:

```text
validated committed metadata home-block checkpoint writes
Flush(Checkpoint)
clean JBD2 superblock write
Flush(JournalState)
checksummed ext4 primary-superblock write (clear incompat-recovery)
Flush(FilesystemState)
```

Metadata home blocks appear only after `Flush(Commit)`, and slots are reused
only after `Flush(JournalState)`. A ring discovered from a real clean ext4
image refuses its first mapped commit plan until the caller acknowledges the
dedicated recovery-marker flush. `FilesystemSuperblockImage` changes only the
little-endian incompatibility word and the primary-superblock CRC32C, whose
all-ones seed is independent of ext4's general metadata checksum seed. A
complete, checksummed commit is required before
`replay_committed_transaction()` returns any home image. Pure tests cut the
mapped operation list at every boundary and prove each durable prefix is either
clean, contains only an uncommitted tail, or replays the complete metadata
image. They also corrupt descriptor, data, and commit bytes independently.
`make ext4-tests` runs the public transaction checks from
`tools/ext4-transaction-tests` with
`--no-default-features --features sync`, followed by the hostile-image suite.

The ring planner validates the journal map and produces recovery-marker, live,
checkpoint, recovery-cleanup, and tail-state operations. A shared executor maps
every operation to checked absolute byte writes and preserves every flush. The
kernel mount adapter binds those writes to `nvme_volume_write()` and each
barrier to `nvme_volume_flush()` for recovery only. Writable namespace methods
are still not redirected into the planner, clean unmount/fsync and allocation
rollback are not complete, deliberate recovery power-cut QEMU evidence is not
yet present, and all VFS mutation operations continue to return `EROFS`.
