/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L

#include <sapote/abi.h>
#include <sapote/network.h>
#include <sapote/runtime.h>
#include <sapote/tls.h>

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
typedef SOCKET host_socket_t;
#define HOST_INVALID_SOCKET INVALID_SOCKET
#define HOST_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <pthread.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
typedef int host_socket_t;
#define HOST_INVALID_SOCKET (-1)
#define HOST_CLOSE close
#endif

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#include "../apps/native-https/trust_anchor.h"

/* include/sapote/network.h is the kernel-private header and intentionally
 * shadows the installed SDK header in host admission builds. */
long sapote_dns_resolve(const char *hostname, uint64_t deadline_ns);
long sapote_stream_open(void);
long sapote_stream_connect(sapote_handle_t stream,
    const struct sapote_ipv4_endpoint *endpoint, uint64_t deadline_ns);
long sapote_stream_read(sapote_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns);
long sapote_stream_write(sapote_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns);
long sapote_stream_shutdown(sapote_handle_t stream, uint32_t flags,
    uint64_t deadline_ns);
long sapote_network_cancel(sapote_handle_t handle);

static uint16_t peer_port;
static unsigned live_handles;
static uintptr_t canceled_socket_bits = (uintptr_t)HOST_INVALID_SOCKET;
static host_socket_t active_socket = HOST_INVALID_SOCKET;
static bool stream_read_waiting;

struct cancel_read_task {
    struct sapote_tls_client *client;
    uint64_t deadline_ns;
    long result;
    long transport_error;
    bool entered;
};

static uint64_t host_now_ns(void)
{
#if defined(_WIN32)
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter;

    if (!QueryPerformanceFrequency(&frequency) ||
        !QueryPerformanceCounter(&counter) || frequency.QuadPart <= 0 ||
        frequency.QuadPart > INT64_C(1000000000)) {
        return UINT64_MAX;
    }
    {
        const uint64_t frequency_value = (uint64_t)frequency.QuadPart;
        const uint64_t counter_value = (uint64_t)counter.QuadPart;
        const uint64_t seconds = counter_value / frequency_value;
        const uint64_t remainder = counter_value % frequency_value;

        if (seconds > UINT64_MAX / UINT64_C(1000000000)) {
            return UINT64_MAX;
        }
        return seconds * UINT64_C(1000000000) +
            remainder * UINT64_C(1000000000) / frequency_value;
    }
#else
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
        return UINT64_MAX;
    }
    return (uint64_t)value.tv_sec * UINT64_C(1000000000) +
        (uint64_t)value.tv_nsec;
#endif
}

static void host_pause_millis(unsigned milliseconds)
{
#if defined(_WIN32)
    Sleep(milliseconds);
#else
    const struct timespec duration = {
        (time_t)(milliseconds / 1000U),
        (long)(milliseconds % 1000U) * 1000000L};

    (void)nanosleep(&duration, NULL);
#endif
}

#if defined(_WIN32)
static DWORD WINAPI cancel_reader(LPVOID opaque)
#else
static void *cancel_reader(void *opaque)
#endif
{
    struct cancel_read_task *task = opaque;
    unsigned char byte;

    __atomic_store_n(&task->entered, true, __ATOMIC_RELEASE);
    task->result = sapote_tls_client_read(task->client, &byte, 1U,
        task->deadline_ns);
    task->transport_error =
        sapote_tls_client_transport_error(task->client);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static int wait_socket(host_socket_t descriptor, bool write,
    uint64_t deadline_ns)
{
    for (;;) {
        const uint64_t now = host_now_ns();
        uint64_t remaining;
        int milliseconds;

        if (now >= deadline_ns) {
            return -1;
        }
        remaining = deadline_ns - now;
        milliseconds = remaining / UINT64_C(1000000) > INT_MAX ? INT_MAX :
            (int)(remaining / UINT64_C(1000000));
        if (milliseconds == 0) {
            milliseconds = 1;
        }
#if defined(_WIN32)
        {
            fd_set set;
            struct timeval timeout = {
                milliseconds / 1000, (milliseconds % 1000) * 1000};
            int result;

            FD_ZERO(&set);
            FD_SET(descriptor, &set);
            result = select(0, write ? NULL : &set, write ? &set : NULL,
                NULL, &timeout);
            if (result > 0) {
                return 0;
            }
            if (result == 0) {
                return -1;
            }
            return -2;
        }
#else
        {
            struct pollfd item = {
                descriptor, (short)(write ? POLLOUT : POLLIN), 0};
            const int result = poll(&item, 1, milliseconds);

            if (result > 0) {
                return 0;
            }
            if (result == 0) {
                return -1;
            }
            if (errno != EINTR) {
                return -2;
            }
        }
#endif
    }
}

static long socket_error(host_socket_t descriptor)
{
    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        return -(long)SAPOTE_ECANCELED;
    }
#if defined(_WIN32)
    switch (WSAGetLastError()) {
    case WSAETIMEDOUT:
    case WSAEWOULDBLOCK:
        return -(long)SAPOTE_ETIMEDOUT;
    case WSAECONNRESET:
        return -(long)SAPOTE_EIO;
    default:
        return -(long)SAPOTE_EPIPE;
    }
#else
    switch (errno) {
    case ETIMEDOUT:
    case EAGAIN:
        return -(long)SAPOTE_ETIMEDOUT;
    case ECONNRESET:
        return -(long)SAPOTE_EIO;
    default:
        return -(long)SAPOTE_EPIPE;
    }
#endif
}

