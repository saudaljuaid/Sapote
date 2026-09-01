/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/package_fetch.h>

#include <bearssl.h>
#include <limits.h>
#include <sapote/abi.h>
#include <sapote/runtime.h>
#include <string.h>

struct fetch_sink {
    sapote_handle_t handle;
    br_sha256_context sha256;
    long error;
};

static bool equal_bytes(const uint8_t *left, const uint8_t *right, size_t count)
{
    uint8_t difference = 0U;
    for (size_t index = 0U; index < count; ++index) {
        difference |= left[index] ^ right[index];
    }
    return difference == 0U;
}

static bool path_valid(const char *path)
{
    size_t length;
    if (path == NULL) {
        return false;
    }
    length = strlen(path);
    return length != 0U && length <= SAPOTE_PATH_MAX;
}

static long write_body(void *context, const void *bytes, size_t byte_count)
{
    struct fetch_sink *sink = context;
    const uint8_t *source = bytes;
    size_t written = 0U;
    if (sink == NULL || bytes == NULL || byte_count > LONG_MAX) {
        return -1;
    }
    while (written < byte_count) {
        long count = sapote_file_write(sink->handle, source + written,
            byte_count - written);
        if (count <= 0 || (size_t)count > byte_count - written) {
            sink->error = count <= 0 ? count : -(long)SAPOTE_EIO;
            return -1;
        }
        br_sha256_update(&sink->sha256, source + written, (size_t)count);
        written += (size_t)count;
    }
    return (long)written;
}

static void cleanup_temporary(
    const struct sapote_package_fetch_request *request,
    struct sapote_package_fetch_report *report
)
{
    long status = sapote_path_unlink(SAPOTE_VOLUME_DATA,
        request->temporary_path);
    if (status < 0 && status != -(long)SAPOTE_ENOENT) {
        report->cleanup_error = status;
        return;
    }
    status = sapote_volume_sync(SAPOTE_VOLUME_DATA);
    if (status < 0) {
        report->cleanup_error = status;
    }
}

enum sapote_package_fetch_status sapote_package_fetch_stage(
    const struct sapote_package_fetch_request *request,
    struct sapote_package_fetch_report *report
)
{
    struct fetch_sink sink;
    struct sapote_https_stream_request stream;
    struct sapote_https_response response;
    long opened;
    long status;
    if (report == NULL) {
        return SAPOTE_PACKAGE_FETCH_ARGUMENT;
    }
    (void)memset(report, 0, sizeof(*report));
    report->https_status = SAPOTE_HTTPS_ARGUMENT;
    if (request == NULL || request->reserved != 0U ||
        !path_valid(request->temporary_path) ||
        !path_valid(request->staged_path) ||
        strcmp(request->temporary_path, request->staged_path) == 0 ||
        request->maximum_bytes == 0U ||
        request->maximum_bytes > SAPOTE_PACKAGE_FETCH_MAX_BYTES ||
        request->expected_bytes > request->maximum_bytes ||
        ((request->expected_bytes == 0U) !=
            (request->expected_sha256 == NULL))) {
        return SAPOTE_PACKAGE_FETCH_ARGUMENT;
    }
    opened = sapote_file_open(SAPOTE_VOLUME_DATA, request->temporary_path,
        SAPOTE_OPEN_WRITE | SAPOTE_OPEN_CREATE | SAPOTE_OPEN_TRUNCATE);
    if (opened < 0) {
        report->storage_error = opened;
        return SAPOTE_PACKAGE_FETCH_OPEN;
    }
    sink.handle = (sapote_handle_t)opened;
    sink.error = 0;
    br_sha256_init(&sink.sha256);
    stream = (struct sapote_https_stream_request){
        request->hostname, request->port, request->reserved, request->path,
        request->trust_anchors, request->trust_anchor_count,
        request->deadline_ns, request->maximum_bytes, write_body, &sink
    };
    report->https_status = sapote_https_get_stream(&stream, &response);
    report->bearssl_error = response.bearssl_error;
    report->transport_error = response.transport_error;
    report->bytes_received = response.body_length;
    if (report->https_status == SAPOTE_HTTPS_BODY_WRITE) {
        report->storage_error = sink.error;
    }
    status = sapote_handle_close(sink.handle);
    if (status < 0 && report->storage_error == 0) {
        report->storage_error = status;
    }
    if (report->https_status != SAPOTE_HTTPS_OK) {
        cleanup_temporary(request, report);
        return report->https_status == SAPOTE_HTTPS_BODY_WRITE ?
            SAPOTE_PACKAGE_FETCH_WRITE : SAPOTE_PACKAGE_FETCH_HTTPS;
    }
    if (status < 0) {
        cleanup_temporary(request, report);
        return SAPOTE_PACKAGE_FETCH_CLOSE;
    }
    br_sha256_out(&sink.sha256, report->sha256);
    if (request->expected_bytes != 0U &&
        report->bytes_received != request->expected_bytes) {
        cleanup_temporary(request, report);
        return SAPOTE_PACKAGE_FETCH_LENGTH;
    }
    if (request->expected_sha256 != NULL &&
        !equal_bytes(report->sha256, request->expected_sha256,
            sizeof(report->sha256))) {
        cleanup_temporary(request, report);
        return SAPOTE_PACKAGE_FETCH_DIGEST;
    }
    status = sapote_volume_sync(SAPOTE_VOLUME_DATA);
    if (status < 0) {
        report->storage_error = status;
        cleanup_temporary(request, report);
        return SAPOTE_PACKAGE_FETCH_SYNC;
    }
    status = sapote_path_replace(SAPOTE_VOLUME_DATA,
        request->temporary_path, request->staged_path);
    if (status < 0) {
        report->storage_error = status;
        cleanup_temporary(request, report);
        return SAPOTE_PACKAGE_FETCH_PUBLISH;
    }
    report->published = true;
    status = sapote_volume_sync(SAPOTE_VOLUME_DATA);
    if (status < 0) {
        report->storage_error = status;
        return SAPOTE_PACKAGE_FETCH_SYNC;
    }
    report->durable = true;
    return SAPOTE_PACKAGE_FETCH_OK;
}

const char *sapote_package_fetch_status_string(
    enum sapote_package_fetch_status status
)
{
    static const char *const names[] = {
        "ok", "invalid package fetch argument", "temporary file open failed",
        "HTTPS fetch failed", "temporary file write failed",
        "temporary file close failed", "download length mismatch",
        "download digest mismatch", "download durability barrier failed",
        "staged-file publish failed"
    };
    return (unsigned)status < sizeof(names) / sizeof(names[0]) ?
        names[status] : "unknown package fetch status";
}
