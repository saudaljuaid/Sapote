/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_PACKAGE_TRUST_H
#define SAPOTE_PACKAGE_TRUST_H

#include <stddef.h>
#include <stdint.h>

#include <sapote/package_manager.h>

#define PACKAGE_TRUST_MAX_KEYS 64U

enum package_trust_status {
    PACKAGE_TRUST_STATUS_OK = 0,
    PACKAGE_TRUST_STATUS_NULL_ARGUMENT,
    PACKAGE_TRUST_STATUS_BOUND,
    PACKAGE_TRUST_STATUS_ORDER,
    PACKAGE_TRUST_STATUS_KEY_ID,
    PACKAGE_TRUST_STATUS_PUBLIC_KEY,
    PACKAGE_TRUST_STATUS_COUNT
};

struct package_trust_key {
    uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES];
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES];
    enum package_manager_key_status status;
};

/* The admitted key array must remain live and immutable with the store. */
struct package_trust_store {
    const struct package_trust_key *keys;
    size_t key_count;
};

enum package_trust_status package_trust_open(
    const struct package_trust_key *keys,
    size_t key_count,
    struct package_trust_store *result
);

void package_trust_manager(
    struct package_trust_store *store,
    struct package_manager_trust *result
);

enum package_manager_key_status package_trust_lookup(
    void *context,
    const uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES],
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES]
);

bool package_trust_verify(
    void *context,
    const uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES],
    const uint8_t *message,
    size_t message_bytes,
    size_t zero_offset,
    size_t zero_bytes
);

#endif
