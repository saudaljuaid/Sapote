# Sapote ext4plus port boundary

Sapote vendors the `src/` tree from ext4plus commit
`ec7e8443e474376977bb752cde370762226a5a50` as its sole ext4
implementation candidate. Upstream's MIT and Apache-2.0 notices are retained
unchanged.

The source was selected because it is designed for `no_std` + `alloc`, has a
caller-supplied block I/O contract, performs broad checked ext4 metadata
validation, supports extents and 64-bit files, and already implements the
required read/write object operations.

The unmodified commit does **not** provide journaled writes. It reads and
validates an existing JBD2 journal but sends mutations directly to home blocks.
Sapote must not expose the backend read-write or claim crash consistency until
the port adds an ordered, checksummed JBD2 writer over Sapote's explicit NVMe
Flush fence and passes deliberate power-loss tests. Read-only mount admission
may be integrated before that point.

Sapote's local journaling delta is intentionally below that admission line.
`src/journal/transaction.rs` builds one bounded checksum-v3 descriptor,
metadata set, optional 64-bit revoke record, and commit record and returns an
ordered-data operation plan with explicit data, journal-payload, commit, and
checkpoint flush barriers. Its clean-journal ring coordinator bounds and
validates a caller-supplied physical slot map, wraps without collision, refuses
overrun of uncheckpointed records, sequences commit durability, reclaims only
the oldest checkpointed transaction, and rolls back the newest pre-commit
reservation without leaving a sequence gap. Replay and public tests cover
corruption, every power-cut prefix, revocation suppression, wrap, exhaustion,
abort, and out-of-order reclamation.

`JournalSuperblockImage` validates and preserves the exact JBD2 v2 superblock,
admits Sapote's 4 KiB/checksum-v3/64-bit profile, emits checksummed clean/live
sequence-and-start images, and maps a complete clean journal-inode block list
into the ring. The mapping is bounded, distinct, in-range, and includes the
physical superblock in alias checks. Ring preparation also refuses revokes when
the admitted superblock does not advertise block revocations.

`load_journal_inode_map` discovers the internal journal through ext4plus's own
checked inode and extent/block-map iterator, bounds the complete physical map,
reads logical block zero without the replay overlay, and requires its length to
agree with the JBD2 superblock. The public test consumes the real deterministic
e2fsprogs fixture produced immediately before the Cargo suite. Clean-ring
admission additionally requires ext4's incompat-recovery bit to be clear; a
zero JBD2 `s_start` is not treated as authoritative on its own. Superblock
admission reports header, feature, checksum-type, and checksum failures
separately so a real image is never weakened to fit an opaque refusal.

`recover_committed_ring` scans Sapote's bounded single-descriptor transaction
profile from the validated live start across at most one wrap. It validates
every record, discards an uncommitted tail, applies later revocations to older
pending images, and returns a collapsed checkpoint set plus the checksummed
clean superblock state. The ext4 recovery bit must agree with the JBD2 state;
ambiguous or inconsistent combinations are refused.

This planner does not issue home or superblock writes, bind a barrier to
platform I/O,
or redirect ext4 mutation methods away from their upstream home-block writes.
Those gaps keep the Sapote backend read-only.

Sapote also tightens upstream writer admission: an image carrying ext4's
`RO_COMPAT_READONLY` feature now discards the supplied writer just as an image
requiring recovery or carrying an unsupported read-only-compatible feature
does. A focused superblock test pins that refusal. This is a prerequisite for a
future writable profile, not writable-backend admission by itself.

Sapote-specific changes stay in reviewable commits and are summarized here as
they land. The intended port configuration is `--no-default-features
--features sync`; the asynchronous and hosted `std` surfaces are out of scope.
The standalone lockfile is generated from the repository's committed offline
crate mirror and matches the dependency versions in `src/rust/Cargo.lock`.
The vendored manifest replaces workspace-inherited edition/license fields and
removes upstream-only `xtask`/dev dependencies because those sources and test
fixtures are deliberately outside the runtime vendor boundary.

Vendored scope is intentionally limited to `Cargo.toml`, `Cargo.lock`, the two
license files, `README.md`, and `src/`. Upstream test disk images, `xtask`, and
host integration tests are not runtime build inputs and are not vendored.
