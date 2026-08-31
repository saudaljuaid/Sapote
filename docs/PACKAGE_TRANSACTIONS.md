<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Package transaction state and recovery foundation

`tools/sapote-transaction.py` defines a bounded installed-database format, an
atomic-generation authority record, an immutable transaction journal, and a
deterministic in-memory reference model for package state transitions. The model
exercises staging, commit recovery, rollback, removal, dependency retention,
integrity verification, and repair.

This is format and reference groundwork. It does not write a host System image,
install a package into Sapote, or provide a guest package service. No guest code
currently reads these bytes. A future service must connect this contract to
authenticated package-v3 bytes, the signed repository lock, bounded filesystem
operations, durability barriers, and the guest CLI.

## Trust and namespace boundary

The reference API starts after repository and package admission. Candidate
metadata uses package-v3 identity, SemVer, publisher key ID, immutable file path,
kind, mode, length, and SHA-256 semantics. Dependency records retain the exact
identifier/constraint/provider binding chosen by the signed repository resolver.
The transaction layer does not resolve versions again, accept embedded keys, or
turn a SHA-256 into a signature. A future guest caller must authenticate the
repository index, download digest, publisher key, package-v3 signature, and file
payload before calling the equivalent staging operation.

The installed database and journal use SHA-256 for corruption detection and
cross-record binding, not publisher or repository authenticity. Repair payloads
in the host model are described as authenticated because the API verifies them
against the already authenticated installed file length and digest; a guest
service must additionally obtain those bytes from a trusted cache or repeat the
repository/package verification chain.

Immutable package files and mutable user data are distinct namespaces. Package
transactions own only the paths listed in the installed database. The model's
`user_data` mapping is never copied into a package generation, verified against a
package digest, removed, rolled back, or repaired. A future guest implementation
must preserve that separation in its filesystem layout and authorization checks.

## Complete immutable generations

Each committed generation consists of:

1. one canonical installed database;
2. exactly the immutable owned files named by that database; and
3. no unowned immutable files.

Installation, update, removal, and repair build a complete generation at
`current + 1` without changing the selected generation. Every staged byte and
digest is validated before the commit point. A 128-byte authority record is then
atomically replaced to select the complete new generation. Old immutable state
is retained until authority replacement and cleanup are durable.

The host model deliberately charges free space for the complete old generation,
complete staged generation, and journal at once. This is conservative. A future
filesystem may share immutable extents or content-addressed objects, but it must
prove equivalent reference accounting and must never reclaim the old generation
before the new one is complete and authoritative.

## Installed database version 1

The database is at most 32 MiB and starts with `SAPIDB01`. Integers are
little-endian. Fixed-width text is ASCII, NUL-terminated, and zero-tailed. Tables
are contiguous in package, dependency-edge, and owned-file order. SHA-256 covers
every byte following the 512-byte header.

