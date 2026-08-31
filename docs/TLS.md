<!-- SPDX-License-Identifier: GPL-3.0-only -->

# TLS client boundary

Sapote's first transport-TLS profile uses the pinned BearSSL 0.6 archive in the
SDK. `sapote/tls.h` is a native userspace client API; it uses only public Sapote
DNS, stream, entropy, realtime, monotonic-deadline, and handle services.

The profile is deliberately narrow:

- TLS 1.2 only;
- `ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256`, followed by
  `ECDHE_RSA_WITH_AES_128_GCM_SHA256`;
- RSA certificate keys of at least 2,048 bits, with RSA trust anchors admitted
  only through 4,096 bits; P-256 EC trust anchors are also accepted;
- lowercase canonical DNS hostnames, 253 bytes total and 63 bytes per label;
- at most 16 external CA trust anchors;
- no renegotiation or session resumption;
- a fixed 4,096-step handshake work bound plus monotonic deadlines on every
  underlying transport operation.

`sapote_tls_client_open()` refuses an empty or malformed trust store. It passes
the same nonempty hostname to DNS, SNI, and BearSSL's minimal X.509 validator,
so a valid chain for another host is not accepted. It reads the kernel's
validated realtime seconds and converts them to BearSSL's proleptic-Gregorian
day count for certificate validity. Monotonic time remains the only deadline
source. Thirty-two bytes from the native entropy service seed each independent
client engine; host random and clock adapters are disabled at compile time.

Every failure after stream creation shuts down and closes the stream and clears
the client allocation. `sapote_tls_client_close()` attempts authenticated
`close_notify`, then tears down the transport even when the peer omits its
reply. BearSSL and native transport error values remain separately queryable
while the client is alive.

## Trust-anchor lifetime

The API accepts BearSSL's public `br_x509_trust_anchor` records. Their DN and
key byte arrays are caller-owned and must remain immutable for the entire
client lifetime. This avoids copying attacker-sized certificate material into
unbounded SDK allocations. Proof packages generate their fixed test anchor
from the deterministic offline CA; a future system trust-store service must
enforce the same bounds before constructing these records.

## Current evidence boundary

The source and freestanding archive are reproducible and the API is wired to
real Sapote services. That is not yet an HTTPS success claim. Required QEMU
evidence still includes a deterministic offline peer and CA, successful
hostname/chain/time validation, wrong-host and unknown-root refusal, expired and
not-yet-valid certificates, truncated records, timeout/cancel behavior,
authenticated close, concurrent-session isolation, HTTP bounds, and package
digest failure after a valid TLS connection.
