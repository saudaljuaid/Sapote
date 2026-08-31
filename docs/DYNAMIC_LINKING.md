<!-- SPDX-License-Identifier: GPL-3.0-only -->

# x86-64 dynamic-linking foundation

`src/rust/elf64_dynamic.rs` is an allocation-free admission, symbol lookup,
dependency ordering, image preparation, and RELA application foundation for
bounded x86-64 `ET_DYN` objects. `tools/elf64-dynamic-host-test.rs` exercises it
with genuine multi-object ELF64 byte fixtures using both SysV and GNU hashes.

This is deliberately not described as Sapote's guest dynamic loader. The
current guest path in `native_image.rs` admits one static `ET_EXEC` and explicitly
rejects `PT_DYNAMIC`, relocation sections, and dynamic symbols. The C native
process mapper reads that one manifest-named file, copies its already admitted
segments, and installs their final permissions. It has no authenticated
dependency-file reader, multi-object load-bias policy, or constructor lifecycle.
Adding those pieces safely requires later package-service and process-loader
integration; this slice does not touch VFS, ext4, paging, or the existing mapper.

## Admission policy

The parser accepts little-endian System V x86-64 `ET_DYN` only. Every integer is
decoded from a checked byte slice; it never overlays untrusted bytes with a Rust
or C structure. Arithmetic for file ranges, virtual ranges, table extents,
runtime addresses, relocation values, and signed PC-relative results is checked.

| Bound | Limit |
| --- | ---: |
| ELF file | 64 MiB |
| Program headers | 32 |
| `PT_LOAD` segments | 16 |
| Mapped span per object | 256 MiB |
| Dynamic entries | 256 |
| `DT_NEEDED` entries | 16 |
| Dynamic strings | 1 MiB table; 63 bytes per consumed name |
| Dynamic symbols | 4,096 |
| Main plus PLT RELA records | 256 |
| Dependency objects | 32 |
| Init/fini array entries | 256 each |

`PT_LOAD` records must be ordered, page-disjoint, congruent, and use only R,
R-X, or RW permissions. A W+X segment, page sharing between differently
protected loads, wrapped address, invalid file extent, span above the bound, or
entry outside executable content is refused. Link-time addresses are below 4
GiB; caller-selected page-aligned runtime bias plus every mapped address must
remain in lower canonical userspace.

Exactly one RW `PT_DYNAMIC` and one non-executable RW `PT_GNU_STACK` are
required. Optional `PT_GNU_RELRO` is recorded as a page-rounded final read-only
intent. Bounded read-only `PT_NOTE`, `PT_PHDR`, and `PT_GNU_EH_FRAME` records are
accepted. `PT_INTERP`, `PT_TLS`, and unknown program types are explicitly
refused. TLS is not silently approximated.

Section headers are not used for linking, but an included table and every
non-`SHT_NOBITS` file extent must still be bounded. Extended section numbering
is outside this version.

## Dynamic metadata and hashes

The RW dynamic table must be file-backed, have 16-byte entries, terminate with
`DT_NULL`, and contain only zero entries afterward. Singleton tags cannot be
repeated. This foundation understands:

- `DT_NEEDED` and optional `DT_SONAME`;
- `DT_STRTAB`, `DT_STRSZ`, `DT_SYMTAB`, and 24-byte `DT_SYMENT`;
- `DT_HASH` and/or `DT_GNU_HASH`;
- `DT_RELA`, `DT_RELASZ`, 24-byte `DT_RELAENT`, and `DT_RELACOUNT`;
- eager `DT_JMPREL`, `DT_PLTRELSZ`, `DT_PLTREL=DT_RELA`, and `DT_PLTGOT`;
- `DT_BIND_NOW`, supported NOW flag bits, `DT_INIT`, `DT_FINI`, and bounded
  init/fini arrays;
- a zero initial `DT_DEBUG` slot.

