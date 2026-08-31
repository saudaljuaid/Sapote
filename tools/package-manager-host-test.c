/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sapote/package_manager.h>

#define FIXTURE_NOW UINT64_C(1800000060)
#define FIXTURE_REPOSITORY_VERSION UINT64_C(42)
#define INSTALLED_PACKAGE_COUNT 2U
#define INSTALLED_EDGE_COUNT 1U
#define INSTALLED_EDGE_OFFSET (PACKAGE_STATE_DATABASE_HEADER_BYTES + \
    INSTALLED_PACKAGE_COUNT * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES)
#define INSTALLED_BYTES (INSTALLED_EDGE_OFFSET + \
    INSTALLED_EDGE_COUNT * PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES)

#define CHECK(condition) do { \
    if (!(condition)) { \
        (void)fprintf(stderr, "package-manager host check failed at line %d: %s\n", \
            __LINE__, #condition); \
        return 1; \
    } \
} while (0)

struct file_bytes {
    uint8_t *bytes;
    size_t count;
};

enum trust_mode {
    TRUST_NORMAL = 0,
    TRUST_UNKNOWN,
    TRUST_REVOKED,
    TRUST_REJECT_SIGNATURE
};

struct trust_context {
    uint8_t root_public[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES];
    uint8_t publisher_public[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES];
    enum trust_mode mode;
    uint32_t verification_count;
};

static bool same_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;

    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static bool any_bytes(const uint8_t *bytes, size_t count)
{
    uint8_t combined = 0U;

    for (size_t index = 0U; index < count; ++index) {
        combined |= bytes[index];
    }
    return combined != 0U;
}

static bool text_is(const struct package_manager_text *text, const char *value)
{
    size_t length = strlen(value);

    return text->length == length &&
        same_bytes(text->bytes, (const uint8_t *)value, length);
}

static struct file_bytes read_file(const char *path, size_t maximum)
{
    struct file_bytes result = { NULL, 0U };
    FILE *file = fopen(path, "rb");
    long length;

    if (file == NULL || fseek(file, 0L, SEEK_END) != 0) {
        if (file != NULL) {
            (void)fclose(file);
        }
        return result;
    }
    length = ftell(file);
    if (length <= 0L || (uint64_t)length > maximum ||
        fseek(file, 0L, SEEK_SET) != 0) {
        (void)fclose(file);
        return result;
    }
    result.bytes = malloc((size_t)length);
    if (result.bytes == NULL ||
        fread(result.bytes, 1U, (size_t)length, file) != (size_t)length ||
        fclose(file) != 0) {
        free(result.bytes);
        result.bytes = NULL;
        return result;
    }
    result.count = (size_t)length;
    return result;
}

static enum package_manager_key_status trust_lookup(
    void *opaque,
    const uint8_t key_id[PACKAGE_MANAGER_SHA256_BYTES],
    uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES]
)
{
    struct trust_context *context = opaque;
    uint8_t root_id[PACKAGE_MANAGER_SHA256_BYTES];
    uint8_t publisher_id[PACKAGE_MANAGER_SHA256_BYTES];

    if (context->mode == TRUST_UNKNOWN) {
        return PACKAGE_MANAGER_KEY_UNKNOWN;
    }
    if (context->mode == TRUST_REVOKED) {
        return PACKAGE_MANAGER_KEY_REVOKED;
    }
    if (package_state_sha256(context->root_public,
            sizeof(context->root_public), root_id) != PACKAGE_STATE_STATUS_OK ||
        package_state_sha256(context->publisher_public,
            sizeof(context->publisher_public), publisher_id) !=
            PACKAGE_STATE_STATUS_OK) {
        return PACKAGE_MANAGER_KEY_UNKNOWN;
    }
    if (same_bytes(key_id, root_id, sizeof(root_id))) {
        (void)memcpy(public_key, context->root_public, sizeof(context->root_public));
        return PACKAGE_MANAGER_KEY_TRUSTED;
    }
    if (same_bytes(key_id, publisher_id, sizeof(publisher_id))) {
        (void)memcpy(public_key, context->publisher_public,
            sizeof(context->publisher_public));
        return PACKAGE_MANAGER_KEY_TRUSTED;
    }
    return PACKAGE_MANAGER_KEY_UNKNOWN;
}

/*
 * The Python driver verifies these exact fixtures with real Ed25519 first.
 * This callback verifies the C trust-plumbing contract: the selected key,
 * embedded signature range and zeroed-message range must all agree.  It is not
 * a substitute for the still-missing in-kernel Ed25519 provider.
 */
