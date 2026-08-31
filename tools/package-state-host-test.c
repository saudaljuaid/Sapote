/* SPDX-License-Identifier: GPL-3.0-only */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/package_state.h>

#define OLD_DATABASE_BYTES PACKAGE_STATE_DATABASE_HEADER_BYTES
#define NEW_PACKAGE_COUNT 2U
#define NEW_EDGE_COUNT 1U
#define NEW_FILE_COUNT 2U
#define NEW_EDGE_OFFSET (PACKAGE_STATE_DATABASE_HEADER_BYTES + \
    NEW_PACKAGE_COUNT * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES)
#define NEW_FILE_OFFSET (NEW_EDGE_OFFSET + \
    NEW_EDGE_COUNT * PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES)
#define NEW_DATABASE_BYTES (NEW_FILE_OFFSET + \
    NEW_FILE_COUNT * PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES)

#define CHECK(condition, code) do { if (!(condition)) { return (code); } } while (0)

static void clear_bytes(uint8_t *bytes, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static void copy_bytes(uint8_t *destination, const uint8_t *source, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        destination[index] = source[index];
    }
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

static uint64_t get_u64(const uint8_t *bytes)
{
    return (uint64_t)bytes[0] |
        (uint64_t)bytes[1] << 8U |
        (uint64_t)bytes[2] << 16U |
        (uint64_t)bytes[3] << 24U |
        (uint64_t)bytes[4] << 32U |
        (uint64_t)bytes[5] << 40U |
        (uint64_t)bytes[6] << 48U |
        (uint64_t)bytes[7] << 56U;
}

static void put_text(uint8_t *field, size_t width, const char *text)
{
    size_t length = 0U;
    clear_bytes(field, width);
    while (text[length] != '\0') {
        field[length] = (uint8_t)text[length];
        ++length;
    }
}

static void put_magic(uint8_t *field, const char *magic)
{
    for (size_t index = 0U; index < 8U; ++index) {
        field[index] = (uint8_t)magic[index];
    }
}

static bool same_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t different = 0U;
    for (size_t index = 0U; index < count; ++index) {
        different |= left[index] ^ right[index];
    }
    return different == 0U;
}

static void digest_text(const char *text, uint8_t output[PACKAGE_STATE_SHA256_BYTES])
{
    size_t length = 0U;
    while (text[length] != '\0') {
        ++length;
    }
    if (package_state_sha256((const uint8_t *)text, length, output) !=
        PACKAGE_STATE_STATUS_OK) {
        output[0] = 1U;
    }
}

static void finalize_database(uint8_t *database, size_t byte_count)
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    if (package_state_sha256(database + PACKAGE_STATE_DATABASE_HEADER_BYTES,
        byte_count - PACKAGE_STATE_DATABASE_HEADER_BYTES, digest) ==
        PACKAGE_STATE_STATUS_OK) {
        copy_bytes(database + 104U, digest, sizeof(digest));
    }
}

