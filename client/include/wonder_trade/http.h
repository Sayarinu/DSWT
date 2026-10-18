#ifndef WONDER_TRADE_HTTP_H
#define WONDER_TRADE_HTTP_H

#include <stddef.h>

#define WT_HTTP_HEADER_MAX 2048
#define WT_HTTP_BODY_MAX 4096

/* Parse exactly one bounded, fixed-length HTTP/1.1 200 response. */
typedef struct WtHttpResponse {
    unsigned char headers[WT_HTTP_HEADER_MAX + 1];
    size_t headerSize;
    size_t bodySize;
    size_t contentLength;
    int headersComplete;
    int complete;
} WtHttpResponse;

void WtHttp_Init(WtHttpResponse *response);
int WtHttp_Feed(WtHttpResponse *response, const unsigned char *data, size_t size,
    unsigned char *body, size_t capacity);

/* Callbacks use Mbed TLS read/write return codes. wait returns whether to retry
 * WANT_READ/WANT_WRITE under the caller's deadline and cancellation policy. */
typedef struct WtHttpIO {
    void *arg;
    int (*write)(void *, const unsigned char *, size_t);
    int (*read)(void *, unsigned char *, size_t);
    int (*wait)(void *, int);
} WtHttpIO;

/* Caller-owned storage, kept off the DS main/worker stacks. Only body/bodySize
 * are results; the remaining fields belong to the exchange implementation. */
typedef struct WtHttpExchange {
    unsigned char body[WT_HTTP_BODY_MAX];
    size_t bodySize;
    WtHttpResponse response;
    unsigned char headers[512], input[512];
} WtHttpExchange;

/* An absent body selects GET /v1/ping; a nonempty body selects POST /wt/v1.
 * Accept exactly one fixed-length 200 response followed by peer closure.
 * The authenticated Content-Length delimits the body even with a plain EOF;
 * EOF before completion and any trailing application bytes fail closed.
 * Request and authority storage must not overlap exchange.
 * Failure clears all exchange storage. This function never retries a request. */
int WtHttp_Exchange(WtHttpExchange *, WtHttpIO, const char *authority,
    const unsigned char *request, size_t requestSize);

#endif
