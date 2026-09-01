/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/package_fetch.h>

#include <sapote/abi.h>
#include <sapote/runtime.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition, code) do { if (!(condition)) return (code); } while (0)

static const uint8_t body[] = "hello from the Sapote HTTPS peer\n";
static const uint8_t body_sha256[32] = {
    0xbeU, 0xb6U, 0x68U, 0xceU, 0xc4U, 0x96U, 0x0aU, 0x4fU,
    0x74U, 0xddU, 0x82U, 0x70U, 0x3aU, 0xe8U, 0x2cU, 0x3dU,
    0x0aU, 0x69U, 0xd4U, 0xecU, 0xc1U, 0x3bU, 0x09U, 0xdeU,
    0xc6U, 0x4fU, 0xc7U, 0xecU, 0x72U, 0x26U, 0x5eU, 0x53U
};

static uint8_t temporary[128];
static uint8_t staged[128];
static size_t temporary_bytes;
static size_t staged_bytes;
static bool temporary_present;
static bool staged_present;
static bool handle_live;
static unsigned write_calls;
static unsigned sync_calls;
static unsigned fail_write_call;
static unsigned fail_sync_call;
static bool fail_close;
static bool fail_replace;
static bool upload_live;
static bool fail_upload_open;
static bool fail_upload_write;
static bool fail_upload_seal;
static bool fail_upload_close;
static uint8_t uploaded[128];
static size_t uploaded_bytes;
static enum sapote_https_status forced_https_status;
static bool partial_https_body;

static void reset_fixture(void)
{
    (void)memset(temporary, 0, sizeof(temporary));
    (void)memset(staged, 0, sizeof(staged));
    temporary_bytes = 0U;
    staged_bytes = 0U;
    temporary_present = false;
    staged_present = false;
    handle_live = false;
    write_calls = 0U;
    sync_calls = 0U;
    fail_write_call = 0U;
    fail_sync_call = 0U;
    fail_close = false;
    fail_replace = false;
    upload_live = false;
    fail_upload_open = false;
    fail_upload_write = false;
    fail_upload_seal = false;
    fail_upload_close = false;
    (void)memset(uploaded, 0, sizeof(uploaded));
    uploaded_bytes = 0U;
    forced_https_status = SAPOTE_HTTPS_OK;
    partial_https_body = false;
}

long sapote_package_upload_open(void)
{
    if (fail_upload_open || upload_live) {
        return -(long)SAPOTE_EIO;
    }
    upload_live = true;
    uploaded_bytes = 0U;
    return 19;
}

long sapote_package_upload_write(sapote_handle_t upload, const void *bytes,
    size_t byte_count)
{
    if (!upload_live || upload != 19U || bytes == NULL || fail_upload_write) {
        return -(long)SAPOTE_EIO;
    }
    if (byte_count > sizeof(uploaded) - uploaded_bytes) {
        return -(long)SAPOTE_ENOSPC;
    }
    (void)memcpy(uploaded + uploaded_bytes, bytes, byte_count);
    uploaded_bytes += byte_count;
    return (long)byte_count;
}

long sapote_package_upload_seal(sapote_handle_t upload,
    uint64_t expected_bytes, const uint8_t expected_sha256[32],
    struct sapote_package_upload_report *report)
{
    if (!upload_live || upload != 19U || expected_sha256 == NULL ||
        report == NULL) {
        return -(long)SAPOTE_EINVAL;
    }
    (void)memset(report, 0, sizeof(*report));
    report->actual_bytes = uploaded_bytes;
    if (uploaded_bytes == sizeof(body) - 1U &&
        memcmp(uploaded, body, uploaded_bytes) == 0) {
        (void)memcpy(report->actual_sha256, body_sha256,
            sizeof(body_sha256));
    }
    if (fail_upload_seal) {
        return -(long)SAPOTE_EIO;
    }
    if (expected_bytes != uploaded_bytes) {
        return -(long)SAPOTE_EINVAL;
    }
    if (memcmp(expected_sha256, report->actual_sha256,
            sizeof(report->actual_sha256)) != 0) {
        return -(long)SAPOTE_EACCES;
    }
    report->result_flags = SAPOTE_PACKAGE_UPLOAD_SEALED |
        SAPOTE_PACKAGE_UPLOAD_DURABLE;
    return 0;
}

long sapote_package_upload_close(sapote_handle_t upload)
{
    if (!upload_live || upload != 19U) {
        return -(long)SAPOTE_ESTALE;
    }
    if (fail_upload_close) {
        return -(long)SAPOTE_EIO;
    }
    upload_live = false;
    uploaded_bytes = 0U;
    return 0;
}

long sapote_file_open(uint16_t volume, const char *path, uint32_t flags)
{
    if (volume != SAPOTE_VOLUME_DATA || strcmp(path, "FETCH.NEW") != 0 ||
        flags != (SAPOTE_OPEN_WRITE | SAPOTE_OPEN_CREATE |
            SAPOTE_OPEN_TRUNCATE) || handle_live) {
        return -(long)SAPOTE_EINVAL;
    }
    temporary_present = true;
    temporary_bytes = 0U;
    handle_live = true;
    return 7;
}

