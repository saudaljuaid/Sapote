<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Future browser-engine port plan

Sapote 2.1.0 does not contain a browser, browser icon, HTML renderer,
JavaScript engine, or HTTPS client. This document selects a concrete direction
so the networking ABI is judged against a real consumer instead of an invented
demo.

## Candidate: NetSurf framebuffer frontend

[NetSurf](https://www.netsurf-browser.org/about/) is the preferred first port.
It has its own C layout engine, emphasizes a small footprint and portability,
and already has a framebuffer frontend whose plotters draw the browser UI and
page without requiring a host GUI toolkit. The project explicitly welcomes
[new platform ports](https://www.netsurf-browser.org/developers/). Those traits
fit Sapote better than Chromium, Firefox, or a large C++/JIT engine.

This is a design selection, not a vendored dependency or compatibility claim.
A port must pin and audit one exact upstream source commit and all transitive
libraries before any NetSurf code enters this repository.

## Port layers

1. **Freestanding build.** Establish a separate Ring 3 target and static link
   of the NetSurf core, framebuffer frontend, LibCSS, Hubbub/LibDOM, parser and
   image dependencies. Replace hosted libc assumptions with a measured native
   runtime; do not extend the Linux-compatibility allowlist to disguise gaps.
2. **Process service.** Add authenticated process creation, multiple user
   mappings, bounded heap growth, exit, clock, sleep, and event delivery. Keep
   executable aliases W^X and make every allocation visible to a resource
   census.
3. **Frontend surface.** Expose a bounded shared or copied pixel surface,
   damage rectangles, keyboard/pointer events, clipboard refusal, and one
   Sapote Redwood window. The NetSurf plotter API remains above this seam.
4. **URL fetcher.** Implement the NetSurf fetch callbacks over networking ABI
   v1: DNS, stream open/connect/read/write/shutdown/close, poll, cancel, and
   monotonic deadlines. HTTP parsing should move to the browser only after its
   parser has equivalent or stronger bounds; the kernel HTTP helper remains
   available for downloads.
5. **Resource loader.** Add read-only application resources on the System
   volume and a separate bounded cache on Data. Cache writes use temporary-file
   sync-and-replace. No web content may write System.
6. **TLS gate.** Browser HTTPS remains disabled until the wider release gate in
   `TLS_EVALUATION.md` is implemented and independently tested. The bounded SDK
   client is not a browser root-store, origin-policy, or update solution. Plain
   HTTP UI must visibly state that transport is unauthenticated.
7. **Content isolation.** Run the frontend and fetched content in a private
   process generation. Add per-origin storage only after path, quota, eviction,
   and teardown policies exist. Downloads require an explicit user action and
   an 8.3 destination under Data.

## Missing platform contracts

The current kernel lacks several services a browser engine normally assumes:

- a browser-scale allocator and a policy for many long-lived native processes;
- condition variables and browser-scale asynchronous worker completion beyond
  the bounded native thread, TLS, and mutex services;
- locale, Unicode normalization/shaping, scalable fonts, clipboard and IME;
- dynamic loading, sockets compatible with a hosted libc, signals and files as
  general descriptors;
- a maintained browser root certificate store and clock rollback policy;
- browser-integrated/release-gated TLS, compression/content-encoding, cookies,
  cache policy and same-origin storage;
- robust crash containment, watchdogs, per-process quotas, and update delivery.

The first executable target is an offline, pinned HTML/CSS fixture rendered in
a private process with no networking. Plain HTTP against the offline peer comes
next. Internet URLs, JavaScript, downloads, and browser HTTPS remain separate
projects.

## Acceptance sequence

Each stage needs host parser tests, a real QEMU scenario, resource-census
equality, malformed-input controls, and authentic framebuffer evidence:

1. offline static page and image;
2. input, scrolling, navigation and bounded history;
3. deterministic plain-HTTP A/CNAME/redirect page load;
4. cancellation, timeout, reset and out-of-memory recovery;
5. synchronized user-approved Data download;
6. only then, independently reviewed TLS and HTTPS.

Until all six pass, Sapote release notes must say “browser foundation” or
“future browser port plan,” never “browser support.”
