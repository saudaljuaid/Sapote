/* SPDX-License-Identifier: GPL-3.0-only */
#include <sapote/tls.h>

#include <limits.h>
#include <sapote/network.h>
#include <sapote/runtime.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SAPOTE_TLS_MAX_HOSTNAME 253U
#define SAPOTE_TLS_MAX_LABEL 63U
#define SAPOTE_TLS_MAX_TRUST_ANCHORS 16U
#define SAPOTE_TLS_MAX_HANDSHAKE_STEPS 4096U
#define SAPOTE_TLS_UNIX_EPOCH_DAYS UINT64_C(719528)
#define SAPOTE_TLS_ENTROPY_BYTES 32U

struct sapote_tls_client {
    br_ssl_client_context ssl;
    br_x509_minimal_context x509;
    br_sslio_context io;
    unsigned char buffer[BR_SSL_BUFSIZE_BIDI];
    sapote_handle_t stream;
    uint64_t deadline_ns;
    long transport_error;
    enum sapote_tls_status status;
    bool connected;
};

static const uint16_t suites[] = {
    BR_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
    BR_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256
};

static bool hostname_valid(const char *hostname)
{
    size_t length;
    size_t label = 0U;

    if (hostname == NULL) {
        return false;
    }
    length = strlen(hostname);
    if (length == 0U || length > SAPOTE_TLS_MAX_HOSTNAME) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)hostname[index];

        if (value == '.') {
            if (label == 0U || label > SAPOTE_TLS_MAX_LABEL ||
                hostname[index - 1U] == '-') {
                return false;
            }
            label = 0U;
            continue;
        }
        if (!((value >= 'a' && value <= 'z') ||
                (value >= '0' && value <= '9') || value == '-') ||
            (label == 0U && value == '-')) {
            return false;
        }
        ++label;
    }
    return label != 0U && label <= SAPOTE_TLS_MAX_LABEL &&
        hostname[length - 1U] != '-';
}

static bool trust_anchor_valid(const br_x509_trust_anchor *anchor)
{
    if (anchor == NULL || anchor->dn.data == NULL ||
        anchor->dn.len == 0U || anchor->dn.len > 4096U ||
        (anchor->flags & BR_X509_TA_CA) == 0U) {
        return false;
    }
    if (anchor->pkey.key_type == BR_KEYTYPE_RSA) {
        return anchor->pkey.key.rsa.n != NULL &&
            anchor->pkey.key.rsa.nlen >= 256U &&
            anchor->pkey.key.rsa.nlen <= 512U &&
            anchor->pkey.key.rsa.e != NULL &&
            anchor->pkey.key.rsa.elen != 0U &&
            anchor->pkey.key.rsa.elen <= 8U;
    }
    return anchor->pkey.key_type == BR_KEYTYPE_EC &&
        anchor->pkey.key.ec.curve == BR_EC_secp256r1 &&
        anchor->pkey.key.ec.q != NULL &&
        anchor->pkey.key.ec.qlen == 65U &&
        anchor->pkey.key.ec.q[0] == 4U;
}

static int transport_read(void *context, unsigned char *buffer, size_t length)
{
    struct sapote_tls_client *client = context;
    const long count = sapote_stream_read(client->stream, buffer, length,
        client->deadline_ns);

    if (count <= 0 || count > INT_MAX || (size_t)count > length) {
        client->transport_error = count;
        return -1;
    }
    return (int)count;
}

static int transport_write(void *context, const unsigned char *buffer,
    size_t length)
{
    struct sapote_tls_client *client = context;
    const long count = sapote_stream_write(client->stream, buffer, length,
        client->deadline_ns);

    if (count <= 0 || count > INT_MAX || (size_t)count > length) {
        client->transport_error = count;
        return -1;
    }
    return (int)count;
}

static enum sapote_tls_status drive_handshake(struct sapote_tls_client *client)
{
    for (size_t step = 0U; step < SAPOTE_TLS_MAX_HANDSHAKE_STEPS; ++step) {
        const unsigned state = br_ssl_engine_current_state(&client->ssl.eng);
        size_t length = 0U;

        if ((state & BR_SSL_CLOSED) != 0U) {
            return SAPOTE_TLS_HANDSHAKE;
        }
        if ((state & (BR_SSL_SENDAPP | BR_SSL_RECVAPP)) != 0U) {
            return SAPOTE_TLS_OK;
        }
        if ((state & BR_SSL_SENDREC) != 0U) {
            unsigned char *data = br_ssl_engine_sendrec_buf(
                &client->ssl.eng, &length);
            int count;

            if (data == NULL || length == 0U) {
                return SAPOTE_TLS_HANDSHAKE;
            }
            count = transport_write(client, data, length);
            if (count <= 0) {
                return SAPOTE_TLS_TRANSPORT;
            }
            br_ssl_engine_sendrec_ack(&client->ssl.eng, (size_t)count);
            continue;
        }
        if ((state & BR_SSL_RECVREC) != 0U) {
            unsigned char *data = br_ssl_engine_recvrec_buf(
                &client->ssl.eng, &length);
            int count;

            if (data == NULL || length == 0U) {
                return SAPOTE_TLS_HANDSHAKE;
            }
            count = transport_read(client, data, length);
            if (count <= 0) {
                return SAPOTE_TLS_TRANSPORT;
            }
            br_ssl_engine_recvrec_ack(&client->ssl.eng, (size_t)count);
            continue;
        }
        return SAPOTE_TLS_HANDSHAKE;
    }
    return SAPOTE_TLS_HANDSHAKE;
}