long sapote_file_write(sapote_handle_t handle, const void *bytes, size_t count)
{
    size_t accepted = count > 7U ? 7U : count;
    ++write_calls;
    if (!handle_live || handle != 7U || bytes == NULL ||
        (fail_write_call != 0U && write_calls == fail_write_call)) {
        return -(long)SAPOTE_EIO;
    }
    if (accepted > sizeof(temporary) - temporary_bytes) {
        return -(long)SAPOTE_ENOSPC;
    }
    (void)memcpy(temporary + temporary_bytes, bytes, accepted);
    temporary_bytes += accepted;
    return (long)accepted;
}

long sapote_handle_close(sapote_handle_t handle)
{
    if (!handle_live || handle != 7U) {
        return -(long)SAPOTE_ESTALE;
    }
    handle_live = false;
    return fail_close ? -(long)SAPOTE_EIO : 0;
}

long sapote_path_unlink(uint16_t volume, const char *path)
{
    if (volume != SAPOTE_VOLUME_DATA || strcmp(path, "FETCH.NEW") != 0) {
        return -(long)SAPOTE_EINVAL;
    }
    if (!temporary_present) {
        return -(long)SAPOTE_ENOENT;
    }
    temporary_present = false;
    temporary_bytes = 0U;
    return 0;
}

long sapote_volume_sync(uint16_t volume)
{
    ++sync_calls;
    if (volume != SAPOTE_VOLUME_DATA ||
        (fail_sync_call != 0U && sync_calls == fail_sync_call)) {
        return -(long)SAPOTE_EIO;
    }
    return 0;
}

long sapote_path_replace(uint16_t volume, const char *source,
    const char *destination)
{
    if (volume != SAPOTE_VOLUME_DATA || strcmp(source, "FETCH.NEW") != 0 ||
        strcmp(destination, "FETCH.BIN") != 0 || !temporary_present ||
        fail_replace) {
        return -(long)SAPOTE_EIO;
    }
    (void)memcpy(staged, temporary, temporary_bytes);
    staged_bytes = temporary_bytes;
    staged_present = true;
    temporary_present = false;
    temporary_bytes = 0U;
    return 0;
}

enum sapote_https_status sapote_https_get_stream(
    const struct sapote_https_stream_request *request,
    struct sapote_https_response *response)
{
    size_t first = partial_https_body ? 5U : 17U;
    long count;
    if (request == NULL || response == NULL || request->write_body == NULL ||
        request->body_limit < sizeof(body) - 1U) {
        return SAPOTE_HTTPS_ARGUMENT;
    }
    (void)memset(response, 0, sizeof(*response));
    response->status_code = 200U;
    response->content_length = sizeof(body) - 1U;
    count = request->write_body(request->write_context, body, first);
    if (count != (long)first) {
        return SAPOTE_HTTPS_BODY_WRITE;
    }
    response->body_length = first;
    if (forced_https_status != SAPOTE_HTTPS_OK) {
        return forced_https_status;
    }
    count = request->write_body(request->write_context, body + first,
        sizeof(body) - 1U - first);
    if (count != (long)(sizeof(body) - 1U - first)) {
        return SAPOTE_HTTPS_BODY_WRITE;
    }
    response->body_length = sizeof(body) - 1U;
    return SAPOTE_HTTPS_OK;
}

static struct sapote_package_fetch_request request(bool exact)
{
    static const br_x509_trust_anchor anchor = {0};
    const struct sapote_package_fetch_request result = {
        "repo.sapote.test", 443U, 0U, "/index.sri", &anchor, 1U,
        UINT64_MAX, 128U, exact ? sizeof(body) - 1U : 0U,
        exact ? body_sha256 : NULL, "FETCH.NEW", "FETCH.BIN"
    };
    return result;
}

static struct sapote_package_fetch_upload_request upload_request(void)
{
    static const br_x509_trust_anchor anchor = {0};
    const struct sapote_package_fetch_upload_request result = {
        "repo.sapote.test", 443U, 0U, "/package.spk", &anchor, 1U,
        UINT64_MAX, sizeof(body) - 1U, body_sha256
    };

    return result;
}

