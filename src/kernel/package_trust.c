/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/package_trust.h>

#include <monocypher-ed25519.h>

static const uint8_t field_prime[32] = {
    0xedU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
    0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x7fU
};

/* Canonical low-order encodings plus the two canonical-y negative-zero forms. */
static const uint8_t invalid_points[10][32] = {
    { 0x01U },
    {
        0x26U, 0xe8U, 0x95U, 0x8fU, 0xc2U, 0xb2U, 0x27U, 0xb0U,
        0x45U, 0xc3U, 0xf4U, 0x89U, 0xf2U, 0xefU, 0x98U, 0xf0U,
        0xd5U, 0xdfU, 0xacU, 0x05U, 0xd3U, 0xc6U, 0x33U, 0x39U,
        0xb1U, 0x38U, 0x02U, 0x88U, 0x6dU, 0x53U, 0xfcU, 0x05U
    },
    { 0x00U },
    {
        0xc7U, 0x17U, 0x6aU, 0x70U, 0x3dU, 0x4dU, 0xd8U, 0x4fU,
        0xbaU, 0x3cU, 0x0bU, 0x76U, 0x0dU, 0x10U, 0x67U, 0x0fU,
        0x2aU, 0x20U, 0x53U, 0xfaU, 0x2cU, 0x39U, 0xccU, 0xc6U,
        0x4eU, 0xc7U, 0xfdU, 0x77U, 0x92U, 0xacU, 0x03U, 0x7aU
    },
    {
        0xecU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0x7fU
    },
    {
        0xc7U, 0x17U, 0x6aU, 0x70U, 0x3dU, 0x4dU, 0xd8U, 0x4fU,
        0xbaU, 0x3cU, 0x0bU, 0x76U, 0x0dU, 0x10U, 0x67U, 0x0fU,
        0x2aU, 0x20U, 0x53U, 0xfaU, 0x2cU, 0x39U, 0xccU, 0xc6U,
        0x4eU, 0xc7U, 0xfdU, 0x77U, 0x92U, 0xacU, 0x03U, 0xfaU
    },
    {
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U
    },
    {
        0x26U, 0xe8U, 0x95U, 0x8fU, 0xc2U, 0xb2U, 0x27U, 0xb0U,
        0x45U, 0xc3U, 0xf4U, 0x89U, 0xf2U, 0xefU, 0x98U, 0xf0U,
        0xd5U, 0xdfU, 0xacU, 0x05U, 0xd3U, 0xc6U, 0x33U, 0x39U,
        0xb1U, 0x38U, 0x02U, 0x88U, 0x6dU, 0x53U, 0xfcU, 0x85U
    },
    {
        0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U,
        0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x80U
    },
    {
        0xecU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU,
        0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU, 0xffU
    }
};

static bool equal_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;
    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static int compare_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        if (left[index] != right[index]) {
            return left[index] < right[index] ? -1 : 1;
        }
    }
    return 0;
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
}

static bool canonical_non_small_order_point(const uint8_t point[32])
{
    uint8_t y[32];
    bool less_than_prime = false;
    copy_bytes(y, point, sizeof(y));
    y[31] &= UINT8_C(0x7f);
    for (size_t index = sizeof(y); index > 0U; --index) {
        size_t current = index - 1U;
        if (y[current] != field_prime[current]) {
            less_than_prime = y[current] < field_prime[current];
            break;
        }
    }
    if (!less_than_prime) {
        return false;
    }
    for (size_t index = 0U; index <
        sizeof(invalid_points) / sizeof(invalid_points[0]); ++index) {
        if (equal_bytes(point, invalid_points[index], sizeof(invalid_points[index]))) {
            return false;
        }
    }
    return true;
}

