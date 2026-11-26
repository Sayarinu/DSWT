#include "wonder_trade/http.h"

#include <string.h>
#include <stdio.h>
#include <mbedtls/ssl.h>

static int WtHttp_Equal(const char *value, size_t size, const char *expected)
{
    size_t i;
    if (size != strlen(expected)) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c >= 'A' && c <= 'Z') {
            c += 'a' - 'A';
        }
        if (c != (unsigned char)expected[i]) {
            return 0;
        }
    }
    return 1;
}

static int WtHttp_Headers(WtHttpResponse *response, size_t capacity)
{
    char *line = (char *)response->headers;
    char *end = strstr(line, "\r\n");
    int lengthFound = 0;
    int typeFound = 0;
    int closeFound = 0;

    if (end == NULL || end - line < 13 || memcmp(line, "HTTP/1.1 200 ", 13) != 0) {
        return -1;
    }
    line = end + 2;
    while (*line != '\r') {
        char *colon;
        char *value;
        char *valueEnd;
        char *name;
        size_t length = 0;
        end = strstr(line, "\r\n");
        colon = strchr(line, ':');
        if (end == NULL || colon == NULL || colon >= end || colon == line) {
            return -1;
        }
        for (name = line; name < colon; name++) {
            unsigned char c = (unsigned char)*name;
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || strchr("!#$%&'*+-.^_`|~", c) != NULL)) {
                return -1;
            }
        }
        value = colon + 1;
        while (value < end && (*value == ' ' || *value == '\t')) {
            value++;
        }
        valueEnd = end;
        while (valueEnd > value && (valueEnd[-1] == ' ' || valueEnd[-1] == '\t')) {
            valueEnd--;
        }
        if (WtHttp_Equal(line, colon - line, "content-length")) {
            if (lengthFound++ || value == valueEnd) {
                return -1;
            }
            for (; value < valueEnd; value++) {
                if (*value < '0' || *value > '9' || length > WT_HTTP_BODY_MAX / 10) {
                    return -1;
                }
                length = length * 10 + (*value - '0');
            }
            if (length > capacity || length > WT_HTTP_BODY_MAX) {
                return -1;
            }
            response->contentLength = length;
        } else if (WtHttp_Equal(line, colon - line, "transfer-encoding")
            || WtHttp_Equal(line, colon - line, "content-encoding")
            || WtHttp_Equal(line, colon - line, "upgrade")
            || WtHttp_Equal(line, colon - line, "trailer")
            || WtHttp_Equal(line, colon - line, "te")) {
            return -1;
        } else if (WtHttp_Equal(line, colon - line, "content-type")) {
            if (typeFound++ || !WtHttp_Equal(value, valueEnd - value, "application/octet-stream")) {
                return -1;
            }
        } else if (WtHttp_Equal(line, colon - line, "connection")) {
            if (closeFound++ || !WtHttp_Equal(value, valueEnd - value, "close")) return -1;
        }
        line = end + 2;
    }
    return lengthFound == 1 && typeFound == 1 && closeFound == 1 ? 0 : -1;
}

void WtHttp_Init(WtHttpResponse *response)
{
    memset(response, 0, sizeof(*response));
}

int WtHttp_Feed(WtHttpResponse *response, const unsigned char *data, size_t size,
    unsigned char *body, size_t capacity)
{
    size_t i;

    for (i = 0; i < size && !response->headersComplete; i++) {
        unsigned char c = data[i];
        if (response->headerSize == WT_HTTP_HEADER_MAX || c == 0 || c == 127
            || (c < 32 && c != '\r' && c != '\n' && c != '\t')
            || (c == '\n' && (response->headerSize == 0 || response->headers[response->headerSize - 1] != '\r'))
            || (response->headerSize > 0 && response->headers[response->headerSize - 1] == '\r' && c != '\n')) {
            return -1;
        }
        response->headers[response->headerSize++] = c;
        if (response->headerSize >= 4
            && memcmp(response->headers + response->headerSize - 4, "\r\n\r\n", 4) == 0) {
            response->headers[response->headerSize] = 0;
            if (WtHttp_Headers(response, capacity) != 0) {
                return -1;
            }
            response->headersComplete = 1;
        }
    }
    if (!response->headersComplete) {
        return 0;
    }
    if (size - i > response->contentLength - response->bodySize) {
        return -1;
    }
    memcpy(body + response->bodySize, data + i, size - i);
    response->bodySize += size - i;
    response->complete = response->bodySize == response->contentLength;
    return response->complete;
}

