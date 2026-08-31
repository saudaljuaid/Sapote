/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/runtime.h>
#include <sapote/tls.h>

#include <stdio.h>
#include <string.h>

#include "trust_anchor.h"

#define HTTPS_PORT 443U
#define HTTPS_DEADLINE_NS UINT64_C(15000000000)

static const unsigned char expected_body[] =
    "hello from the Sapote HTTPS peer\n";

static uint64_t deadline(void)
{
    const uint64_t now = sapote_monotonic_ns();

    return now > UINT64_MAX - HTTPS_DEADLINE_NS ? UINT64_MAX :
        now + HTTPS_DEADLINE_NS;
}

int main(void)
{
    unsigned char body[128];
    struct sapote_https_response response;
    const struct sapote_https_request request = {
        "repo.sapote.test", HTTPS_PORT, 0U, "/artifact.bin",
        sapote_https_test_anchors,
        sizeof(sapote_https_test_anchors) /
            sizeof(sapote_https_test_anchors[0]),
        deadline(), body, sizeof(body)
    };
    enum sapote_https_status status;
    FILE *output;

    puts("SAPOTE HTTPSAPP PHASE start");
    status = sapote_https_get(&request, &response);
    if (status != SAPOTE_HTTPS_OK) {
        printf("SAPOTE HTTPSAPP REFUSED %s tls=%d transport=%ld\n",
            sapote_https_status_string(status), response.bearssl_error,
            response.transport_error);
        return 20;
    }
    if (response.status_code != 200U ||
        response.body_length != sizeof(expected_body) - 1U ||
        memcmp(body, expected_body, sizeof(expected_body) - 1U) != 0) {
        return 21;
    }
    puts("SAPOTE HTTPSAPP PHASE authenticated-download PASS");
    output = fopen("HTTPS.TXT", "w");
    if (output == NULL ||
        fwrite(body, 1U, response.body_length, output) !=
            response.body_length ||
        fflush(output) != 0 || fclose(output) != 0 ||
        sapote_volume_sync(SAPOTE_VOLUME_DATA) < 0) {
        return 22;
    }
    puts("SAPOTE HTTPSAPP PHASE durable-output PASS");
    puts("SAPOTE HTTPSAPP PASS hostname time trust length close");
    return 0;
}