enum package_trust_status package_trust_open(
    const struct package_trust_key *keys,
    size_t key_count,
    struct package_trust_store *result
)
{
    if (result == NULL) {
        return PACKAGE_TRUST_STATUS_NULL_ARGUMENT;
    }
    result->keys = NULL;
    result->key_count = 0U;
    if (keys == NULL && key_count != 0U) {
        return PACKAGE_TRUST_STATUS_NULL_ARGUMENT;
    }
    if (key_count > PACKAGE_TRUST_MAX_KEYS) {
        return PACKAGE_TRUST_STATUS_BOUND;
    }
    for (size_t index = 0U; index < key_count; ++index) {
        uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES];
        if (keys[index].status != PACKAGE_MANAGER_KEY_TRUSTED &&
            keys[index].status != PACKAGE_MANAGER_KEY_REVOKED) {
            return PACKAGE_TRUST_STATUS_PUBLIC_KEY;
        }
        if (package_state_sha256(keys[index].public_key,
            sizeof(keys[index].public_key), key_id) != PACKAGE_STATE_STATUS_OK ||
            !equal_bytes(key_id, keys[index].key_id, sizeof(key_id))) {
            return PACKAGE_TRUST_STATUS_KEY_ID;
        }
        if (!canonical_non_small_order_point(keys[index].public_key)) {
            return PACKAGE_TRUST_STATUS_PUBLIC_KEY;
        }
        if (index != 0U && compare_bytes(keys[index - 1U].key_id,
            keys[index].key_id, sizeof(keys[index].key_id)) >= 0) {
            return PACKAGE_TRUST_STATUS_ORDER;
        }
    }
    result->keys = keys;
    result->key_count = key_count;
    return PACKAGE_TRUST_STATUS_OK;
}

enum package_manager_key_status package_trust_lookup(
    void *context,
    const uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES],
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES]
)
{
    const struct package_trust_store *store = context;
    if (store == NULL || key_id == NULL || public_key == NULL ||
        (store->keys == NULL && store->key_count != 0U) ||
        store->key_count > PACKAGE_TRUST_MAX_KEYS) {
        return PACKAGE_MANAGER_KEY_UNKNOWN;
    }
    for (size_t index = 0U; index < store->key_count; ++index) {
        if (equal_bytes(key_id, store->keys[index].key_id,
            PACKAGE_MANAGER_SHA256_BYTES)) {
            copy_bytes(public_key, store->keys[index].public_key,
                PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
            return store->keys[index].status;
        }
    }
    return PACKAGE_MANAGER_KEY_UNKNOWN;
}

bool package_trust_verify(
    void *context,
    const uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES],
    const uint8_t *message,
    size_t message_bytes,
    size_t zero_offset,
    size_t zero_bytes
)
{
    static const uint8_t zeros[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES] = { 0U };
    const struct package_trust_store *store = context;
    crypto_sha512_ctx hash_context;
    uint8_t expanded_hash[64];
    uint8_t reduced_hash[32];
    bool trusted = false;
    if (store == NULL || public_key == NULL || signature == NULL ||
        message == NULL || store->keys == NULL ||
        store->key_count == 0U || store->key_count > PACKAGE_TRUST_MAX_KEYS ||
        zero_bytes != PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES ||
        zero_offset > message_bytes || zero_bytes > message_bytes - zero_offset ||
        !canonical_non_small_order_point(public_key) ||
        !canonical_non_small_order_point(signature)) {
        return false;
    }
    for (size_t index = 0U; index < store->key_count; ++index) {
        if (store->keys[index].status == PACKAGE_MANAGER_KEY_TRUSTED &&
            equal_bytes(public_key, store->keys[index].public_key,
                PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES)) {
            trusted = true;
        }
    }
    if (!trusted) {
        return false;
    }
    crypto_sha512_init(&hash_context);
    crypto_sha512_update(&hash_context, signature, 32U);
    crypto_sha512_update(&hash_context, public_key,
        PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
    crypto_sha512_update(&hash_context, message, zero_offset);
    crypto_sha512_update(&hash_context, zeros, zero_bytes);
    crypto_sha512_update(&hash_context, message + zero_offset + zero_bytes,
        message_bytes - zero_offset - zero_bytes);
    crypto_sha512_final(&hash_context, expanded_hash);
    crypto_eddsa_reduce(reduced_hash, expanded_hash);
    return crypto_eddsa_check_equation(signature, public_key, reduced_hash) == 0;
}

void package_trust_manager(
    struct package_trust_store *store,
    struct package_manager_trust *result
)
{
    if (result == NULL) {
        return;
    }
    result->lookup = package_trust_lookup;
    result->verify = package_trust_verify;
    result->context = store;
}