static void WtHttp_Clear(void *pointer, size_t size)
{
    volatile unsigned char *bytes = pointer;
    size_t i;
    for (i = 0; i < size; i++) bytes[i] = 0;
}

static int WtHttp_AlphaNumeric(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static int WtHttp_Authority(const char *authority)
{
    size_t size = 0, digits = 0;
    unsigned port = 0;
    if (!authority || !WtHttp_AlphaNumeric((unsigned char)authority[0])) return 0;
    while (size <= 253 && authority[size] && authority[size] != ':') {
        if (!WtHttp_AlphaNumeric((unsigned char)authority[size]) && authority[size] != '.' && authority[size] != '-') return 0;
        size++;
    }
    if (size > 253 || !WtHttp_AlphaNumeric((unsigned char)authority[size - 1])) return 0;
    if (authority[size] == 0) return 1;
    for (size++; digits < 5 && authority[size] >= '0' && authority[size] <= '9'; size++, digits++)
        port = port * 10 + (unsigned)(authority[size] - '0');
    return digits > 0 && port > 0 && port <= 65535 && authority[size] == 0;
}

static int WtHttp_Write(WtHttpIO io, const unsigned char *bytes, size_t size)
{
    size_t sent = 0;
    while (sent < size) {
        int result = io.write(io.arg, bytes + sent, size - sent);
        if (result > 0) {
            if ((size_t)result > size - sent) return -1;
            sent += (size_t)result;
        } else if ((result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
            || !io.wait(io.arg, result)) return -1;
    }
    return 0;
}

int WtHttp_Exchange(WtHttpExchange *exchange, WtHttpIO io, const char *authority,
    const unsigned char *request, size_t requestSize)
{
    int length, result;
    if (!exchange) return -1;
    WtHttp_Clear(exchange, sizeof(*exchange));
    if (!io.write || !io.read || !io.wait || !WtHttp_Authority(authority)
        || ((request == NULL) != (requestSize == 0)) || requestSize > WT_HTTP_BODY_MAX) return -1;
    if (requestSize) {
        length = snprintf((char *)exchange->headers, sizeof(exchange->headers),
            "POST /wt/v1 HTTP/1.1\r\nHost: %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\nConnection: close\r\n\r\n",
            authority, (unsigned)requestSize);
    } else {
        length = snprintf((char *)exchange->headers, sizeof(exchange->headers),
            "GET /v1/ping HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", authority);
    }
    if (length <= 0 || (size_t)length >= sizeof(exchange->headers)
        || WtHttp_Write(io, exchange->headers, (size_t)length) != 0
        || WtHttp_Write(io, request, requestSize) != 0) goto failed;
    WtHttp_Init(&exchange->response);
    for (;;) {
        result = io.read(io.arg, exchange->input, sizeof(exchange->input));
        if (result > 0) {
            if ((size_t)result > sizeof(exchange->input) || exchange->response.complete
                || WtHttp_Feed(&exchange->response, exchange->input, (size_t)result,
                    exchange->body, sizeof(exchange->body)) < 0) goto failed;
        } else if (result == 0 || result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            if (!exchange->response.complete) goto failed;
            exchange->bodySize = exchange->response.bodySize;
            return 0;
        } else if ((result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE)
            || !io.wait(io.arg, result)) goto failed;
    }
failed:
    WtHttp_Clear(exchange, sizeof(*exchange));
    return -1;
}