static host_socket_t host_descriptor(sapote_handle_t handle)
{
    return handle == 1U ? active_socket : HOST_INVALID_SOCKET;
}

uint64_t sapote_monotonic_ns(void)
{
    return host_now_ns();
}

long sapote_realtime_seconds(void)
{
    return INT64_C(1788177600);
}

long sapote_random(void *buffer, size_t length)
{
    unsigned char *bytes = buffer;
    uint32_t state = UINT32_C(0x8f31a42d);

    if (buffer == NULL && length != 0U) {
        return -(long)SAPOTE_EINVAL;
    }
    for (size_t index = 0U; index < length; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = (unsigned char)state;
    }
    return (long)length;
}

long sapote_random_strong(void *buffer, size_t length)
{
    return sapote_random(buffer, length);
}

long sapote_dns_resolve(const char *hostname, uint64_t deadline_ns)
{
    if (hostname == NULL || hostname[0] == '\0' ||
        deadline_ns <= host_now_ns()) {
        return -(long)SAPOTE_EINVAL;
    }
    return INT64_C(0x7f000001);
}

long sapote_stream_open(void)
{
    const host_socket_t descriptor = socket(AF_INET, SOCK_STREAM, 0);

    if (descriptor == HOST_INVALID_SOCKET ||
        active_socket != HOST_INVALID_SOCKET) {
        if (descriptor != HOST_INVALID_SOCKET) {
            (void)HOST_CLOSE(descriptor);
        }
        return -(long)SAPOTE_EIO;
    }
    active_socket = descriptor;
    ++live_handles;
    return 1;
}

long sapote_stream_connect(sapote_handle_t stream,
    const struct sapote_ipv4_endpoint *endpoint, uint64_t deadline_ns)
{
    const host_socket_t descriptor = host_descriptor(stream);
    struct sockaddr_in address;

    if (endpoint == NULL || endpoint->port != peer_port ||
        deadline_ns <= host_now_ns()) {
        return -(long)SAPOTE_EINVAL;
    }
    (void)memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(peer_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(descriptor, (const struct sockaddr *)&address,
            sizeof(address)) != 0) {
        return socket_error(descriptor);
    }
    return 0;
}

long sapote_stream_read(sapote_handle_t stream, void *buffer, size_t length,
    uint64_t deadline_ns)
{
    const host_socket_t descriptor = host_descriptor(stream);
    int count;
    int ready;

    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        return -(long)SAPOTE_ECANCELED;
    }
    __atomic_store_n(&stream_read_waiting, true, __ATOMIC_RELEASE);
    ready = wait_socket(descriptor, false, deadline_ns);
    __atomic_store_n(&stream_read_waiting, false, __ATOMIC_RELEASE);
    if (ready != 0) {
        return ready == -1 ? -(long)SAPOTE_ETIMEDOUT :
            socket_error(descriptor);
    }
    count = recv(descriptor, (char *)buffer,
        length > INT_MAX ? INT_MAX : (int)length, 0);
    return count > 0 ? count : count == 0 ? -(long)SAPOTE_EPIPE :
        socket_error(descriptor);
}

