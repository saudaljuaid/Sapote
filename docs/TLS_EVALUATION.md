<!-- SPDX-License-Identifier: GPL-3.0-only -->

# TLS evaluation

Sapote 2.1.0 does not implement TLS or HTTPS. `https://` is rejected by the
HTTP parser, and degraded entropy is reported honestly. Plain HTTP provides no
server authentication, confidentiality, or integrity against an active peer.

## Required platform work

TLS cannot be enabled by adding a cipher implementation alone. The platform
must first provide:

- a cryptographically strong, continuously health-checked entropy source and
  DRBG with explicit failure behavior;
- the available validated RTC calendar time plus a policy for rollback;
- a checksum-pinned root store with a documented update/revocation process;
- strict DNS-name/SAN matching, SNI, certificate-chain limits, signature and
  key-size policy, expiry checks, unknown-critical-extension refusal, and test
  vectors for every parser;
- secret-zeroization rules, bounded stacks/buffers, constant-time primitive
  selection, and no secret-dependent logging;
- nonblocking TLS state integrated with authenticated handles, readiness,
  deadlines, cancellation, reset, partial I/O, close-notify and process death;
- reproducible source pinning, license review, fuzzing, interoperability tests,
  malformed-record/certificate controls, and independent security review.

## Library assessment

BearSSL is the closest architectural fit for an initial experiment. Its
[documented design](https://bearssl.org/) uses a caller-driven state machine,
does no dynamic allocation, is intended for small and bare-metal systems, and
can integrate with polling outside the engine. Its
[API documentation](https://bearssl.org/apidoc/index.html) makes callers own
all context and record buffers, which is compatible with Sapote's explicit
bounds.

It is not ready to ship unchanged. The published release is TLS 1.2-focused;
the project describes TLS 1.3 and richer X.509 path building as unresolved
design work. Sapote would still own entropy, time, trust anchors, hostname
policy, I/O adaptation, updates, zeroization, and security response. A pinned
BearSSL experiment must therefore remain disabled by default and cannot justify
an HTTPS claim.

Other options are less suitable today:

- OpenSSL assumes a much broader hosted runtime and dynamic allocation surface.
- mbed TLS is configurable and well known in embedded systems, but its selected
  configuration, allocator behavior and platform layer would still need a
  larger audited seam.
- rustls would require a substantially more complete Rust userspace runtime,
  allocator, threading and ecosystem port than Sapote currently has.

## Proposed bounded profile

If the prerequisites are completed, the first experimental profile should be a
client-only, static build with one connection, TLS 1.2 ECDHE, AEAD-only cipher
suites, SNI, strict WebPKI validation, fixed record and chain buffers, no client
certificates, no renegotiation, no session persistence, and explicit refusal of
unsupported algorithms or oversized messages. This profile is deliberately
narrow and would not constitute broad web compatibility.

## Release gate

HTTPS may be named as a capability only after an exact-commit QEMU capture shows
DNS, TCP, handshake, certificate validation, encrypted HTTP, close and resource
teardown; the packet audit confirms that HTTP plaintext is absent from PCAP;
expired, wrong-name, unknown-root, malformed-chain, weak-algorithm, bad-record,
truncation, replay, entropy-failure, timeout and reset controls all fail closed;
and an independent review has no unresolved high-severity finding.