| Header offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | `SAPIDB01` magic |
| 8 | 2 | format version (`1`) |
| 10 | 2 | header bytes (`512`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | exact total bytes |
| 24 | 8 | positive authoritative generation |
| 32 | 4 | positive native ABI |
| 36 | 4 | reserved zero |
| 40 | 16 | architecture (`x86_64`) |
| 56 | 8 | package-table offset (`512`) |
| 64 | 4 | package count, at most 256 |
| 68 | 4 | package-record bytes (`256`) |
| 72 | 8 | dependency-edge-table offset |
| 80 | 4 | edge count, at most 4,096 |
| 84 | 4 | edge-record bytes (`192`) |
| 88 | 8 | owned-file-table offset |
| 96 | 4 | file count, at most 4,096 |
| 100 | 4 | file-record bytes (`256`) |
| 104 | 32 | SHA-256 of all table bytes |
| 136 | 376 | reserved zero bytes |

Package records are sorted by identifier. The same package identifier cannot
appear twice; an update replaces that identity in the next generation.

| Package offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 64 | canonical lowercase dotted identifier |
| 64 | 64 | canonical SemVer 2.0.0 version |
| 128 | 32 | authenticated package-v3 SHA-256 |
| 160 | 32 | authenticated publisher Ed25519 key ID |
| 192 | 4 | flags (`1` means explicitly installed root) |
| 196 | 4 | first owned dependency-edge index |
| 200 | 4 | dependency-edge count |
| 204 | 4 | owned-file count |
| 208 | 48 | reserved zero bytes |

Dependency edges are contiguous for each package and sorted by requested
identifier, constraint, and selected provider. The 192-byte record stores the
requested identifier at 0–63, its package-v3 canonical constraint at 64–119,
the exact provider package identifier at 120–183, and eight reserved zero bytes.
Every provider must exist in the same database. Duplicate requested identifiers
and dependency cycles are refused. Every non-explicit package must be reachable
from an explicit root, so the database cannot canonically retain an orphaned
dependency. The record intentionally preserves virtual provider binding rather
than guessing it during recovery.

Owned-file records are globally sorted by path, so ownership collisions have one
unambiguous refusal before any staging mutation.

| File offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 128 | canonical package-root-relative path |
| 128 | 4 | owner package-table index |
| 132 | 2 | package-v3 file kind |
| 134 | 2 | flags (`0`) |
| 136 | 4 | canonical mode (`0444` or `0555`) |
| 140 | 4 | reserved zero |
| 144 | 8 | exact nonzero length, at most 64 MiB |
| 152 | 32 | file SHA-256 |
| 184 | 64 | library SONAME, empty for other kinds |
| 248 | 8 | reserved zero bytes |

An executable must be `0555`; other current package-v3 kinds may be `0444` or
`0555`. Paths, kinds, modes, and bounds intentionally match package v3.

## Authoritative-generation record

The authority record is exactly 128 bytes and starts with `SAPGEN01`.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | `SAPGEN01` magic |
| 8 | 2 | version (`1`) |
| 10 | 2 | record bytes (`128`) |
| 12 | 4 | flags (`0`) |
| 16 | 8 | selected generation |
| 24 | 8 | exact database bytes |
| 32 | 32 | selected database SHA-256 |
| 64 | 32 | SHA-256 of bytes 0–63 |
| 96 | 32 | reserved zero bytes |

The guest durability contract is stronger than encoding these bytes: it must
flush the staged generation and journal before atomically replacing authority,
then flush authority before reclaiming old state. An implementation may use an
atomic rename, two fixed slots plus a monotonically selected sector, or another
filesystem-specific primitive only if recovery can observe the complete old
record or complete new record, never a synthesized mixture.

## Prepared journal version 1

The journal is exactly 512 bytes and immutable after creation. Authority, not a
mutable journal phase, identifies the commit side. This avoids requiring several
independently durable phase writes.

| Offset | Bytes | Field |
| ---: | ---: | --- |
| 0 | 8 | `SAPTXN01` magic |
| 8 | 2 | version (`1`) |
| 10 | 2 | journal bytes (`512`) |
| 12 | 4 | flags (`0`) |
| 16 | 2 | operation: install `1`, update `2`, remove `3`, repair `4` |
| 18 | 2 | phase (`1`, prepared) |
| 20 | 4 | reserved zero |
| 24 | 8 | base generation |
| 32 | 8 | target generation (`base + 1`) |
| 40 | 8 | conservative required staging space |
| 48 | 8 | base database bytes |
| 56 | 8 | target database bytes |
| 64 | 32 | base database SHA-256 |
| 96 | 32 | target database SHA-256 |
| 128 | 32 | transaction ID: SHA-256 of the record with this field zero |
| 160 | 64 | optional single target package identifier |
| 224 | 288 | reserved zero bytes |

The target field is empty for repair and multi-package plans. It is diagnostic;
the complete target database is authoritative for state.

## Recovery decision

Recovery validates authority, journal, both database digests, database
generation numbers, exact owned-file sets, file lengths, and file digests.

| Observed authority | Complete states | Result |
| --- | --- | --- |
| base | complete base, any target | select base; discard target and journal |
| target | complete target | select target; discard base and journal |
| target or invalid | incomplete target, complete base | repair authority to base |
| any | no complete base and no authoritative complete target | refuse recovery |

Thus an interruption before authority replacement rolls back an update or
remove. An interruption after replacement completes it. Recovery never merges
some old files with some new metadata. Cancellation is permitted only while the
base authority is still selected; afterward normal recovery is required.

`include/sapote/package_state.h` and `src/kernel/package_state.c` provide the
first guest-consumable part of this contract. They are freestanding and
allocation-free: all untrusted integers are decoded from checked little-endian
byte fields, SHA-256 is computed incrementally in fixed storage, dependency
cycle/reachability state is bounded by the 256-package maximum, and no on-disk
structure is overlaid with a C struct. The parser enforces the exact v1 table
locations, counts, record sizes, canonical text and SemVer grammars, package-v3
per-package limits, unique sorting, provider existence, file ownership, modes,
SONAMEs, zero reserved bytes, and both content and envelope digests.

The C recovery core accepts two database candidates and one explicit
`owned_files_complete` proof per candidate. That proof is deliberately not
inferred from a database checksum: a future privileged service must iterate the
owned-file records, open the immutable generation without following unsafe
links, bound every read, and verify exact length and SHA-256 before setting it.
The core then re-parses and re-hashes both databases itself, binds them to the
journal, and selects only the complete base or authoritative complete target.
An authority generation, database length, or database digest unrelated to the
prepared journal is refused as a state mismatch. With no journal, exactly one
complete candidate must match authority.

This remains a decision core, not filesystem recovery. It returns which complete
generation is safe; it does not rename authority, delete staging, install files,
or mutate user data.

## Removal, references, and repair

The explicit bit distinguishes requested roots from automatically selected
dependencies. Transactional removal clears a requested root by constructing a
new generation from every other explicit root and its exact provider edges.
Dependencies reachable from another root remain installed. Dependencies that
become unreachable are removed in the same generation. A direct attempt to
remove a package still required by another root is refused rather than silently
breaking the graph.

Verification reports missing, size-mismatched, digest-mismatched, and unowned
immutable paths deterministically. Repair builds another complete generation;
it accepts a replacement only when its length and SHA-256 match installed
authenticated metadata. Extra replacement paths are refused. Mutable user data
is outside both operations.

## Host reference commands and tests

The CLI exposes byte-format construction and inspection only:

```sh
python3 tools/sapote-transaction.py build-database \
    --spec build/installed-state.json --output build/installed.db
python3 tools/sapote-transaction.py inspect-database build/installed.db
python3 tools/sapote-transaction.py inspect-authority build/installed.authority
python3 tools/sapote-transaction.py inspect-journal build/installed.journal

python3 tools/sapote_transaction_host_test.py

gcc -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iinclude \
    tools/package-state-host-test.c src/kernel/package_state.c \
    -o build/package-state-host-test
build/package-state-host-test
```

The test creates only in-memory reference stores. It covers deterministic
encoding, zero reserved fields, interrupted commit on both sides of authority
replacement, incomplete-new fallback, update rollback, transactional removal,
shared dependency retention, ownership collision, cancellation cleanup,
low-space refusal, verify/repair, and preserved user data.

The C host test independently constructs canonical empty-base and two-package
target byte fixtures, then mutates reserved fields, table bounds, canonical
package text, provider bindings, ownership paths, content digests, authority,
and journal records. It exercises recovery before and after the authority switch,
incomplete-new fallback, absent-journal selection, corrupt-authority fallback,
and unrelated-authority refusal under `-Werror`.

Guest integration still requires authenticated download/cache plumbing,
filesystem generation directories or equivalent immutable storage, bounded I/O
and cancellation points, fsync/flush and atomic-replace guarantees, boot-time
recovery invocation, service authorization, CLI presentation, and end-to-end
power-failure tests. Until those exist, this tool must not be described as a
host installer or Sapote's guest package manager.