static void database_header(
    uint8_t *database,
    size_t byte_count,
    uint64_t generation,
    uint32_t package_count,
    uint32_t edge_count,
    uint32_t file_count
)
{
    put_magic(database, "SAPIDB01");
    put_u16(database + 8U, 1U);
    put_u16(database + 10U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    put_u64(database + 16U, byte_count);
    put_u64(database + 24U, generation);
    put_u32(database + 32U, 1U);
    put_text(database + 40U, 16U, "x86_64");
    put_u64(database + 56U, PACKAGE_STATE_DATABASE_HEADER_BYTES);
    put_u32(database + 64U, package_count);
    put_u32(database + 68U, PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES);
    put_u64(database + 72U, PACKAGE_STATE_DATABASE_HEADER_BYTES +
        (uint64_t)package_count * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES);
    put_u32(database + 80U, edge_count);
    put_u32(database + 84U, PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES);
    put_u64(database + 88U, PACKAGE_STATE_DATABASE_HEADER_BYTES +
        (uint64_t)package_count * PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES +
        (uint64_t)edge_count * PACKAGE_STATE_DATABASE_EDGE_RECORD_BYTES);
    put_u32(database + 96U, file_count);
    put_u32(database + 100U, PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES);
}

static void build_old_database(uint8_t database[OLD_DATABASE_BYTES])
{
    clear_bytes(database, OLD_DATABASE_BYTES);
    database_header(database, OLD_DATABASE_BYTES, 1U, 0U, 0U, 0U);
    finalize_database(database, OLD_DATABASE_BYTES);
}

static void build_new_database(uint8_t database[NEW_DATABASE_BYTES])
{
    uint8_t *application;
    uint8_t *library;
    uint8_t *edge;
    uint8_t *application_file;
    uint8_t *library_file;

    clear_bytes(database, NEW_DATABASE_BYTES);
    database_header(database, NEW_DATABASE_BYTES, 2U,
        NEW_PACKAGE_COUNT, NEW_EDGE_COUNT, NEW_FILE_COUNT);
    application = database + PACKAGE_STATE_DATABASE_HEADER_BYTES;
    library = application + PACKAGE_STATE_DATABASE_PACKAGE_RECORD_BYTES;
    edge = database + NEW_EDGE_OFFSET;
    application_file = database + NEW_FILE_OFFSET;
    library_file = application_file + PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES;

    put_text(application, 64U, "org.sapote.app");
    put_text(application + 64U, 64U, "1.0.0");
    digest_text("application package", application + 128U);
    digest_text("publisher public key", application + 160U);
    put_u32(application + 192U, 1U);
    put_u32(application + 196U, 0U);
    put_u32(application + 200U, 1U);
    put_u32(application + 204U, 1U);

    put_text(library, 64U, "org.sapote.lib");
    put_text(library + 64U, 64U, "1.0.0");
    digest_text("library package", library + 128U);
    digest_text("publisher public key", library + 160U);
    put_u32(library + 192U, 0U);
    put_u32(library + 196U, 1U);
    put_u32(library + 200U, 0U);
    put_u32(library + 204U, 1U);

    put_text(edge, 64U, "org.sapote.lib");
    put_text(edge + 64U, 56U, "^1.0.0");
    put_text(edge + 120U, 64U, "org.sapote.lib");

    put_text(application_file, 128U, "bin/app");
    put_u32(application_file + 128U, 0U);
    put_u16(application_file + 132U, 1U);
    put_u32(application_file + 136U, 0555U);
    put_u64(application_file + 144U, 3U);
    digest_text("app", application_file + 152U);

    put_text(library_file, 128U, "lib/libx.so.1");
    put_u32(library_file + 128U, 1U);
    put_u16(library_file + 132U, 2U);
    put_u32(library_file + 136U, 0444U);
    put_u64(library_file + 144U, 3U);
    digest_text("lib", library_file + 152U);
    put_text(library_file + 184U, 64U, "libx.so.1");
    finalize_database(database, NEW_DATABASE_BYTES);
}

static void build_authority(
    uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t *database,
    size_t database_bytes
)
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    clear_bytes(authority, PACKAGE_STATE_AUTHORITY_BYTES);
    put_magic(authority, "SAPGEN01");
    put_u16(authority + 8U, 1U);
    put_u16(authority + 10U, PACKAGE_STATE_AUTHORITY_BYTES);
    put_u64(authority + 16U, get_u64(database + 24U));
    put_u64(authority + 24U, database_bytes);
    (void)package_state_sha256(database, database_bytes, authority + 32U);
    (void)package_state_sha256(authority, 64U, digest);
    copy_bytes(authority + 64U, digest, sizeof(digest));
}

static void rehash_authority(uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES])
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    (void)package_state_sha256(authority, 64U, digest);
    copy_bytes(authority + 64U, digest, sizeof(digest));
}

static void rehash_journal(uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES])
{
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];
    clear_bytes(journal + 128U, PACKAGE_STATE_SHA256_BYTES);
    (void)package_state_sha256(journal, PACKAGE_STATE_JOURNAL_BYTES, digest);
    copy_bytes(journal + 128U, digest, sizeof(digest));
}

static void build_journal(
    uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES],
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES]
)
{
    clear_bytes(journal, PACKAGE_STATE_JOURNAL_BYTES);
    put_magic(journal, "SAPTXN01");
    put_u16(journal + 8U, 1U);
    put_u16(journal + 10U, PACKAGE_STATE_JOURNAL_BYTES);
    put_u16(journal + 16U, PACKAGE_STATE_OPERATION_INSTALL);
    put_u16(journal + 18U, 1U);
    put_u64(journal + 24U, 1U);
    put_u64(journal + 32U, 2U);
    put_u64(journal + 40U, 4096U);
    put_u64(journal + 48U, OLD_DATABASE_BYTES);
    put_u64(journal + 56U, NEW_DATABASE_BYTES);
    (void)package_state_sha256(
        old_database, OLD_DATABASE_BYTES, journal + 64U);
    (void)package_state_sha256(
        new_database, NEW_DATABASE_BYTES, journal + 96U);
    put_text(journal + 160U, 64U, "org.sapote.app");
    rehash_journal(journal);
}

static int test_sha256(void)
{
    static const uint8_t expected[PACKAGE_STATE_SHA256_BYTES] = {
        0xbaU, 0x78U, 0x16U, 0xbfU, 0x8fU, 0x01U, 0xcfU, 0xeaU,
        0x41U, 0x41U, 0x40U, 0xdeU, 0x5dU, 0xaeU, 0x22U, 0x23U,
        0xb0U, 0x03U, 0x61U, 0xa3U, 0x96U, 0x17U, 0x7aU, 0x9cU,
        0xb4U, 0x10U, 0xffU, 0x61U, 0xf2U, 0x00U, 0x15U, 0xadU
    };
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

    CHECK(package_state_sha256((const uint8_t *)"abc", 3U, digest) ==
        PACKAGE_STATE_STATUS_OK, 1);
    CHECK(same_bytes(digest, expected, sizeof(expected)), 2);
    return 0;
}

