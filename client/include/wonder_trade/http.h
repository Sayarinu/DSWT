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

#endif