long sapote_stream_write(sapote_handle_t stream, const void *buffer,
    size_t length, uint64_t deadline_ns)
{
    const host_socket_t descriptor = host_descriptor(stream);
    int count;
    int ready;

    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        return -(long)SAPOTE_ECANCELED;
    }
    ready = wait_socket(descriptor, true, deadline_ns);
    if (ready != 0) {
        return ready == -1 ? -(long)SAPOTE_ETIMEDOUT :
            socket_error(descriptor);
    }
    count = send(descriptor, (const char *)buffer,
        length > INT_MAX ? INT_MAX : (int)length, MSG_NOSIGNAL);
    return count > 0 ? count : socket_error(descriptor);
}

long sapote_stream_shutdown(sapote_handle_t stream, uint32_t flags,
    uint64_t deadline_ns)
{
    const host_socket_t descriptor = host_descriptor(stream);

    (void)flags;
    (void)deadline_ns;
#if defined(_WIN32)
    return shutdown(descriptor, SD_BOTH) == 0 ? 0 :
        socket_error(descriptor);
#else
    return shutdown(descriptor, SHUT_RDWR) == 0 ? 0 :
        socket_error(descriptor);
#endif
}

long sapote_network_cancel(sapote_handle_t handle)
{
    const host_socket_t descriptor = host_descriptor(handle);

    __atomic_store_n(&canceled_socket_bits, (uintptr_t)descriptor,
        __ATOMIC_RELEASE);
#if defined(_WIN32)
    (void)shutdown(descriptor, SD_BOTH);
#else
    (void)shutdown(descriptor, SHUT_RDWR);
#endif
    return 0;
}

long sapote_handle_close(sapote_handle_t handle)
{
    const host_socket_t descriptor = host_descriptor(handle);
    int result;

    result = HOST_CLOSE(descriptor);
    if (live_handles == 0U) {
        return -(long)SAPOTE_ESTALE;
    }
    --live_handles;
    active_socket = HOST_INVALID_SOCKET;
    if ((uintptr_t)descriptor ==
            __atomic_load_n(&canceled_socket_bits, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&canceled_socket_bits,
            (uintptr_t)HOST_INVALID_SOCKET, __ATOMIC_RELEASE);
    }
    return result == 0 ? 0 : -(long)SAPOTE_EIO;
}

static int parse_unsigned(const char *text, unsigned long maximum,
    unsigned long *result)
{
    char *end;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > maximum) {
        return -1;
    }
    *result = value;
    return 0;
}

static int run_cancel(const char *hostname, uint64_t deadline_ns)
{
    struct sapote_tls_client *client = NULL;
    const struct sapote_tls_client_config config = {
        hostname, peer_port, 0U, sapote_https_test_anchors,
        sizeof(sapote_https_test_anchors) /
            sizeof(sapote_https_test_anchors[0]), deadline_ns};
    struct cancel_read_task task;
    long cancel_result;
    bool joined;
#if defined(_WIN32)
    HANDLE thread;
#else
    pthread_t thread;
#endif

    if (sapote_tls_client_open(&config, &client) != SAPOTE_TLS_OK ||
        client == NULL) {
        return 1;
    }
    task = (struct cancel_read_task){client, deadline_ns, 0, 0, false};
#if defined(_WIN32)
    thread = CreateThread(NULL, 0, cancel_reader, &task, 0, NULL);
    if (thread == NULL) {
        (void)sapote_tls_client_close(client, deadline_ns);
        return 1;
    }
#else
    if (pthread_create(&thread, NULL, cancel_reader, &task) != 0) {
        (void)sapote_tls_client_close(client, deadline_ns);
        return 1;
    }
#endif
    for (unsigned wait = 0U; wait < 1000U &&
            !__atomic_load_n(&stream_read_waiting, __ATOMIC_ACQUIRE); ++wait) {
        host_pause_millis(1U);
    }
    if (!__atomic_load_n(&stream_read_waiting, __ATOMIC_ACQUIRE)) {
        (void)sapote_tls_client_cancel(client);
#if defined(_WIN32)
        (void)WaitForSingleObject(thread, 2000U);
        (void)CloseHandle(thread);
#else
        (void)pthread_join(thread, NULL);
#endif
        (void)sapote_tls_client_close(client, deadline_ns);
        return 1;
    }
    cancel_result = sapote_tls_client_cancel(client);
#if defined(_WIN32)
    joined = WaitForSingleObject(thread, 2000U) == WAIT_OBJECT_0;
    (void)CloseHandle(thread);
#else
    joined = pthread_join(thread, NULL) == 0;
#endif
    if (cancel_result != 0 || !joined || task.result != -1 ||
        task.transport_error != -(long)SAPOTE_ECANCELED ||
        sapote_tls_client_close(client, deadline_ns) != SAPOTE_TLS_IO) {
        return 1;
    }
    puts("HTTPS REFUSAL blocking TLS operation canceled across threads");
    return 0;
}

