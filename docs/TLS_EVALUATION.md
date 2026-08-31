<!-- SPDX-License-Identifier: GPL-3.0-only -->

# TLS evaluation and release gate

Sapote now has a pinned, bounded BearSSL TLS 1.2 SDK client and strict
single-response HTTPS download profile. The desktop HTTP parser remains a
separate plaintext facility and does not upgrade `https://` URLs. This SDK
slice is not yet a general Internet or release-ready HTTPS claim; see `TLS.md`
and `HTTPS.md` for its implemented boundary and evidence.

## Platform requirements

TLS was not enabled by adding a cipher implementation alone. The bounded SDK
profile consumes kernel entropy, validated realtime, DNS, stream, deadline,
cancellation, and handle services. Release work still includes:

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

BearSSL is the selected architectural fit for the bounded client. Its
[documented design](https://bearssl.org/) uses a caller-driven state machine,
does no dynamic allocation, is intended for small and bare-metal systems, and
can integrate with polling outside the engine. Its
[API documentation](https://bearssl.org/apidoc/index.html) makes callers own
all context and record buffers, which is compatible with Sapote's explicit
bounds.

It is not a complete web stack. The published release is TLS 1.2-focused;
the project describes TLS 1.3 and richer X.509 path building as unresolved
design work. Sapote still owns entropy, time, trust anchors, hostname policy,
I/O adaptation, updates, zeroization, and security response. The pinned
profile therefore cannot by itself justify a release-wide HTTPS claim.

Other options are less suitable today:

- OpenSSL assumes a much broader hosted runtime and dynamic allocation surface.
- mbed TLS is configurable and well known in embedded systems, but its selected
  configuration, allocator behavior and platform layer would still need a
  larger audited seam.
- rustls would require a substantially more complete Rust userspace runtime,
  allocator, threading and ecosystem port than Sapote currently has.

## Implemented bounded profile

The first profile is a client-only static build with one connection, TLS 1.2
ECDHE, AEAD-only cipher suites, SNI, external bounded trust anchors, strict
hostname/chain/time validation, fixed record and header buffers, no client
certificates, no renegotiation, no session persistence, and explicit refusal
of unsupported or oversized HTTP records. It deliberately does not constitute
broad web compatibility.

## Release gate

HTTPS may be named as a capability only after an exact-commit QEMU capture shows
DNS, TCP, handshake, certificate validation, encrypted HTTP, close and resource
teardown; the packet audit confirms that HTTP plaintext is absent from PCAP;
expired, wrong-name, unknown-root, malformed-chain, weak-algorithm, bad-record,
truncation, replay, entropy-failure, timeout and reset controls all fail closed;
and an independent review has no unresolved high-severity finding.

The bounded SDK/QEMU profile does not claim to pass this wider release gate.