static void release_client(struct sapote_tls_client *client,
    uint64_t deadline_ns)
{
    if (client == NULL) {
        return;
    }
    if (client->stream != SAPOTE_HANDLE_INVALID) {
        (void)sapote_stream_shutdown(client->stream,
            SAPOTE_SHUTDOWN_WRITE | SAPOTE_SHUTDOWN_READ, deadline_ns);
        (void)sapote_handle_close(client->stream);
        client->stream = SAPOTE_HANDLE_INVALID;
    }
    (void)memset(client, 0, sizeof(*client));
    free(client);
}

enum sapote_tls_status sapote_tls_client_open(
    const struct sapote_tls_client_config *config,
    struct sapote_tls_client **result)
{
    struct sapote_tls_client *client;
    struct sapote_ipv4_endpoint endpoint;
    unsigned char entropy[SAPOTE_TLS_ENTROPY_BYTES];
    long realtime;
    long resolved;
    long opened;
    enum sapote_tls_status status;

    if (result == NULL) {
        return SAPOTE_TLS_ARGUMENT;
    }
    *result = NULL;
    if (config == NULL || config->reserved != 0U || config->port == 0U ||
        !hostname_valid(config->hostname) ||
        config->trust_anchors == NULL || config->trust_anchor_count == 0U ||
        config->trust_anchor_count > SAPOTE_TLS_MAX_TRUST_ANCHORS ||
        config->deadline_ns <= sapote_monotonic_ns()) {
        return SAPOTE_TLS_ARGUMENT;
    }
    for (size_t index = 0U; index < config->trust_anchor_count; ++index) {
        if (!trust_anchor_valid(&config->trust_anchors[index])) {
            return SAPOTE_TLS_TRUST;
        }
    }
    realtime = sapote_realtime_seconds();
    if (realtime < 0 ||
        (uint64_t)realtime / UINT64_C(86400) >
            UINT32_MAX - SAPOTE_TLS_UNIX_EPOCH_DAYS) {
        return SAPOTE_TLS_CLOCK;
    }
    if (sapote_random(entropy, sizeof(entropy)) != (long)sizeof(entropy)) {
        (void)memset(entropy, 0, sizeof(entropy));
        return SAPOTE_TLS_ENTROPY;
    }
    resolved = sapote_dns_resolve(config->hostname, config->deadline_ns);
    if (resolved <= 0 || (uint64_t)resolved > UINT32_MAX) {
        (void)memset(entropy, 0, sizeof(entropy));
        return SAPOTE_TLS_DNS;
    }
    opened = sapote_stream_open();
    if (opened < 0) {
        (void)memset(entropy, 0, sizeof(entropy));
        return SAPOTE_TLS_TRANSPORT;
    }
    client = calloc(1U, sizeof(*client));
    if (client == NULL) {
        (void)sapote_handle_close((sapote_handle_t)opened);
        (void)memset(entropy, 0, sizeof(entropy));
        return SAPOTE_TLS_NO_MEMORY;
    }
    client->stream = (sapote_handle_t)opened;
    client->deadline_ns = config->deadline_ns;
    endpoint = (struct sapote_ipv4_endpoint){
        (uint32_t)resolved, config->port, 0U};
    if (sapote_stream_connect(client->stream, &endpoint,
            config->deadline_ns) < 0) {
        (void)memset(entropy, 0, sizeof(entropy));
        release_client(client, config->deadline_ns);
        return SAPOTE_TLS_TRANSPORT;
    }

