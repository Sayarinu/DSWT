#include "wonder_trade/http.h"

#include <string.h>

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
            || WtHttp_Equal(line, colon - line, "content-encoding")) {
            return -1;
        } else if (WtHttp_Equal(line, colon - line, "content-type")) {
            if (typeFound++ || !WtHttp_Equal(value, valueEnd - value, "application/octet-stream")) {
                return -1;
            }
        }
        line = end + 2;
    }
    return lengthFound == 1 && typeFound == 1 ? 0 : -1;
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
