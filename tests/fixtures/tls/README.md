<!-- SPDX-License-Identifier: GPL-3.0-only -->

# Offline TLS fixture

These keys and certificates exist only for Phipia's deterministic local TLS
peer. They are public test material and must never be installed as production
trust roots. The fixed realtime used by the host client is 2026-08-31 12:00 UTC.

`ca.pem` is the trusted offline root. `anchor.txt` is the same root's DER
subject name and RSA public key encoded for the BearSSL test client. The valid,
expired, future, and untrusted leaf records all use the hostname
`repo.phipia.test`; the untrusted leaf is signed by a deliberately absent root.
The Python peer consumes the committed PEM records without network access or
certificate generation.

SHA-256:

```text
513d2a7cf57d96d19a09f7e3f0ff0f0c2469dfe68c131a94ba02eb97bc96cf96  anchor.txt
dcd3566c582a1401aec21baecaf4bef30fbc1ac2884aaf133def4cfe7069c415  ca.pem
f207af1bc263e1acee9d40f12f58856a5e107b56e6da45fbc10ab72b23839936  expired-key.pem
bb6374235d6571eec44e4f5bf0cd06b968a2cadff01885cd71e8166fa8dd2a8d  expired.pem
83401a2d3db347b41b35014e384dd601b42ef81587daf063d3ca357b112ffa51  future-key.pem
e86c66bf7ebc93719beb4537f5074f187dfe6cfcec68f91f730bf6fb3be40155  future.pem
4db57fbd96f5810ed07c17c9cb032ece5b4a3aec59f97736abb5c2b469afea8e  untrusted-key.pem
ba6b587f0f94ca37f6a57acb3798daa949939e768ef2f1bca994d86984909b52  untrusted.pem
55c0533e694dc6e038cb8cc533e132e74d356a40972a684ca69d094bb59764fe  valid-key.pem
b59d6869fef52d81beb6aae82bc8299bbfb7fd75f20e553aa87938e4af8513a8  valid.pem
```
