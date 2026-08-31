/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef SAPOTE_USER_TLS_H
#define SAPOTE_USER_TLS_H

#include <bearssl.h>
#include <stddef.h>
#include <stdint.h>

struct sapote_tls_client;

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

enum sapote_tls_status sapote_tls_client_open(
    const struct sapote_tls_client_config *config,
    struct sapote_tls_client **result);
long sapote_tls_client_read(struct sapote_tls_client *client, void *buffer,
    size_t length, uint64_t deadline_ns);
long sapote_tls_client_write(struct sapote_tls_client *client,
    const void *buffer, size_t length, uint64_t deadline_ns);
enum sapote_tls_status sapote_tls_client_flush(
    struct sapote_tls_client *client, uint64_t deadline_ns);
enum sapote_tls_status sapote_tls_client_close(
    struct sapote_tls_client *client, uint64_t deadline_ns);
enum sapote_tls_status sapote_tls_client_status(
    const struct sapote_tls_client *client);
int sapote_tls_client_bearssl_error(const struct sapote_tls_client *client);
long sapote_tls_client_transport_error(const struct sapote_tls_client *client);
const char *sapote_tls_status_string(enum sapote_tls_status status);

#endif
