#define _POSIX_C_SOURCE 200809L
#include "wonder_trade/http.h"
#include "wonder_trade/seed.h"
#include "wonder_trade/tls.h"
#include "wonder_trade/wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <mbedtls/memory_buffer_alloc.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_util.h>
#include <psa/crypto.h>

static WtSeed seedStore;
static unsigned char arena[WT_TLS_HEAP_SIZE];
static int seedFd;
static int writes;
static int failWrite;
static WtHttpExchange exchange;

typedef struct HttpContext { WtTls *tls; time_t deadline; } HttpContext;

static int hash(const unsigned char *bytes, size_t size, unsigned char digest[32])
{
    size_t length = 0;
    return psa_hash_compute(PSA_ALG_SHA_256, bytes, size, digest, 32, &length) == PSA_SUCCESS && length == 32 ? 0 : -1;
}

static int readBank(void *arg, int bank, unsigned char record[4096])
{
    (void)arg;
    return pread(seedFd, record, 4096, bank * 4096) == 4096 ? 0 : -1;
}

static int writeBank(void *arg, int bank, const unsigned char record[4096])
{
    unsigned char check[4096];
    (void)arg;
    writes++;
    if (writes == failWrite) {
        return -1;
    }
    if (pwrite(seedFd, record, 4096, bank * 4096) != 4096 || fsync(seedFd) != 0
        || readBank(NULL, bank, check) != 0 || memcmp(check, record, 4096) != 0) {
        return -1;
    }
    return 0;
}

static int readSeed(unsigned char *output, size_t size)
{
    return WtSeed_Read(&seedStore, output, size);
}

static int writeSeed(unsigned char *output, size_t size)
{
    return WtSeed_Write(&seedStore, output, size);
}

static int sendData(void *arg, const unsigned char *data, size_t size)
{
    int count = (int)send(*(int *)arg, data, size, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return count < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : count;
}

static int receiveData(void *arg, unsigned char *data, size_t size)
{
    int count = (int)recv(*(int *)arg, data, size, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return count < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : count;
}

static int waitAgain(int result, time_t deadline)
{
    struct timespec delay = { 0, 1000000 };
    if (result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0;
    }
    if (time(NULL) >= deadline) {
        return 0;
    }
    nanosleep(&delay, NULL);
    return 1;
}

static int writeHttp(void *arg, const unsigned char *bytes, size_t size)
{
    return WtTls_Write(((HttpContext *)arg)->tls, bytes, size);
}

static int readHttp(void *arg, unsigned char *bytes, size_t size)
{
    return WtTls_Read(((HttpContext *)arg)->tls, bytes, size);
}

static int waitHttp(void *arg, int result)
{
    return waitAgain(result, ((HttpContext *)arg)->deadline);
}

int main(int argc, char **argv)
{
    WtTls tls;
    WtWireReply reply;
    HttpContext context = { &tls, 0 };
    WtHttpIO io = { &context, writeHttp, readHttp, waitHttp };
    struct sockaddr_in address;
    unsigned char pins[2][32];
    unsigned char request[WT_WIRE_REQUEST_MAX + 1];
    size_t requestSize = 0, peak = 0, blocks = 0;
    size_t used = 0, live = 0;
    time_t deadline = time(NULL) + 5;
    int socketFd, result, i, p;

    if (argc != 7 && argc != 8) {
        return 2;
    }
    if (argc == 8) {
        FILE *input = fopen(argv[7], "rb");
        int invalid;
        if (!input) return 2;
        requestSize = fread(request, 1, sizeof(request), input);
        invalid = ferror(input) || requestSize == 0 || requestSize > WT_WIRE_REQUEST_MAX;
        if (fclose(input) != 0 || invalid || WtWire_CheckRequest(request, requestSize, hash) != 0) return 2;
    }
    for (p = 0; p < 2; p++) {
        if (strlen(argv[3 + p]) != 64) {
            return 2;
        }
        for (i = 0; i < 32; i++) {
            char hex[3] = { argv[3 + p][2 * i], argv[3 + p][2 * i + 1], 0 };
            pins[p][i] = (unsigned char)strtoul(hex, NULL, 16);
        }
    }
    seedFd = open(argv[5], O_RDWR);
    failWrite = atoi(argv[6]);
    if (seedFd < 0) {
        return 2;
    }
    mbedtls_memory_buffer_alloc_init(arena, sizeof(arena));
    mbedtls_platform_set_nv_seed(readSeed, writeSeed);
    result = WtSeed_Open(&seedStore, NULL, readBank, writeBank);
    if (result != 0) {
        close(seedFd);
        return 3;
    }
    socketFd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((unsigned short)atoi(argv[1]));
    if (connect(socketFd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        return 4;
    }
    fcntl(socketFd, F_SETFL, O_NONBLOCK);
    result = WtTls_Open(&tls, argv[2], pins, &socketFd, sendData, receiveData);
    if (result == 0) {
        do {
            result = WtTls_Handshake(&tls);
        } while (waitAgain(result, deadline));
    }
    if (result == 0) {
        context.deadline = time(NULL) + 5;
        result = WtHttp_Exchange(&exchange, io, "localhost", requestSize ? request : NULL, requestSize);
        if (result == 0 && requestSize) {
            result = WtWire_Decode(request, requestSize, exchange.body, exchange.bodySize, hash, &reply);
            if (result == 0) printf("wire_result=%u\n", (unsigned)reply.result);
        } else if (result == 0) {
            result = exchange.bodySize == sizeof("DSWT_ACK_V1\n") - 1
                && memcmp(exchange.body, "DSWT_ACK_V1\n", sizeof("DSWT_ACK_V1\n") - 1) == 0 ? 0 : -1;
        }
    }
    mbedtls_platform_zeroize(request, sizeof(request));
    mbedtls_platform_zeroize(&exchange, sizeof(exchange));
    mbedtls_platform_zeroize(&reply, sizeof(reply));
    WtTls_Close(&tls);
    close(socketFd);
    close(seedFd);
    mbedtls_memory_buffer_alloc_max_get(&peak, &blocks);
    mbedtls_memory_buffer_alloc_cur_get(&used, &live);
    fprintf(stderr, "result=%d peak=%zu blocks=%zu live=%zu writes=%d\n", result, peak, blocks, live, writes);
    mbedtls_memory_buffer_alloc_free();
    return result == 0 && used == 0 && live == 0 ? 0 : 1;
}
