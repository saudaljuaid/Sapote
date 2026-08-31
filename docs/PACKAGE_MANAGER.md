<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Guest package-manager admission and planning core

`src/kernel/package_manager.c` is the bounded guest-side parser and planner for
signed repository-index v1, signed package v3, and installed-database v1 bytes.
It is a privileged service building block, not yet the `sap` command or the
Redwood Store backend.

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

No in-kernel Ed25519 provider or immutable guest key store is connected yet.
Consequently no guest path currently returns an admitted repository or package.
The host verification first authenticates test fixtures with Python
`cryptography`; the C harness then tests the callback selection and zero-range
contract. That is parser/planner evidence, not an in-guest signature claim.

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

## Verification and current integration limit

`make package-manager-tests` builds real deterministic Ed25519 packages and
indexes with the canonical host tools, verifies them with the real host crypto
backend, and passes those exact bytes to the strict freestanding C core. It
covers package/repository admission, search, dependency-first install planning,
installed-state no-op and removal, unknown/revoked keys, unavailable crypto,
signature rejection, digest changes, freshness, rollback, ABI mismatch,
unsatisfied dependencies, conflicts, ambiguous providers, cycles, backtracking,
and the dependency-depth bound.

Still missing are the privileged fetch/staging service, guest Ed25519 and trust
store, package extraction and owned-file generation builder, public client ABI,
`sap` commands, Store catalog wiring, ext4 writable commit path, cancellation,
and QEMU install/update/recovery evidence. `package_service.c` currently recovers
already-staged FAT32 generations; it does not perform these missing operations.
