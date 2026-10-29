#define _POSIX_C_SOURCE 200809L
#include "probe.h"
#include "wonder_trade/http.h"
#include "wonder_trade/seed.h"
#include "wonder_trade/tls.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

typedef struct Connection { WtTls tls; struct timespec deadline; } Connection;

static int Send(void *arg, const unsigned char *bytes, size_t size)
{
    int count = (int)send(*(int *)arg, bytes, size, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return count < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : count;
}

static int Receive(void *arg, unsigned char *bytes, size_t size)
{
    int count = (int)recv(*(int *)arg, bytes, size, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_READ;
    return count < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : count;
}

static int Wait(void *arg, int result)
{
    Connection *connection = arg;
    struct timespec now, pause = {0, 1000000};
    if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec >= connection->deadline.tv_sec) return 0;
    nanosleep(&pause, NULL);
    return 1;
}

static int WriteHttp(void *arg, const unsigned char *bytes, size_t size)
{
    return WtTls_Write(&((Connection *)arg)->tls, bytes, size);
}

static int ReadHttp(void *arg, unsigned char *bytes, size_t size)
{
    return WtTls_Read(&((Connection *)arg)->tls, bytes, size);
}

/* Load tests may give each simulated client its own loopback network; unset keeps the kernel's choice. */
static int BindSource(int fd)
{
    const char *source = getenv("WT_PROBE_SOURCE");
    struct sockaddr_in local = {0};
    if (!source) return 0;
    local.sin_family = AF_INET;
    if (inet_pton(AF_INET, source, &local.sin_addr) != 1 || (ntohl(local.sin_addr.s_addr) >> 24) != 127) return -1;
    return bind(fd, (struct sockaddr *)&local, sizeof(local));
}

int Exchange(Probe *probe, unsigned port, const char *hostname, const unsigned char pins[2][32])
{
    Connection connection = {0};
    WtHttpIO io = {&connection, WriteHttp, ReadHttp, Wait};
    WtHttpExchange http;
    WtWireReply reply;
    struct sockaddr_in address = {0};
    char checkpoint[64];
    unsigned operation = probe->engine.frame[5];
    const char *stage = "connect";
    struct timespec started, finished;
    int fd = -1, result = -1, opened = 0;
    if (!WtSeed_RequestMatches(&probe->engine.journal, probe->engine.frame, probe->engine.frameSize)) return -1;
    snprintf(checkpoint, sizeof(checkpoint), "wire.%u.send", operation);
    if (Checkpoint(probe, checkpoint) != 0) return -1;
    clock_gettime(CLOCK_MONOTONIC, &started);
    fd = socket(AF_INET, SOCK_STREAM, 0);
    address.sin_family = AF_INET; address.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) goto done;
    if (fd < 0 || BindSource(fd) != 0 || connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0
        || fcntl(fd, F_SETFL, O_NONBLOCK) != 0) goto done;
    clock_gettime(CLOCK_MONOTONIC, &connection.deadline); connection.deadline.tv_sec += 10;
    opened = 1;
    stage = "tls-open";
    result = WtTls_Open(&connection.tls, hostname, pins, &fd, Send, Receive);
    if (result != 0) goto done;
    stage = "tls-handshake";
    do { result = WtTls_Handshake(&connection.tls); } while (Wait(&connection, result));
    if (result != 0) goto done;
    stage = "http";
    result = WtHttp_Exchange(&http, io, hostname, probe->engine.frame, probe->engine.frameSize);
    if (result != 0) goto done;
    stage = "wire-decode";
    result = WtWire_Decode(probe->engine.frame, probe->engine.frameSize, http.body, http.bodySize, Hash, &reply);
    if (result != 0) goto done;
    clock_gettime(CLOCK_MONOTONIC, &finished);
    /* Client-observed connect-to-decoded-reply time, including TLS, for load reports. */
    printf("{\"wire_operation\":%u,\"wire_result\":%u,\"next_counter\":%llu,\"highwater\":%llu,\"elapsed_us\":%lld}\n",
        operation, reply.result, (unsigned long long)reply.nextCounter, (unsigned long long)reply.highwater,
        (long long)(finished.tv_sec - started.tv_sec) * 1000000 + (finished.tv_nsec - started.tv_nsec) / 1000);
    /* The supervisor can discard a validated, committed response before application. */
    snprintf(checkpoint, sizeof(checkpoint), "wire.%u.reply", operation);
    if (Checkpoint(probe, checkpoint) != 0) { result = -1; goto done; }
    stage = "engine-reply";
    result = WtEngine_Reply(&probe->engine, http.body, http.bodySize);
    if (result >= 0 && reply.result != WT_RESULT_OK && reply.result != WT_RESULT_ILLEGAL_POKEMON
        && !(operation == WT_OP_CANCEL && (reply.result == WT_RESULT_CONFLICT || reply.result == WT_RESULT_NOT_FOUND))) result = -100 - reply.result;
done:
    if (result < 0) printf("{\"transport_stage\":\"%s\",\"result\":%d,\"tls_state\":%d,\"tls_flags\":%u}\n",
        stage, result, connection.tls.ssl.MBEDTLS_PRIVATE(state), (unsigned)mbedtls_ssl_get_verify_result(&connection.tls.ssl));
    if (opened) WtTls_Close(&connection.tls);
    if (fd >= 0) close(fd);
    return result;
}
