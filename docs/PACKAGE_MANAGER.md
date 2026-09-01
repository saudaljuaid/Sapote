<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Package admission and planning

`src/kernel/package_manager.c` is the bounded guest-side parser and planner for
signed repository-index v1, signed package v3, and installed-database v1 bytes.
It is the parser and resolver used by privileged package services.

## Trust boundary

The core never treats a digest as a signature and never accepts a key embedded
by the object being verified. Its caller supplies an immutable key lookup and a
real Ed25519 verifier. Repository and package admission fail closed when either
callback is absent, the SHA-256 ID of the returned raw public key differs from
the signed key ID, the key is unknown or revoked, or signature verification
fails. The verifier receives the complete object plus the one 64-byte signature
range that must be treated as zero.

Repository admission additionally enforces the caller's wall-clock freshness,
minimum repository version, native ABI, and `x86_64` policy. Package admission
binds exact downloaded bytes to the signed repository size and SHA-256 before
checking the package's publisher signature, identity, version, ABI, dependency
and conflict records, file layout, per-file digest, mode, kind, and SONAME.

`package_trust.c` supplies the guest Ed25519 verifier and immutable key-table
lookup. Provisioning still belongs to the privileged caller: the table admits
only sorted unique keys whose SHA-256 IDs match, rejects non-canonical or pure
low-order public keys, and preserves explicit trusted/revoked state. Provisioned
keys must be generated Ed25519 keys. Verification incrementally substitutes the
64-byte zero range and narrows pinned Monocypher 4.0.3's cofactored profile with
canonical encoding, negative-zero, pure-low-order, and `S < L` requirements.
Signature bytes are not a uniqueness identifier. No signing or private key
enters the guest.

## Bounds and resolver behavior

The binary bounds match the canonical host formats:

- repository: 32 MiB, 1,024 packages, and 64 relations of each kind per
  package;
- package: 256 MiB, 256 files, 64 dependencies, and 64 conflicts;
- install plan: 128 packages, 256 provider bindings, and dependency depth 16;
- search result: 64 entries;
- installed state: the fixed limits in `package_state.h`.

Resolution selects one deterministic provider identity, tries that provider's
versions from highest SemVer precedence to lowest, backtracks within the fixed
bounds, applies conflicts, and iteratively emits dependencies before consumers.
Distinct packages providing the same requested identity are refused as
ambiguous. Missing constraints, cycles, and graphs beyond the plan/depth bounds
have separate statuses.

The planner checks publisher keys for every selected package, prevents implicit
downgrades unless policy explicitly permits them, and refuses publisher-key
rotation rather than silently changing trust. Removal starts from an explicit
installed root and removes only dependencies no longer reachable from another
explicit root; shared dependencies stay installed. Removal plans do not delete
mutable application data.

Planning is serialized. Its roughly 9 KiB graph workspace is static rather than
placed on Sapote's 16 KiB syscall stack, reentrant entry is refused, candidate
selection uses bounded catalog scans instead of a per-recursion candidate
array, and topological ordering is iterative. The input byte buffers must remain
immutable and live while returned views or plans are used because text and
digest fields are slices into those authenticated buffers.

## Authenticated staging views

After `package_manager_package_open()` admits exact repository-bound and
publisher-signed bytes, `package_manager_package_file()` exposes one immutable
path, kind, mode, SONAME, SHA-256, and payload slice at a time. The accessor
rechecks record bounds, path and kind invariants, and the payload digest before
returning it. Dependency and conflict accessors expose the exact canonical
relations from those same admitted bytes. Out-of-range and structurally forged
views fail closed; no accessor writes a filesystem or reports installation.

These slices are the extraction boundary for a privileged generation builder.
They remain valid only while the admitted package buffer is live and unchanged.
`package_manager_plan_dependency_binding()` also exposes every planned
consumer's exact dependency identifier, constraint, and unique provider
identity. It revalidates the plan item against the authenticated repository and
uses the resolver's bounded ambiguity rule without adding another large binding
array to the already stack-sensitive plan. The provider may be an unchanged
installed package and therefore need no plan item of its own. The future builder
must still prevent path traversal and ownership collisions, write a complete
next generation, verify it, and commit it through the package transaction
protocol.

## Verification and integration

`make package-manager-tests` builds real deterministic Ed25519 packages and
indexes with the canonical host tools, verifies them with the independent host
crypto backend, and passes those exact bytes through the production
freestanding C trust provider and parser. `make package-trust-tests` adds a
fixed cross-provider vector and strict encoding, revocation, and table tests. It
covers package/repository admission, search, dependency-first install planning,
installed-state no-op and removal, unknown/revoked keys, unavailable crypto,
signature rejection, digest changes, freshness, rollback, ABI mismatch,
unsatisfied dependencies, conflicts, ambiguous providers, cycles, backtracking,
and the dependency-depth bound.

`package_service.c` recovers already-staged FAT32 generations. Fetching,
signature-provider setup, generation building and commit, client commands, and
Store presentation remain outside this parser and planner.