static bool trust_verify(
    void *opaque,
    const uint8_t public_key[PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES],
    const uint8_t signature[PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES],
    const uint8_t *message,
    size_t message_bytes,
    size_t zero_offset,
    size_t zero_bytes
)
{
    struct trust_context *context = opaque;
    const bool known_key = same_bytes(public_key, context->root_public,
            sizeof(context->root_public)) ||
        same_bytes(public_key, context->publisher_public,
            sizeof(context->publisher_public));

    if (context->mode == TRUST_REJECT_SIGNATURE || !known_key ||
        zero_bytes != PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES ||
        zero_offset > message_bytes || zero_bytes > message_bytes - zero_offset ||
        signature != message + zero_offset || !any_bytes(signature, zero_bytes) ||
        (zero_offset != 232U && zero_offset != 440U)) {
        return false;
    }
    ++context->verification_count;
    return true;
}

static struct package_manager_policy normal_policy(void)
{
    const struct package_manager_policy result = {
        FIXTURE_NOW, FIXTURE_REPOSITORY_VERSION, 1U, false
    };

    return result;
}

static struct package_manager_trust make_trust(struct trust_context *context)
{
    const struct package_manager_trust result = {
        trust_lookup, trust_verify, context
    };

    return result;
}

static bool find_entry(
    const struct package_manager_repository_view *repository,
    const char *identifier,
    struct package_manager_catalog_entry *result
)
{
    for (uint32_t index = 0U; index < repository->package_count; ++index) {
        if (package_manager_repository_entry(repository, index, result) ==
                PACKAGE_MANAGER_STATUS_OK && text_is(&result->identifier, identifier)) {
            return true;
        }
    }
    return false;
}

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void put_u64(uint8_t *bytes, uint64_t value)
{
    put_u32(bytes, (uint32_t)value);
    put_u32(bytes + 4U, (uint32_t)(value >> 32U));
}

static void put_text(uint8_t *field, size_t width, const char *text)
{
    size_t length = strlen(text);

    (void)memset(field, 0, width);
    if (length < width) {
        (void)memcpy(field, text, length);
    }
}

static void put_magic(uint8_t *field, const char *magic)
{
    (void)memcpy(field, magic, 8U);
}