static int run_tests(void)
{
    struct sapote_package_fetch_request input;
    struct sapote_package_fetch_report report;
    uint8_t wrong_digest[32] = {0U};

    reset_fixture();
    input = request(true);
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_OK && report.published && report.durable &&
        report.bytes_received == sizeof(body) - 1U &&
        memcmp(report.sha256, body_sha256, sizeof(body_sha256)) == 0 &&
        staged_present && staged_bytes == sizeof(body) - 1U &&
        memcmp(staged, body, staged_bytes) == 0 && !temporary_present &&
        !handle_live && sync_calls == 2U && write_calls > 2U, 1);

    reset_fixture();
    input = request(false);
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_OK && report.durable && staged_present, 2);

    reset_fixture();
    input = request(true);
    input.expected_sha256 = wrong_digest;
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_DIGEST && !temporary_present &&
        !staged_present && !handle_live && sync_calls == 1U, 3);

    reset_fixture();
    input = request(true);
    partial_https_body = true;
    forced_https_status = SAPOTE_HTTPS_BODY_TRUNCATED;
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_HTTPS &&
        report.https_status == SAPOTE_HTTPS_BODY_TRUNCATED &&
        report.bytes_received == 5U && !temporary_present && !handle_live, 4);

    reset_fixture();
    input = request(true);
    fail_write_call = 2U;
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_WRITE &&
        report.https_status == SAPOTE_HTTPS_BODY_WRITE &&
        report.storage_error == -(long)SAPOTE_EIO && !temporary_present &&
        !handle_live, 5);

    reset_fixture();
    input = request(true);
    fail_close = true;
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_CLOSE && !temporary_present && !handle_live, 6);

    reset_fixture();
    input = request(true);
    fail_sync_call = 1U;
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_SYNC && !temporary_present && !staged_present &&
        sync_calls == 2U, 7);

    reset_fixture();
    input = request(true);
    fail_replace = true;
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_PUBLISH && !temporary_present &&
        !staged_present && sync_calls == 2U, 8);

    reset_fixture();
    input = request(true);
    fail_sync_call = 2U;
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_SYNC && report.published && !report.durable &&
        staged_present && !temporary_present && sync_calls == 2U, 9);

    reset_fixture();
    input = request(true);
    input.temporary_path = input.staged_path;
    CHECK(sapote_package_fetch_stage(&input, &report) ==
            SAPOTE_PACKAGE_FETCH_ARGUMENT && !temporary_present &&
        !staged_present && !handle_live, 10);

    struct sapote_package_fetch_upload_request upload = upload_request();

    reset_fixture();
    CHECK(sapote_package_fetch_upload(&upload, &report) ==
            SAPOTE_PACKAGE_FETCH_OK && report.durable && !report.published &&
        report.upload == 19U && upload_live &&
        report.bytes_received == sizeof(body) - 1U &&
        memcmp(report.sha256, body_sha256, sizeof(body_sha256)) == 0 &&
        report.upload_flags == (SAPOTE_PACKAGE_UPLOAD_SEALED |
            SAPOTE_PACKAGE_UPLOAD_DURABLE), 11);
    CHECK(sapote_package_upload_close(report.upload) == 0 && !upload_live, 12);

    reset_fixture();
    upload = upload_request();
    upload.expected_sha256 = wrong_digest;
    CHECK(sapote_package_fetch_upload(&upload, &report) ==
            SAPOTE_PACKAGE_FETCH_DIGEST && !upload_live &&
        report.upload == SAPOTE_HANDLE_INVALID, 13);

    reset_fixture();
    upload = upload_request();
    partial_https_body = true;
    forced_https_status = SAPOTE_HTTPS_BODY_TRUNCATED;
    CHECK(sapote_package_fetch_upload(&upload, &report) ==
            SAPOTE_PACKAGE_FETCH_HTTPS && !upload_live &&
        report.upload == SAPOTE_HANDLE_INVALID, 14);

    reset_fixture();
    upload = upload_request();
    fail_upload_write = true;
    CHECK(sapote_package_fetch_upload(&upload, &report) ==
            SAPOTE_PACKAGE_FETCH_WRITE && !upload_live &&
        report.storage_error == -(long)SAPOTE_EIO, 15);

    reset_fixture();
    upload = upload_request();
    fail_upload_open = true;
    CHECK(sapote_package_fetch_upload(&upload, &report) ==
            SAPOTE_PACKAGE_FETCH_UPLOAD_OPEN && !upload_live, 16);

    reset_fixture();
    upload = upload_request();
    fail_upload_seal = true;
    CHECK(sapote_package_fetch_upload(&upload, &report) ==
            SAPOTE_PACKAGE_FETCH_UPLOAD_SEAL && !upload_live &&
        report.storage_error == -(long)SAPOTE_EIO, 17);

    reset_fixture();
    upload = upload_request();
    upload.expected_bytes = 0U;
    CHECK(sapote_package_fetch_upload(&upload, &report) ==
            SAPOTE_PACKAGE_FETCH_ARGUMENT && !upload_live, 18);

    reset_fixture();
    upload = upload_request();
    forced_https_status = SAPOTE_HTTPS_BODY_TRUNCATED;
    partial_https_body = true;
    fail_upload_close = true;
    CHECK(sapote_package_fetch_upload(&upload, &report) ==
            SAPOTE_PACKAGE_FETCH_HTTPS && upload_live && report.upload == 19U &&
        report.cleanup_error == -(long)SAPOTE_EIO, 19);
    fail_upload_close = false;
    CHECK(sapote_package_upload_close(report.upload) == 0 && !upload_live, 20);
    return 0;
}

int main(void)
{
    int status = run_tests();
    if (status != 0) {
        (void)fprintf(stderr, "package fetch host test failed: %d\n", status);
        return status;
    }
    (void)puts("Sapote durable streaming package-fetch tests passed: "
        "path and kernel-owned upload modes");
    return 0;
}
