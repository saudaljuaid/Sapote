/* SPDX-License-Identifier: GPL-3.0-only */
/* Bounded signed-repository install/update client for the native ABI. */

#include <sapote/package_control.h>
#include <sapote/package_fetch.h>
#include <sapote/package_upload.h>
#include <sapote/runtime.h>

#include <stdio.h>
#include <string.h>

#include "../native-https/trust_anchor.h"

#define REPOSITORY_HOST "repo.sapote.test"
#define REPOSITORY_PATH "/repository.sri"
#define HTTPS_PORT 443U
#define HTTPS_DEADLINE_NS UINT64_C(30000000000)
#define REPOSITORY_MAX_BYTES (512U * 1024U)
#define COPY_BYTES 4096U

static uint64_t deadline(void)
{
    const uint64_t now = sapote_monotonic_ns();

    return now > UINT64_MAX - HTTPS_DEADLINE_NS ? UINT64_MAX :
        now + HTTPS_DEADLINE_NS;
}

static bool close_handle(sapote_handle_t handle)
{
    return handle == SAPOTE_HANDLE_INVALID || sapote_handle_close(handle) == 0;
}

static bool discard_repository(void)
{
    long unlinked = sapote_path_unlink(SAPOTE_VOLUME_DATA, "REPO.SRI");
    long synced = sapote_volume_sync(SAPOTE_VOLUME_DATA);

    return unlinked == 0 && synced == 0;
}

static long repository_upload(struct sapote_package_fetch_report *fetch)
{
    const struct sapote_package_fetch_request request = {
        REPOSITORY_HOST, HTTPS_PORT, 0U, REPOSITORY_PATH,
        sapote_https_test_anchors,
        sizeof(sapote_https_test_anchors) /
            sizeof(sapote_https_test_anchors[0]),
        deadline(), REPOSITORY_MAX_BYTES, 0U, NULL,
        "REPO.NEW", "REPO.SRI"
    };
    uint8_t buffer[COPY_BYTES];
    sapote_handle_t file = SAPOTE_HANDLE_INVALID;
    sapote_handle_t upload = SAPOTE_HANDLE_INVALID;
    size_t total = 0U;

    enum sapote_package_fetch_status status = sapote_package_fetch_stage(
        &request, fetch);
    if (status != SAPOTE_PACKAGE_FETCH_OK || fetch->bytes_received == 0U ||
        fetch->bytes_received > REPOSITORY_MAX_BYTES || !fetch->durable) {
        return -SAPOTE_EIO;
    }
    long opened = sapote_file_open(SAPOTE_VOLUME_DATA, "REPO.SRI",
        SAPOTE_OPEN_READ);
    if (opened < 0) {
        return opened;
    }
    file = (sapote_handle_t)opened;
    opened = sapote_package_upload_open();
    if (opened < 0) {
        (void)close_handle(file);
        return opened;
    }
    upload = (sapote_handle_t)opened;
    while (total < fetch->bytes_received) {
        size_t wanted = fetch->bytes_received - total;
        if (wanted > sizeof(buffer)) {
            wanted = sizeof(buffer);
        }
        long read_bytes = sapote_file_read(file, buffer, wanted);
        if (read_bytes <= 0 || (size_t)read_bytes > wanted) {
            (void)close_handle(file);
            (void)close_handle(upload);
            return read_bytes < 0 ? read_bytes : -SAPOTE_EIO;
        }
        long written = sapote_package_upload_write(upload, buffer,
            (size_t)read_bytes);
        if (written != read_bytes) {
            (void)close_handle(file);
            (void)close_handle(upload);
            return written < 0 ? written : -SAPOTE_EIO;
        }
        total += (size_t)read_bytes;
    }
    if (!close_handle(file)) {
        (void)close_handle(upload);
        return -SAPOTE_EIO;
    }
    struct sapote_package_upload_report sealed;
    long result = sapote_package_upload_seal(upload, fetch->bytes_received,
        fetch->sha256, &sealed);
    if (result < 0 || sealed.actual_bytes != fetch->bytes_received ||
        sealed.result_flags != (SAPOTE_PACKAGE_UPLOAD_SEALED |
            SAPOTE_PACKAGE_UPLOAD_DURABLE)) {
        (void)close_handle(upload);
        return result < 0 ? result : -SAPOTE_EIO;
    }
    return (long)upload;
}

