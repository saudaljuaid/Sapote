/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_USER_PACKAGE_FETCH_H
#define SAPOTE_USER_PACKAGE_FETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sapote/tls.h>

#define SAPOTE_PACKAGE_FETCH_SHA256_BYTES 32U
#define SAPOTE_PACKAGE_FETCH_MAX_BYTES (256U * 1024U * 1024U)

enum sapote_package_fetch_status {
    SAPOTE_PACKAGE_FETCH_OK = 0,
    SAPOTE_PACKAGE_FETCH_ARGUMENT,
    SAPOTE_PACKAGE_FETCH_OPEN,
    SAPOTE_PACKAGE_FETCH_HTTPS,
    SAPOTE_PACKAGE_FETCH_WRITE,
    SAPOTE_PACKAGE_FETCH_CLOSE,
    SAPOTE_PACKAGE_FETCH_LENGTH,
    SAPOTE_PACKAGE_FETCH_DIGEST,
    SAPOTE_PACKAGE_FETCH_SYNC,
    SAPOTE_PACKAGE_FETCH_PUBLISH,
    SAPOTE_PACKAGE_FETCH_COUNT
};

struct sapote_package_fetch_request {
    const char *hostname;
    uint16_t port;
    uint16_t reserved;
    const char *path;
    const br_x509_trust_anchor *trust_anchors;
    size_t trust_anchor_count;
    uint64_t deadline_ns;
    size_t maximum_bytes;
    /* Zero means the signed caller has not supplied an exact length yet. */
    size_t expected_bytes;
    /* NULL only when expected_bytes is zero (for an unparsed index fetch). */
    const uint8_t *expected_sha256;
    const char *temporary_path;
    const char *staged_path;
};

struct sapote_package_fetch_report {
    enum sapote_https_status https_status;
    int bearssl_error;
    long transport_error;
    long storage_error;
    long cleanup_error;
    size_t bytes_received;
    uint8_t sha256[SAPOTE_PACKAGE_FETCH_SHA256_BYTES];
    bool published;
    bool durable;
};

/*
 * Fetch into an inert Data-volume staging path. This does not authenticate a
 * repository/package signature and cannot install or advance package state.
 */
enum sapote_package_fetch_status sapote_package_fetch_stage(
    const struct sapote_package_fetch_request *request,
    struct sapote_package_fetch_report *report
);

const char *sapote_package_fetch_status_string(
    enum sapote_package_fetch_status status
);

#endif
