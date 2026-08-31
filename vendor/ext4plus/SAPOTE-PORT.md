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

Sapote's first local journaling delta is intentionally below that admission
line. `src/journal/transaction.rs` builds one bounded checksum-v3 descriptor,
metadata set, and commit record and returns an ordered-data operation plan with
explicit data, journal-payload, commit, and checkpoint flush barriers. It also
validates a complete transaction for replay and has corruption and every-cut
unit controls. It does not allocate the journal ring, emit revokes, update the
journal superblock, bind a barrier to platform I/O, or redirect ext4 mutation
methods away from their upstream home-block writes. Those gaps keep the Sapote
backend read-only.

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