static int test_parsers_and_mutations(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    /* Independently emitted by sapote-transaction.py for this exact state. */
    static const uint8_t canonical_database_sha256[PACKAGE_STATE_SHA256_BYTES] = {
        0x1cU, 0x6cU, 0xc5U, 0x0cU, 0xacU, 0xd1U, 0x34U, 0xa4U,
        0xd1U, 0x4aU, 0xfdU, 0x66U, 0xcfU, 0x05U, 0x37U, 0x40U,
        0x79U, 0xd3U, 0x92U, 0xaaU, 0x28U, 0x40U, 0xe4U, 0x6aU,
        0x68U, 0x32U, 0x12U, 0x5dU, 0xafU, 0x60U, 0x1dU, 0x33U
    };
    struct package_state_database_view database_view;
    struct package_state_authority_view authority_view;
    struct package_state_journal_view journal_view;
    uint8_t changed[NEW_DATABASE_BYTES];
    uint8_t authority_changed[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t journal_changed[PACKAGE_STATE_JOURNAL_BYTES];
    uint8_t digest[PACKAGE_STATE_SHA256_BYTES];

    CHECK(package_state_database_parse(old_database, OLD_DATABASE_BYTES,
        &database_view) == PACKAGE_STATE_STATUS_OK &&
        database_view.generation == 1U && database_view.package_count == 0U, 10);
    CHECK(package_state_database_parse(new_database, NEW_DATABASE_BYTES,
        &database_view) == PACKAGE_STATE_STATUS_OK &&
        database_view.generation == 2U && database_view.package_count == 2U &&
        database_view.edge_count == 1U && database_view.file_count == 2U, 11);
    CHECK(package_state_sha256(new_database, NEW_DATABASE_BYTES, digest) ==
        PACKAGE_STATE_STATUS_OK && same_bytes(digest,
            canonical_database_sha256, sizeof(digest)), 25);
    CHECK(package_state_authority_parse(authority, PACKAGE_STATE_AUTHORITY_BYTES,
        &authority_view) == PACKAGE_STATE_STATUS_OK &&
        authority_view.generation == 2U, 12);
    CHECK(package_state_journal_parse(journal, PACKAGE_STATE_JOURNAL_BYTES,
        &journal_view) == PACKAGE_STATE_STATUS_OK &&
        journal_view.operation == PACKAGE_STATE_OPERATION_INSTALL &&
        journal_view.base_generation == 1U &&
        journal_view.target_generation == 2U, 13);

    copy_bytes(changed, new_database, sizeof(changed));
    changed[136U] = 1U;
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_RESERVED, 14);

    copy_bytes(changed, new_database, sizeof(changed));
    changed[PACKAGE_STATE_DATABASE_HEADER_BYTES] = (uint8_t)'O';
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_PACKAGE, 15);

    copy_bytes(changed, new_database, sizeof(changed));
    changed[NEW_DATABASE_BYTES - 1U] = 1U;
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_FILE, 16);

    copy_bytes(changed, new_database, sizeof(changed));
    put_text(changed + NEW_EDGE_OFFSET + 120U, 64U, "org.sapote.missing");
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DEPENDENCY, 17);

    copy_bytes(changed, new_database, sizeof(changed));
    put_text(changed + NEW_EDGE_OFFSET + 120U, 64U, "org.sapote.app");
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DEPENDENCY, 26);

    copy_bytes(changed, new_database, sizeof(changed));
    put_u32(changed + PACKAGE_STATE_DATABASE_HEADER_BYTES + 192U, 0U);
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DEPENDENCY, 27);

    copy_bytes(changed, new_database, sizeof(changed));
    put_text(changed + NEW_EDGE_OFFSET + 64U, 56U, "1.0.0");
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DEPENDENCY, 28);

    copy_bytes(changed, new_database, sizeof(changed));
    put_text(changed + NEW_FILE_OFFSET +
        PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES, 128U, "bin/app");
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_FILE, 18);

    copy_bytes(changed, new_database, sizeof(changed));
    clear_bytes(changed + NEW_FILE_OFFSET +
        PACKAGE_STATE_DATABASE_FILE_RECORD_BYTES + 184U, 64U);
    finalize_database(changed, sizeof(changed));
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_FILE, 29);

    copy_bytes(changed, new_database, sizeof(changed));
    changed[NEW_DATABASE_BYTES - 9U] ^= 1U;
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_DIGEST, 19);

    copy_bytes(changed, new_database, sizeof(changed));
    put_u32(changed + 64U, PACKAGE_STATE_DATABASE_MAX_PACKAGES + 1U);
    CHECK(package_state_database_parse(changed, sizeof(changed), &database_view) ==
        PACKAGE_STATE_STATUS_TABLE, 20);

    copy_bytes(authority_changed, authority, sizeof(authority_changed));
    authority_changed[64U] ^= 1U;
    CHECK(package_state_authority_parse(authority_changed,
        sizeof(authority_changed), &authority_view) ==
        PACKAGE_STATE_STATUS_DIGEST, 21);

    copy_bytes(journal_changed, journal, sizeof(journal_changed));
    journal_changed[224U] = 1U;
    CHECK(package_state_journal_parse(journal_changed,
        sizeof(journal_changed), &journal_view) ==
        PACKAGE_STATE_STATUS_RESERVED, 22);

    copy_bytes(journal_changed, journal, sizeof(journal_changed));
    journal_changed[128U] ^= 1U;
    CHECK(package_state_journal_parse(journal_changed,
        sizeof(journal_changed), &journal_view) ==
        PACKAGE_STATE_STATUS_DIGEST, 23);

    copy_bytes(journal_changed, journal, sizeof(journal_changed));
    put_u64(journal_changed + 24U, UINT64_MAX);
    put_u64(journal_changed + 32U, 0U);
    rehash_journal(journal_changed);
    CHECK(package_state_journal_parse(journal_changed,
        sizeof(journal_changed), &journal_view) ==
        PACKAGE_STATE_STATUS_JOURNAL, 24);
    return 0;
}

