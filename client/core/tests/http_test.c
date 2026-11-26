#include "wonder_trade/http.h"

#include <assert.h>
#include <string.h>

static int parse(const char *input, size_t fragment, size_t capacity)
{
    WtHttpResponse response;
    unsigned char body[WT_HTTP_BODY_MAX];
    size_t i;
    size_t length = strlen(input);
    int result = 0;
    WtHttp_Init(&response);
    for (i = 0; i < length; i += fragment) {
        size_t count = length - i < fragment ? length - i : fragment;
        result = WtHttp_Feed(&response, (const unsigned char *)input + i, count, body, capacity);
        if (result < 0) {
            return result;
        }
    }
    return result;
}

int main(void)
{
    const char *valid = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc";
    const char *prefix = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\n";
    const char *invalid[] = {
        "Content-Length: 3\r\nContent-Length: 3\r\n\r\nabc",
        "Content-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\nabc",
        "Content-Length: 3\r\nContent-Encoding: gzip\r\n\r\nabc",
        "Content-Length: -1\r\n\r\nabc",
        "Content-Length: 99999999999999999999\r\n\r\nabc",
        "Content-Length: 4097\r\n\r\nabc",
        "Content-Length: 1\r\n\r\nabc",
        "Content-Length: 0\r\n\r\nabc",
        "Content-Length: 3x\r\n\r\nabc",
        "Content-Length: 3\r\nContent-Type: application/octet-stream\r\n\r\nabc",
        "Content-Length: 3\r\n folded: x\r\n\r\nabc",
        "Content-Length: 3\nignored: x\r\n\r\nabc",
    };
    char buffer[WT_HTTP_HEADER_MAX + 256];
    size_t i;
    for (i = 1; i <= strlen(valid); i++) {
        assert(parse(valid, i, 3) == 1);
        assert(parse(valid, i, 2) == -1);
    }
    for (i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        strcpy(buffer, prefix);
        strcat(buffer, invalid[i]);
        assert(parse(buffer, 1, WT_HTTP_BODY_MAX) == -1);
    }
    assert(parse("HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n", 1, WT_HTTP_BODY_MAX) == -1);
    assert(parse("HTTP/1.1 302 Found\r\nContent-Length: 0\r\n\r\n", 1, WT_HTTP_BODY_MAX) == -1);
    assert(parse("HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 0\r\n\r\n", 1, WT_HTTP_BODY_MAX) == -1);
    assert(parse("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 3\r\nConnection: close\r\n\r\nab", 1, WT_HTTP_BODY_MAX) == 0);
    strcpy(buffer, prefix);
    memset(buffer + strlen(prefix), 'x', WT_HTTP_HEADER_MAX);
    buffer[strlen(prefix) + WT_HTTP_HEADER_MAX] = 0;
    assert(parse(buffer, 1, WT_HTTP_BODY_MAX) == -1);
    return 0;
}
