/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/package_fetch.h>
#include <sapote/runtime.h>

#include <stdio.h>

#include "trust_anchor.h"

#define HTTPS_PORT 443U
#define HTTPS_DEADLINE_NS UINT64_C(15000000000)
#define EXPECTED_BODY_BYTES 33U

static const uint8_t expected_sha256[32] = {
    0xbeU, 0xb6U, 0x68U, 0xceU, 0xc4U, 0x96U, 0x0aU, 0x4fU,
    0x74U, 0xddU, 0x82U, 0x70U, 0x3aU, 0xe8U, 0x2cU, 0x3dU,
    0x0aU, 0x69U, 0xd4U, 0xecU, 0xc1U, 0x3bU, 0x09U, 0xdeU,
    0xc6U, 0x4fU, 0xc7U, 0xecU, 0x72U, 0x26U, 0x5eU, 0x53U
};

static uint64_t deadline(void)
{
    const uint64_t now = sapote_monotonic_ns();

    return now > UINT64_MAX - HTTPS_DEADLINE_NS ? UINT64_MAX :
        now + HTTPS_DEADLINE_NS;
}

int main(void)
{
    struct sapote_package_fetch_report report;
    const struct sapote_package_fetch_request request = {
        "repo.sapote.test", HTTPS_PORT, 0U, "/artifact.bin",
        sapote_https_test_anchors,
        sizeof(sapote_https_test_anchors) /
            sizeof(sapote_https_test_anchors[0]),
        deadline(), 128U, EXPECTED_BODY_BYTES, expected_sha256,
        "HTTPS.NEW", "HTTPS.TXT"
    };
    enum sapote_package_fetch_status status;

    puts("SAPOTE HTTPSAPP PHASE start");
    status = sapote_package_fetch_stage(&request, &report);
    if (status != SAPOTE_PACKAGE_FETCH_OK) {
        printf("SAPOTE HTTPSAPP REFUSED %s https=%s tls=%d transport=%ld "
            "storage=%ld cleanup=%ld\n",
            sapote_package_fetch_status_string(status),
            sapote_https_status_string(report.https_status),
            report.bearssl_error, report.transport_error,
            report.storage_error, report.cleanup_error);
        return 20;
    }
    if (report.bytes_received != EXPECTED_BODY_BYTES ||
        !report.published || !report.durable) {
        return 21;
    }
    puts("SAPOTE HTTPSAPP PHASE authenticated-download PASS");
    puts("SAPOTE HTTPSAPP PHASE durable-output PASS");
    puts("SAPOTE HTTPSAPP PASS hostname time trust length close");
    return 0;
}