static int test_recovery(
    const uint8_t old_database[OLD_DATABASE_BYTES],
    const uint8_t new_database[NEW_DATABASE_BYTES],
    const uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES],
    const uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES]
)
{
    struct package_state_generation old_generation = {
        old_database, OLD_DATABASE_BYTES, true
    };
    struct package_state_generation new_generation = {
        new_database, NEW_DATABASE_BYTES, true
    };
    struct package_state_recovery_result result;
    uint8_t changed_authority[PACKAGE_STATE_AUTHORITY_BYTES];

    CHECK(package_state_recovery_decide(old_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_OLD && result.generation == 1U, 30);

    CHECK(package_state_recovery_decide(new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_NEW && result.generation == 2U, 31);

    new_generation.owned_files_complete = false;
    CHECK(package_state_recovery_decide(new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_OLD, 32);

    old_generation.owned_files_complete = false;
    CHECK(package_state_recovery_decide(new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) ==
        PACKAGE_STATE_STATUS_INCOMPLETE, 33);

    old_generation.owned_files_complete = true;
    new_generation.owned_files_complete = true;
    CHECK(package_state_recovery_decide(new_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, NULL, 0U,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_NEW, 34);

    copy_bytes(changed_authority, new_authority, sizeof(changed_authority));
    changed_authority[64U] ^= 1U;
    CHECK(package_state_recovery_decide(changed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) == PACKAGE_STATE_STATUS_OK &&
        result.choice == PACKAGE_STATE_RECOVERY_OLD, 35);

    copy_bytes(changed_authority, new_authority, sizeof(changed_authority));
    put_u64(changed_authority + 16U, 99U);
    rehash_authority(changed_authority);
    CHECK(package_state_recovery_decide(changed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) ==
        PACKAGE_STATE_STATUS_MISMATCH, 36);

    copy_bytes(changed_authority, old_authority, sizeof(changed_authority));
    changed_authority[32U] ^= 1U;
    rehash_authority(changed_authority);
    CHECK(package_state_recovery_decide(changed_authority,
        PACKAGE_STATE_AUTHORITY_BYTES, journal, PACKAGE_STATE_JOURNAL_BYTES,
        &old_generation, &new_generation, &result) ==
        PACKAGE_STATE_STATUS_MISMATCH, 37);
    return 0;
}

int main(void)
{
    uint8_t old_database[OLD_DATABASE_BYTES];
    uint8_t new_database[NEW_DATABASE_BYTES];
    uint8_t old_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t new_authority[PACKAGE_STATE_AUTHORITY_BYTES];
    uint8_t journal[PACKAGE_STATE_JOURNAL_BYTES];
    int result;

    result = test_sha256();
    if (result != 0) {
        return result;
    }
    build_old_database(old_database);
    build_new_database(new_database);
    build_authority(old_authority, old_database, OLD_DATABASE_BYTES);
    build_authority(new_authority, new_database, NEW_DATABASE_BYTES);
    build_journal(journal, old_database, new_database);
    result = test_parsers_and_mutations(
        old_database, new_database, new_authority, journal);
    if (result != 0) {
        return result;
    }
    return test_recovery(old_database, new_database,
        old_authority, new_authority, journal);
}
