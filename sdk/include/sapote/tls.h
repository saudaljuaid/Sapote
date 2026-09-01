/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_USER_TLS_H
#define SAPOTE_USER_TLS_H

/* BearSSL 0.6 deliberately leaves BR_DOXYGEN_IGNORE undefined while using it
 * in two #if expressions.  Isolate that exact upstream -Wundef diagnostic so
 * Sapote applications can include this public header under -Werror without
 * changing BearSSL's many #ifndef BR_DOXYGEN_IGNORE declarations. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include <bearssl.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include <stddef.h>
#include <stdint.h>

struct sapote_tls_client;

#define SAPOTE_HTTPS_MAX_HEADER_BYTES 4096U
#define SAPOTE_HTTPS_MAX_PATH_BYTES 1024U

enum sapote_tls_status {
    SAPOTE_TLS_OK = 0,
    SAPOTE_TLS_ARGUMENT,
    SAPOTE_TLS_NO_MEMORY,
    SAPOTE_TLS_TRUST,
    SAPOTE_TLS_CLOCK,
    SAPOTE_TLS_ENTROPY,
    SAPOTE_TLS_DNS,
    SAPOTE_TLS_TRANSPORT,
    SAPOTE_TLS_HANDSHAKE,
    SAPOTE_TLS_IO,
    SAPOTE_TLS_CLOSE
};

struct sapote_tls_client_config {
    const char *hostname;
    uint16_t port;
    uint16_t reserved;
    const br_x509_trust_anchor *trust_anchors;
    size_t trust_anchor_count;
    uint64_t deadline_ns;
};

/* Diagnostics are output-only.  They make a failed open auditable even though
 * no client object is returned to the caller. */
struct sapote_tls_diagnostics {
    int bearssl_error;
    long transport_error;
};

enum sapote_https_status {
    SAPOTE_HTTPS_OK = 0,
    SAPOTE_HTTPS_ARGUMENT,
    SAPOTE_HTTPS_NO_MEMORY,
    SAPOTE_HTTPS_TRUST,
    SAPOTE_HTTPS_CLOCK,
    SAPOTE_HTTPS_ENTROPY,
    SAPOTE_HTTPS_DNS,
    SAPOTE_HTTPS_TRANSPORT,
    SAPOTE_HTTPS_TIMEOUT,
    SAPOTE_HTTPS_CANCELED,
    SAPOTE_HTTPS_RESET,
    SAPOTE_HTTPS_TRUNCATED,
    SAPOTE_HTTPS_HOSTNAME,
    SAPOTE_HTTPS_CERTIFICATE_TIME,
    SAPOTE_HTTPS_AUTHENTICATION,
    SAPOTE_HTTPS_HANDSHAKE,
    SAPOTE_HTTPS_IO,
    SAPOTE_HTTPS_HTTP_VERSION,
    SAPOTE_HTTPS_HTTP_STATUS,
    SAPOTE_HTTPS_HTTP_HEADERS,
    SAPOTE_HTTPS_CONTENT_LENGTH_REQUIRED,
    SAPOTE_HTTPS_CONTENT_TOO_LARGE,
    SAPOTE_HTTPS_BODY_TRUNCATED,
    SAPOTE_HTTPS_BODY_EXTRA,
    SAPOTE_HTTPS_CLOSE,
    SAPOTE_HTTPS_BODY_WRITE
};

typedef long (*sapote_https_body_write_function)(
    void *context,
    const void *bytes,
    size_t byte_count
);

struct sapote_https_request {
    const char *hostname;
    uint16_t port;
    uint16_t reserved;
    const char *path;
    const br_x509_trust_anchor *trust_anchors;
    size_t trust_anchor_count;
    uint64_t deadline_ns;
    void *body;
    size_t body_capacity;
};

struct sapote_https_response {
    uint16_t status_code;
    uint16_t reserved;
    size_t content_length;
    size_t body_length;
    int bearssl_error;
    long transport_error;
};

struct sapote_https_stream_request {
    const char *hostname;
    uint16_t port;
    uint16_t reserved;
    const char *path;
    const br_x509_trust_anchor *trust_anchors;
    size_t trust_anchor_count;
    uint64_t deadline_ns;
    size_t body_limit;
    sapote_https_body_write_function write_body;
    void *write_context;
};

enum sapote_tls_status sapote_tls_client_open(
    const struct sapote_tls_client_config *config,
    struct sapote_tls_client **result);
enum sapote_tls_status sapote_tls_client_open_diagnostic(
    const struct sapote_tls_client_config *config,
    struct sapote_tls_diagnostics *diagnostics,
    struct sapote_tls_client **result);
long sapote_tls_client_read(struct sapote_tls_client *client, void *buffer,
    size_t length, uint64_t deadline_ns);
long sapote_tls_client_write(struct sapote_tls_client *client,
    const void *buffer, size_t length, uint64_t deadline_ns);
enum sapote_tls_status sapote_tls_client_flush(
    struct sapote_tls_client *client, uint64_t deadline_ns);
long sapote_tls_client_cancel(struct sapote_tls_client *client);
enum sapote_tls_status sapote_tls_client_close(
    struct sapote_tls_client *client, uint64_t deadline_ns);
enum sapote_tls_status sapote_tls_client_status(
    const struct sapote_tls_client *client);
int sapote_tls_client_bearssl_error(const struct sapote_tls_client *client);
long sapote_tls_client_transport_error(const struct sapote_tls_client *client);
const char *sapote_tls_status_string(enum sapote_tls_status status);
enum sapote_https_status sapote_https_get(
    const struct sapote_https_request *request,
    struct sapote_https_response *response);
enum sapote_https_status sapote_https_get_stream(
    const struct sapote_https_stream_request *request,
    struct sapote_https_response *response);
const char *sapote_https_status_string(enum sapote_https_status status);

#endif
