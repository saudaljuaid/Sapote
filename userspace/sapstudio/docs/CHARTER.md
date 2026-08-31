<!-- SPDX-License-Identifier: GPL-3.0-only -->

# SapStudio charter

SapStudio is Sapote's native non-linear video editor. It targets deterministic
editing, exact timeline arithmetic, sample-accurate audio, explicit color
handling, and reproducible export.

The repository contains the editor model, media and render pipelines, format
code, application coordination, a freestanding image, verification tools, and
the platform seams used to run on Sapote.

## Native to Sapote, and only Sapote

SapStudio is a Sapote application, not a portable application with a Sapote
backend.

- Shipping code has no portability layer, POSIX dependency, or Linux backend.
  Host builds exercise pure logic and are not release targets.
- Where Sapote lacks a capability, the answer is to specify and measure that
  capability in Sapote, not to emulate it inside SapStudio.
- Where Sapote's contract is narrower than a library expects, the library is
  adapted or refused. The contract is not widened to suit a dependency.

This scope keeps timing, memory, rendering, and failure behavior tied to one
measured platform contract.

## What the application owns

| Concern | Language | Why |
| --- | --- | --- |
| Project model, timeline, media library, undo/redo, pipeline coordination, UI state | Rust | The whole application is untrusted-input-shaped and lifetime-shaped. This is the default and the majority. |
| The C ABI boundary to Sapote and native libraries | C ABI | Sapote's application ABI is C-shaped. |
| Tiny freestanding shims where the boundary is instruction- or register-shaped | C | A shim is small enough to read in one sitting and holds no state of its own. |
| Sealed inner loops that a measurement proves need it | C++ | Only after a correct Rust implementation exists to be measured against, and bit-exact with it. |

[`ENGINEERING_RULES.md`](ENGINEERING_RULES.md) is the normative version of that
table and of everything else. The table is a summary; the rules govern.

## Inherited law

SapStudio follows three Sapote rules:

**Bounded contracts.** Each feature has explicit limits. A wider surface needs
its own contract and evidence.

**Evidence over assertion.** Verification records the checked contract and its
conditions. Screenshots cover presentation only.

**Refuse instead of repair.** Malformed or ambiguous input returns a named error
without partial publication.

## Scope exclusions

The project excludes cross-platform and POSIX compatibility, telemetry,
analytics, cloud services, proprietary codec SDKs, and code whose license is
incompatible with GPL-3.0-only. Network and plugin features are outside the
current product scope.

## Documents

| Document | Contents |
| --- | --- |
| [`ENGINEERING_RULES.md`](ENGINEERING_RULES.md) | The normative rules. Every other document defers to it. |
| [`DEPENDENCIES.md`](DEPENDENCIES.md) | The open-source map: every component considered, its licence, and its verdict. |
| [`DEPENDENCY_POLICY.md`](DEPENDENCY_POLICY.md) | How a dependency enters the tree, and how it leaves. |
| [`PLATFORM_CONTRACT.md`](PLATFORM_CONTRACT.md) | SapStudio's operating-system requirements and current Sapote integration. |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | The crate map and data model. |
| [`ROADMAP.md`](ROADMAP.md) | Current priorities and release criteria. |
| [`VERIFICATION.md`](VERIFICATION.md) | What counts as evidence. |
| [`BRAND.md`](BRAND.md) | The mark, the palette, the naming, the voice. |
| [`GLOSSARY.md`](GLOSSARY.md) | Editing vocabulary, defined exactly enough to implement. |

SapStudio is licensed under [GPL-3.0-only](../LICENSE), the same licence as
Sapote.