static int run_expired_operation(const char *hostname, uint64_t deadline_ns)
{
    struct sapote_tls_client *client = NULL;
    const struct sapote_tls_client_config config = {
        hostname, peer_port, 0U, sapote_https_test_anchors,
        sizeof(sapote_https_test_anchors) /
            sizeof(sapote_https_test_anchors[0]), deadline_ns};
    unsigned char byte;

    if (sapote_tls_client_open(&config, &client) != SAPOTE_TLS_OK ||
        client == NULL) {
        return 1;
    }
    while (host_now_ns() <= deadline_ns) {
        host_pause_millis(1U);
    }
    if (sapote_tls_client_read(client, &byte, 1U, deadline_ns) != -1 ||
        sapote_tls_client_transport_error(client) !=
            -(long)SAPOTE_ETIMEDOUT ||
        sapote_tls_client_close(client, deadline_ns) != SAPOTE_TLS_IO) {
        return 1;
    }
    puts("HTTPS REFUSAL expired per-operation deadline classified");
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long parsed_port;
    unsigned long expected;
    const char *mode;
    const char *hostname;
    uint64_t deadline_ns;
    int result = 0;

#if defined(_WIN32)
    WSADATA sockets;

    if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) {
        return 2;
    }
#endif
    if (argc != 5 || parse_unsigned(argv[1], UINT16_MAX, &parsed_port) != 0 ||
        parsed_port == 0U ||
        parse_unsigned(argv[4], SAPOTE_HTTPS_CLOSE, &expected) != 0) {
        fprintf(stderr, "usage: %s PORT HOSTNAME MODE EXPECTED_STATUS\n",
            argv[0]);
        result = 2;
        goto finished;
    }
    peer_port = (uint16_t)parsed_port;
    hostname = argv[2];
    mode = argv[3];
    deadline_ns = host_now_ns() +
        (strcmp(mode, "timeout") == 0 ? UINT64_C(300000000) :
         strcmp(mode, "expired-operation") == 0 ? UINT64_C(1000000000) :
            UINT64_C(3000000000));
    if (strcmp(mode, "cancel") == 0) {
        result = expected == SAPOTE_HTTPS_CANCELED ?
            run_cancel(hostname, deadline_ns) : 1;
    } else if (strcmp(mode, "expired-operation") == 0) {
        result = expected == SAPOTE_HTTPS_TIMEOUT ?
            run_expired_operation(hostname, deadline_ns) : 1;
    } else {
        unsigned char body[128];
        struct sapote_https_response response;
        const struct sapote_https_request request = {
            hostname, peer_port, 0U, "/artifact.bin",
            sapote_https_test_anchors,
            sizeof(sapote_https_test_anchors) /
                sizeof(sapote_https_test_anchors[0]),
            deadline_ns, body, sizeof(body)};
        const enum sapote_https_status status =
            sapote_https_get(&request, &response);
        static const unsigned char expected_body[] =
            "hello from the Sapote HTTPS peer\n";

        printf("HTTPS RESULT %u %s tls=%d transport=%ld handles=%u\n",
            (unsigned)status, sapote_https_status_string(status),
            response.bearssl_error, response.transport_error, live_handles);
        if ((unsigned long)status != expected ||
            (status == SAPOTE_HTTPS_OK &&
             (response.status_code != 200U ||
              response.body_length != sizeof(expected_body) - 1U ||
              memcmp(body, expected_body, sizeof(expected_body) - 1U) != 0))) {
            result = 1;
        }
    }
    if (live_handles != 0U) {
        fprintf(stderr, "HTTPS HANDLE LEAK %u\n", live_handles);
        result = 1;
    } else {
        puts("HTTPS HANDLES clean");
    }
finished:
#if defined(_WIN32)
    (void)WSACleanup();
#endif
    return result;
}
