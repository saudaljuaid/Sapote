/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/package_upload.h>

#include <sapote/runtime.h>
#include <string.h>

long sapote_package_upload_open(void)
{
    return sapote_syscall0(SAPOTE_SYS_PACKAGE_UPLOAD_OPEN);
}

long sapote_package_upload_write(
    sapote_handle_t upload,
    const void *bytes,
    size_t byte_count
)
{
    const struct sapote_package_upload_write_request request = {
        sizeof(request), SAPOTE_ABI_VERSION, upload,
        (uint64_t)(uintptr_t)bytes, (uint32_t)byte_count, 0U
    };

    if ((bytes == NULL && byte_count != 0U) ||
        byte_count > SAPOTE_PACKAGE_UPLOAD_WRITE_MAX) {
        return -SAPOTE_EINVAL;
    }
    return sapote_syscall1(SAPOTE_SYS_PACKAGE_UPLOAD_WRITE,
        (uint64_t)(uintptr_t)&request);
}

long sapote_package_upload_seal(
    sapote_handle_t upload,
    uint64_t expected_bytes,
    const uint8_t expected_sha256[SAPOTE_PACKAGE_UPLOAD_SHA256_BYTES],
    struct sapote_package_upload_report *report
)
{
    struct sapote_package_upload_seal_request request;

    if (expected_sha256 == NULL || report == NULL || expected_bytes == 0U ||
        expected_bytes > SAPOTE_PACKAGE_UPLOAD_MAX_BYTES) {
        return -SAPOTE_EINVAL;
    }
    (void)memset(&request, 0, sizeof(request));
    request.size = sizeof(request);
    request.version = SAPOTE_ABI_VERSION;
    request.handle = upload;
    request.expected_bytes = expected_bytes;
    (void)memcpy(request.expected_sha256, expected_sha256,
        sizeof(request.expected_sha256));
    long status = sapote_syscall1(SAPOTE_SYS_PACKAGE_UPLOAD_SEAL,
        (uint64_t)(uintptr_t)&request);

    report->actual_bytes = request.actual_bytes;
    (void)memcpy(report->actual_sha256, request.actual_sha256,
        sizeof(report->actual_sha256));
    report->result_flags = request.result_flags;
    return status;
}

long sapote_package_upload_close(sapote_handle_t upload)
{
    return sapote_handle_close(upload);
}