Consumed SONAME and dependency names are canonical ASCII library basenames.
Duplicate direct dependencies are refused. Symbol zero must be all-zero. Symbol
records consumed by lookup or relocation are bounded and restricted to local,
global, or weak bindings and NOTYPE, OBJECT, or FUNC types. Version tables,
symbol-version selection, GNU unique binding, IFUNC, audit/filter records,
RPATH/RUNPATH, `DT_SYMBOLIC`, text relocations, lazy PLT binding, `DT_REL`, and
`DT_RELR` are unsupported and refused rather than ignored.

For SysV hashes the complete bucket and chain arrays are bounded and every
index is checked. GNU bloom, bucket, symbol-offset, and terminal-chain walks are
bounded by the symbol limit. When both hash families are present they must
derive the same symbol count. Every defined exported name must be reachable
through the selected hash family; duplicate unversioned exports are refused.

## Relocation and W^X contract

The admitted x86-64 RELA subset is:

| Type | Formula |
| --- | --- |
| `R_X86_64_RELATIVE` | `B + A` |
| `R_X86_64_64` | `S + A` |
| `R_X86_64_GLOB_DAT` | `S + A` |
| `R_X86_64_JUMP_SLOT` | `S + A` |
| `R_X86_64_PC32` | checked signed 32-bit `S + A - P` |
| `R_X86_64_NONE` | only the all-zero record |

PLT RELA records must be `JUMP_SLOT`. Relative records require symbol zero;
symbol relocations require a nonzero in-range symbol. Every relocation target
must be wholly inside an RW load, and relocation target byte ranges must be
unique and non-overlapping. COPY, IFUNC/IRELATIVE, TLS, size, and unknown
relocation types are refused. Undefined strong symbols fail; unresolved weak
symbols resolve to zero.

`load_image()` copies admitted load bytes into an exactly sized, zero-filled
private preparation buffer. `apply_relocations()` revalidates the relocation
tables and writes only that buffer.
After relocation, a future guest loader must install the recorded R, RX, RW,
and RELRO intents and discard all writable aliases to executable frames. These
functions do not change page tables, so the module expresses and validates W^X
intent without falsely claiming enforcement in the host test.

Global symbol preemption uses the exact caller-provided object scope. The
intended scope is root first followed by dependency order. Local symbols remain
object-relative, SHN_ABS remains absolute, and all runtime-address additions are
checked.

## Deterministic dependency order

`dependency_order()` resolves exact `DT_NEEDED` names against unique SONAMEs.
It walks each object's needed records in their file order, emits dependencies
before consumers, de-duplicates already visited objects, and refuses missing or
ambiguous SONAMEs, cycles, and graphs above 32 objects. It does not search host
directories or guess aliases.

The package repository resolver remains responsible for selecting exact package
versions and files. This ELF layer starts only after those package bytes and
digests have been authenticated. It does not weaken or duplicate package trust.

## Host verification

The standalone test is wired into `make verify` through
`make dynamic-elf-tests`:

```sh
rustc --edition 2024 --test -D warnings \
    tools/elf64-dynamic-host-test.rs -o build/elf64-dynamic-host-test
build/elf64-dynamic-host-test
```

The tests construct bounded ET_DYN objects, parse both hash families, resolve a
dependency symbol, apply relative, absolute, GOT, PLT, and PC32 relocations, and
check the resulting bytes. Mutation cases cover W+X, unsupported dynamic tags,
duplicate singleton tags, malformed hashes, unsupported relocation types,
non-writable targets, arithmetic overflow, missing/ambiguous dependencies,
cycles, and dependency graph bounds.

Guest execution still requires authenticated library reads, non-overlapping
multi-object load-bias selection, process-page accounting, final map/RELRO
installation, constructor/destructor sequencing, TLS policy, failure unwind,
and end-to-end package tests. Until those exist, Sapote continues to execute
only its existing admitted static native applications.