    br_ssl_client_init_full(&client->ssl, &client->x509,
        config->trust_anchors, config->trust_anchor_count);
    br_ssl_engine_set_versions(&client->ssl.eng, BR_TLS12, BR_TLS12);
    br_ssl_engine_set_suites(&client->ssl.eng, suites,
        sizeof(suites) / sizeof(suites[0]));
    br_ssl_engine_set_all_flags(&client->ssl.eng, BR_OPT_NO_RENEGOTIATION);
    br_ssl_engine_set_buffer(&client->ssl.eng, client->buffer,
        sizeof(client->buffer), 1);
    br_x509_minimal_set_minrsa(&client->x509, 256);
    br_x509_minimal_set_time(&client->x509,
        (uint32_t)((uint64_t)realtime / UINT64_C(86400) +
            SAPOTE_TLS_UNIX_EPOCH_DAYS),
        (uint32_t)((uint64_t)realtime % UINT64_C(86400)));
    br_ssl_engine_inject_entropy(&client->ssl.eng, entropy, sizeof(entropy));
    (void)memset(entropy, 0, sizeof(entropy));
    if (!br_ssl_client_reset(&client->ssl, config->hostname, 0)) {
        release_client(client, config->deadline_ns);
        return SAPOTE_TLS_HANDSHAKE;
    }
    status = drive_handshake(client);
    if (status != SAPOTE_TLS_OK) {
        client->status = status;
        release_client(client, config->deadline_ns);
        return status;
    }
    br_sslio_init(&client->io, &client->ssl.eng, transport_read, client,
        transport_write, client);
    client->connected = true;
    client->status = SAPOTE_TLS_OK;
    *result = client;
    return SAPOTE_TLS_OK;
}

long sapote_tls_client_read(struct sapote_tls_client *client, void *buffer,
    size_t length, uint64_t deadline_ns)
{
    int count;

    if (client == NULL || !client->connected ||
        (buffer == NULL && length != 0U) ||
        length > INT_MAX ||
        deadline_ns <= sapote_monotonic_ns()) {
        return -1;
    }
    if (length == 0U) {
        return 0;
    }
    client->deadline_ns = deadline_ns;
    count = br_sslio_read(&client->io, buffer, length);
    if (count < 0) {
        client->status = SAPOTE_TLS_IO;
        return -1;
    }
    return count;
}

long sapote_tls_client_write(struct sapote_tls_client *client,
    const void *buffer, size_t length, uint64_t deadline_ns)
{
    int count;

    if (client == NULL || !client->connected ||
        (buffer == NULL && length != 0U) ||
        length > INT_MAX ||
        deadline_ns <= sapote_monotonic_ns()) {
        return -1;
    }
    if (length == 0U) {
        return 0;
    }
    client->deadline_ns = deadline_ns;
    count = br_sslio_write(&client->io, buffer, length);
    if (count < 0) {
        client->status = SAPOTE_TLS_IO;
        return -1;
    }
    return count;
}

enum sapote_tls_status sapote_tls_client_flush(
    struct sapote_tls_client *client, uint64_t deadline_ns)
{
    if (client == NULL || !client->connected ||
        deadline_ns <= sapote_monotonic_ns()) {
        return SAPOTE_TLS_ARGUMENT;
    }
    client->deadline_ns = deadline_ns;
    if (br_sslio_flush(&client->io) != 0) {
        client->status = SAPOTE_TLS_IO;
    }
    return client->status;
}

enum sapote_tls_status sapote_tls_client_close(
    struct sapote_tls_client *client, uint64_t deadline_ns)
{
    enum sapote_tls_status status;

    if (client == NULL) {
        return SAPOTE_TLS_ARGUMENT;
    }
    status = client->status;
    if (client->connected) {
        client->deadline_ns = deadline_ns;
        if (br_sslio_close(&client->io) == 0 && status == SAPOTE_TLS_OK) {
            status = SAPOTE_TLS_CLOSE;
        }
    }
    release_client(client, deadline_ns);
    return status;
}

enum sapote_tls_status sapote_tls_client_status(
    const struct sapote_tls_client *client)
{
    return client == NULL ? SAPOTE_TLS_ARGUMENT : client->status;
}

int sapote_tls_client_bearssl_error(const struct sapote_tls_client *client)
{
    return client == NULL ? -1 : br_ssl_engine_last_error(&client->ssl.eng);
}

long sapote_tls_client_transport_error(const struct sapote_tls_client *client)
{
    return client == NULL ? 0 : client->transport_error;
}

const char *sapote_tls_status_string(enum sapote_tls_status status)
{
    static const char *const names[] = {
        "ok", "invalid TLS argument", "TLS allocation failed",
        "invalid or empty TLS trust store", "realtime clock unavailable",
        "kernel entropy unavailable", "DNS resolution failed",
        "TCP transport failed", "TLS handshake or authentication failed",
        "TLS application I/O failed", "authenticated TLS close failed"
    };

    return (unsigned)status < sizeof(names) / sizeof(names[0]) ?
        names[status] : "unknown TLS status";
}