static bool build_installed_database(
    uint8_t database[INSTALLED_BYTES],
    const struct package_manager_catalog_entry *application,
    const struct package_manager_catalog_entry *library
)
{
    uint8_t *application_record;
    uint8_t *library_record;
    uint8_t *edge;
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

    (void)memset(database, 0, INSTALLED_BYTES);
    put_magic(database, "SAPIDB01");
    put_u16(database + 8U, 1U);
    put_u16(database + 10U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    put_u64(database + 16U, INSTALLED_BYTES);
    put_u64(database + 24U, 1U);
    put_u32(database + 32U, 1U);
    put_text(database + 40U, 16U, "x86_64");
    put_u64(database + 56U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    put_u32(database + 64U, INSTALLED_PACKAGE_COUNT);
    put_u32(database + 68U, PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES);
    put_u64(database + 72U, INSTALLED_EDGE_OFFSET);
    put_u32(database + 80U, INSTALLED_EDGE_COUNT);
    put_u32(database + 84U, PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES);
    put_u64(database + 88U, INSTALLED_BYTES);
    put_u32(database + 96U, 0U);
    put_u32(database + 100U, PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES);

    application_record = database + PACKAGE_STATE_DATABASE_HEADER_BYTES;
    library_record = application_record +
        PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES;
    edge = database + INSTALLED_EDGE_OFFSET;
    put_text(application_record, 64U, "org.sapote.app");
    put_text(application_record + 64U, 64U, "1.0.0");
    (void)memcpy(application_record + 128U, application->package_sha256, 32U);
    (void)memcpy(application_record + 160U, application->publisher_key_id, 32U);
    put_u32(application_record + 192U, 1U);
    put_u32(application_record + 196U, 0U);
    put_u32(application_record + 200U, 1U);
    put_u32(application_record + 204U, 0U);

    put_text(library_record, 64U, "org.sapote.lib");
    put_text(library_record + 64U, 64U, "1.0.0");
    (void)memcpy(library_record + 128U, library->package_sha256, 32U);
    (void)memcpy(library_record + 160U, library->publisher_key_id, 32U);
    put_u32(library_record + 192U, 0U);
    put_u32(library_record + 196U, 1U);
    put_u32(library_record + 200U, 0U);
    put_u32(library_record + 204U, 0U);

    put_text(edge, 64U, "org.sapote.lib");
    put_text(edge + 64U, 56U, "^1.0.0");
    put_text(edge + 120U, 64U, "org.sapote.lib");
    if (package_state_sha256(database + PACKAGE_STATE_DATABASE_HEADER_BYTES,
            INSTALLED_BYTES - PACKAGE_STATE_DATABASE_HEADER_BYTES, digest) !=
            PACKAGE_STATE_STATUS_OK) {
        return false;
    }
    (void)memcpy(database + 104U, digest, sizeof(digest));
    return true;
}

static int expect_plan_status(
    const char *path,
    const char *identifier,
    enum package_manager_status expected,
    struct trust_context *context
)
{
    struct file_bytes index = read_file(path,
        PACKAGE_MANAGER_REPOSITORY_MAX_BYTES);
    struct package_manager_policy policy = normal_policy();
    struct package_manager_trust trust = make_trust(context);
    struct package_manager_repository_view repository;
    struct package_manager_plan plan;
    enum package_manager_status status;

    if (index.bytes == NULL || package_manager_repository_open(index.bytes,
            index.count, &policy, &trust, &repository) !=
            PACKAGE_MANAGER_STATUS_OK) {
        free(index.bytes);
        return 1;
    }
    status = package_manager_plan_install(&repository, NULL,
        (const uint8_t *)identifier, strlen(identifier), &policy, &trust, &plan);
    free(index.bytes);
    return status == expected ? 0 : 1;
}

int main(int argc, char **argv)
{
    struct file_bytes repository_bytes;
    struct file_bytes root_key;
    struct file_bytes publisher_key;
    struct file_bytes application_bytes;
    struct file_bytes library_bytes;
    struct trust_context context;
    struct package_manager_trust trust;
    struct package_manager_policy policy = normal_policy();
    struct package_manager_repository_view repository;
    struct package_manager_catalog_entry application_entry;
    struct package_manager_catalog_entry library_entry;
    struct package_manager_package_view application_package;
    struct package_manager_package_view library_package;
    struct package_manager_search_results search;
    struct package_manager_plan plan;
    struct package_state_database_view installed;
    uint8_t installed_bytes[INSTALLED_BYTES];
    uint8_t *changed;

    CHECK(argc == 12);
    repository_bytes = read_file(argv[1], PACKAGE_MANAGER_REPOSITORY_MAX_BYTES);
    root_key = read_file(argv[2], PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
    publisher_key = read_file(argv[3], PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES);
    application_bytes = read_file(argv[4], PACKAGE_MANAGER_PACKAGE_MAX_BYTES);
    library_bytes = read_file(argv[5], PACKAGE_MANAGER_PACKAGE_MAX_BYTES);
    CHECK(repository_bytes.bytes != NULL &&
        root_key.count == PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES &&
        publisher_key.count == PACKAGE_MANAGER_ED25519_PUBLIC_KEY_BYTES &&
        application_bytes.bytes != NULL && library_bytes.bytes != NULL);
    (void)memset(&context, 0, sizeof(context));
    (void)memcpy(context.root_public, root_key.bytes, sizeof(context.root_public));
    (void)memcpy(context.publisher_public, publisher_key.bytes,
        sizeof(context.publisher_public));
    trust = make_trust(&context);

    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
        PACKAGE_MANAGER_STATUS_OK);
    CHECK(repository.repository_version == FIXTURE_REPOSITORY_VERSION &&
        repository.package_count == 2U && context.verification_count == 1U &&
        find_entry(&repository, "org.sapote.app", &application_entry) &&
        find_entry(&repository, "org.sapote.lib", &library_entry));
    CHECK(package_manager_repository_search(&repository,
        (const uint8_t *)"LIB", 3U, &search) == PACKAGE_MANAGER_STATUS_OK &&
        search.count == 1U && search.repository_indices[0] ==
            library_entry.repository_index);
    CHECK(package_manager_plan_install(&repository, NULL,
        (const uint8_t *)"org.sapote.app", 14U, &policy, &trust, &plan) ==
            PACKAGE_MANAGER_STATUS_OK &&
        plan.operation == PACKAGE_MANAGER_PLAN_INSTALL && plan.count == 2U &&
        text_is(&plan.items[0].identifier, "org.sapote.lib") &&
        text_is(&plan.items[1].identifier, "org.sapote.app"));

    CHECK(package_manager_package_open(application_bytes.bytes,
        application_bytes.count, &application_entry, &policy, &trust,
        &application_package) == PACKAGE_MANAGER_STATUS_OK &&
        text_is(&application_package.identifier, "org.sapote.app") &&
        application_package.file_count == 1U);
    CHECK(package_manager_package_open(library_bytes.bytes, library_bytes.count,
        &library_entry, &policy, &trust, &library_package) ==
            PACKAGE_MANAGER_STATUS_OK &&
        text_is(&library_package.identifier, "org.sapote.lib") &&
        library_package.file_count == 1U && context.verification_count == 3U);

    CHECK(build_installed_database(installed_bytes, &application_entry,
        &library_entry));
    CHECK(package_state_database_parse(installed_bytes, INSTALLED_BYTES,
        &installed) == PACKAGE_STATE_STATUS_OK);
    CHECK(package_manager_plan_install(&repository, &installed,
        (const uint8_t *)"org.sapote.app", 14U, &policy, &trust, &plan) ==
            PACKAGE_MANAGER_STATUS_ALREADY_INSTALLED);
    CHECK(package_manager_plan_remove(&installed,
        (const uint8_t *)"org.sapote.app", 14U, &plan) ==
            PACKAGE_MANAGER_STATUS_OK && plan.count == 2U &&
        text_is(&plan.items[0].identifier, "org.sapote.app") &&
        text_is(&plan.items[1].identifier, "org.sapote.lib"));
    CHECK(package_manager_plan_remove(&installed,
        (const uint8_t *)"org.sapote.lib", 14U, &plan) ==
            PACKAGE_MANAGER_STATUS_IN_USE);
    CHECK(package_manager_installed_search(&installed,
        (const uint8_t *)"APP", 3U, &search) == PACKAGE_MANAGER_STATUS_OK &&
        search.count == 1U && search.repository_indices[0] == 0U);

    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, NULL, &repository) ==
            PACKAGE_MANAGER_STATUS_CRYPTO_UNAVAILABLE);
    context.mode = TRUST_UNKNOWN;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_UNKNOWN_KEY);
    context.mode = TRUST_REVOKED;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_REVOKED_KEY);
    context.mode = TRUST_REJECT_SIGNATURE;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_SIGNATURE);
    context.mode = TRUST_NORMAL;
    policy.minimum_repository_version = FIXTURE_REPOSITORY_VERSION + 1U;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_ROLLBACK);
    policy = normal_policy();
    policy.now = FIXTURE_NOW - 1000U;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_FRESHNESS);
    policy = normal_policy();
    policy.abi = 2U;
    CHECK(package_manager_repository_open(repository_bytes.bytes,
        repository_bytes.count, &policy, &trust, &repository) ==
            PACKAGE_MANAGER_STATUS_ABI);
    policy = normal_policy();

    changed = malloc(repository_bytes.count);
    CHECK(changed != NULL);
    (void)memcpy(changed, repository_bytes.bytes, repository_bytes.count);
    changed[repository_bytes.count - 1U] ^= UINT8_C(1);
    CHECK(package_manager_repository_open(changed, repository_bytes.count,
        &policy, &trust, &repository) == PACKAGE_MANAGER_STATUS_DIGEST);
    (void)memcpy(changed, repository_bytes.bytes, repository_bytes.count);
    (void)memset(changed + 232U, 0, PACKAGE_MANAGER_ED25519_SIGNATURE_BYTES);
    CHECK(package_manager_repository_open(changed, repository_bytes.count,
        &policy, &trust, &repository) == PACKAGE_MANAGER_STATUS_RESERVED);
    free(changed);

    context.mode = TRUST_REJECT_SIGNATURE;
    CHECK(package_manager_package_open(application_bytes.bytes,
        application_bytes.count, &application_entry, &policy, &trust,
        &application_package) == PACKAGE_MANAGER_STATUS_SIGNATURE);
    context.mode = TRUST_NORMAL;
    changed = malloc(application_bytes.count);
    CHECK(changed != NULL);
    (void)memcpy(changed, application_bytes.bytes, application_bytes.count);
    changed[application_bytes.count - 1U] ^= UINT8_C(1);
    CHECK(package_manager_package_open(changed, application_bytes.count,
        &application_entry, &policy, &trust, &application_package) ==
            PACKAGE_MANAGER_STATUS_DIGEST);
    free(changed);

    CHECK(expect_plan_status(argv[6], "org.sapote.a",
        PACKAGE_MANAGER_STATUS_CYCLE, &context) == 0);
    CHECK(expect_plan_status(argv[7], "org.sapote.conflict-app",
        PACKAGE_MANAGER_STATUS_CONFLICT, &context) == 0);
    CHECK(expect_plan_status(argv[8], "org.sapote.ambiguous-app",
        PACKAGE_MANAGER_STATUS_AMBIGUOUS_PROVIDER, &context) == 0);
    CHECK(expect_plan_status(argv[9], "org.sapote.unsatisfied",
        PACKAGE_MANAGER_STATUS_DEPENDENCY, &context) == 0);
    CHECK(expect_plan_status(argv[10], "org.sapote.backtrack",
        PACKAGE_MANAGER_STATUS_OK, &context) == 0);
    CHECK(expect_plan_status(argv[11], "org.sapote.chain00",
        PACKAGE_MANAGER_STATUS_GRAPH_BOUND, &context) == 0);

    free(repository_bytes.bytes);
    free(root_key.bytes);
    free(publisher_key.bytes);
    free(application_bytes.bytes);
    free(library_bytes.bytes);
    (void)puts("Sapote bounded guest package-manager core tests passed");
    return 0;
}