static int install(const char *identifier)
{
    struct sapote_package_fetch_report fetch;
    struct sapote_package_control_report control_report;
    struct sapote_package_control_item item;
    sapote_handle_t repository = SAPOTE_HANDLE_INVALID;
    sapote_handle_t control = SAPOTE_HANDLE_INVALID;

    long result = repository_upload(&fetch);
    if (result < 0) {
        printf("sap: repository download failed: %ld\n", result);
        return 20;
    }
    repository = (sapote_handle_t)result;
    result = sapote_package_control_open_install(repository, identifier,
        strlen(identifier), &control_report);
    bool repository_closed = close_handle(repository);
    bool repository_discarded = discard_repository();
    if (!repository_closed || !repository_discarded) {
        if (result >= 0) {
            (void)close_handle((sapote_handle_t)result);
        }
        return 22;
    }
    if (result == -(long)SAPOTE_EEXIST) {
        puts("SAPOTE SAP PHASE already-installed PASS");
        return 0;
    }
    if (result < 0) {
        printf("sap: signed repository or plan refused: %ld\n", result);
        return 21;
    }
    control = (sapote_handle_t)result;
    puts("SAPOTE SAP PHASE signed-plan PASS");

    for (uint32_t index = 0U; index < control_report.plan_count; ++index) {
        char path[SAPOTE_PACKAGE_CONTROL_PATH_BYTES + 2U];
        if (sapote_package_control_item(control, index, &item) < 0 ||
            item.path_bytes == 0U ||
            item.path_bytes >= SAPOTE_PACKAGE_CONTROL_PATH_BYTES) {
            (void)close_handle(control);
            return 23;
        }
        path[0] = '/';
        (void)memcpy(path + 1U, item.download_path, item.path_bytes);
        path[item.path_bytes + 1U] = '\0';
        const struct sapote_package_fetch_upload_request request = {
            REPOSITORY_HOST, HTTPS_PORT, 0U, path,
            sapote_https_test_anchors,
            sizeof(sapote_https_test_anchors) /
                sizeof(sapote_https_test_anchors[0]),
            deadline(), (size_t)item.package_bytes, item.package_sha256
        };
        enum sapote_package_fetch_status status = sapote_package_fetch_upload(
            &request, &fetch);
        if (status != SAPOTE_PACKAGE_FETCH_OK ||
            fetch.upload == SAPOTE_HANDLE_INVALID || !fetch.durable) {
            printf("sap: payload %u download failed: %s\n", index,
                sapote_package_fetch_status_string(status));
            if (fetch.upload != SAPOTE_HANDLE_INVALID) {
                (void)close_handle(fetch.upload);
            }
            (void)close_handle(control);
            return 24;
        }
        result = sapote_package_control_attach(control, index, fetch.upload,
            &control_report);
        if (!close_handle(fetch.upload) || result < 0) {
            printf("sap: payload %u refused: %ld\n", index, result);
            (void)close_handle(control);
            return 25;
        }
    }
    puts("SAPOTE SAP PHASE payloads-authenticated PASS");
    result = sapote_package_control_commit(control, &control_report);
    if (result < 0 &&
        (control_report.result_flags & SAPOTE_PACKAGE_CONTROL_PREPARED) != 0U) {
        result = sapote_package_control_commit(control, &control_report);
    }
    if (result < 0 ||
        (control_report.result_flags & SAPOTE_PACKAGE_CONTROL_COMMITTED) == 0U ||
        control_report.generation == 0U || !close_handle(control)) {
        printf("sap: transaction commit failed: %ld flags=%u\n", result,
            control_report.result_flags);
        return 26;
    }
    printf("SAPOTE SAP PHASE committed generation=%llu PASS\n",
        (unsigned long long)control_report.generation);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "install") != 0) {
        puts("usage: sap install IDENTIFIER");
        return 2;
    }
    puts("SAPOTE SAP PHASE start");
    int result = install(argv[2]);
    if (result == 0) {
        puts("SAPOTE SAP PASS https trust plan payload transaction cleanup");
    }
    return result;
}
