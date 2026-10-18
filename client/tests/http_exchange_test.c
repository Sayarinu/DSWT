#include "wonder_trade/http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <mbedtls/ssl.h>

typedef struct Peer {
    const unsigned char *input;
    size_t size, offset, fragment, written, writeFragment;
    unsigned char output[8192];
    int reads, writes, waits, waitLimit, block, failRead, failWrite, oversizedRead, oversizedWrite, zeroWrite, stall, closeResult;
} Peer;

static WtHttpExchange exchange;
static const char response[] = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc";

static int Write(void *arg, const unsigned char *bytes, size_t size)
{
    Peer *peer = arg;
    size_t count = size < peer->writeFragment ? size : peer->writeFragment;
    peer->writes++;
    if (peer->writes == peer->failWrite) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (peer->zeroWrite) return 0;
    if (peer->oversizedWrite) return (int)size + 1;
    if (peer->block && peer->writes % 2) return MBEDTLS_ERR_SSL_WANT_WRITE;
    assert(peer->written + count <= sizeof(peer->output));
    memcpy(peer->output + peer->written, bytes, count); peer->written += count;
    return (int)count;
}

static int Read(void *arg, unsigned char *bytes, size_t size)
{
    Peer *peer = arg;
    size_t count = peer->size - peer->offset;
    peer->reads++;
    if (peer->reads == peer->failRead) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (peer->oversizedRead) return (int)size + 1;
    if (peer->block && peer->reads % 2) return MBEDTLS_ERR_SSL_WANT_READ;
    if (peer->offset == peer->size) return peer->stall ? MBEDTLS_ERR_SSL_WANT_READ : peer->closeResult;
    if (count > peer->fragment) count = peer->fragment;
    if (count > size) count = size;
    memcpy(bytes, peer->input + peer->offset, count); peer->offset += count;
    return (int)count;
}

static int Wait(void *arg, int result)
{
    Peer *peer = arg;
    assert(result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE);
    return ++peer->waits <= peer->waitLimit;
}

static Peer Setup(const char *input)
{
    Peer peer;
    memset(&peer, 0, sizeof(peer)); peer.input = (const unsigned char *)input;
    peer.size = strlen(input); peer.fragment = 7; peer.writeFragment = 5; peer.waitLimit = 10000;
    return peer;
}

static int Run(Peer *peer, const char *authority, const unsigned char *body, size_t size)
{
    WtHttpIO io = { peer, Write, Read, Wait };
    return WtHttp_Exchange(&exchange, io, authority, body, size);
}

static void Cleared(void)
{
    const unsigned char *bytes = (const unsigned char *)&exchange;
    size_t i;
    for (i = 0; i < sizeof(exchange); i++) assert(bytes[i] == 0);
}

int main(void)
{
    const unsigned char body[] = { 0x57, 0, 0xff, 0x44, 0x53 };
    const char get[] = "GET /v1/ping HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    const char post[] = "POST /wt/v1 HTTP/1.1\r\nHost: localhost:8443\r\nContent-Type: application/octet-stream\r\nContent-Length: 5\r\nConnection: close\r\n\r\n";
    const char *badAuthority[] = { NULL, "", "-host", "host-", "host\r\nX: y", "user@host", "host/", "host name",
        "host:0", "host:65536", "host:999999", "host:-1", "host:", "host:1:2" };
    const char *badResponse[] = {
        "HTTP/1.1 503 Unavailable\r\nContent-Type: application/octet-stream\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 302 Found\r\nContent-Type: application/octet-stream\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 3\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 3\r\nConnection: keep-alive\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 3\r\nConnection: close\r\nConnection: close\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 4097\r\nConnection: close\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: 3\r\nConnection: close\r\n\r\nabc",
        "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 3\r\nConnection: close\r\n\r\nab",
    };
    char authority[260], extra[256];
    unsigned char maximum[4096];
    size_t fragment, i;
    Peer peer;
    WtHttpIO io;
    for (fragment = 1; fragment <= 128; fragment++) {
        peer = Setup(response); peer.fragment = fragment; peer.writeFragment = fragment; peer.block = 1;
        assert(Run(&peer, "localhost", NULL, 0) == 0);
        assert(exchange.bodySize == 3 && memcmp(exchange.body, "abc", 3) == 0);
        assert(peer.written == sizeof(get) - 1 && memcmp(peer.output, get, peer.written) == 0);
        peer = Setup(response); peer.fragment = fragment; peer.writeFragment = fragment; peer.block = 1;
        peer.closeResult = MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
        assert(Run(&peer, "localhost:8443", body, sizeof(body)) == 0);
        assert(peer.written == sizeof(post) - 1 + sizeof(body));
        assert(memcmp(peer.output, post, sizeof(post) - 1) == 0 && memcmp(peer.output + sizeof(post) - 1, body, sizeof(body)) == 0);
    }
    for (i = 0; i < sizeof(badAuthority) / sizeof(*badAuthority); i++) {
        peer = Setup(response); assert(Run(&peer, badAuthority[i], NULL, 0) != 0 && peer.written == 0); Cleared();
    }
    memset(authority, 'a', 253); strcpy(authority + 253, ":65535");
    peer = Setup(response); assert(Run(&peer, authority, NULL, 0) == 0);
    memset(authority, 'a', 254); authority[254] = 0;
    peer = Setup(response); assert(Run(&peer, authority, NULL, 0) != 0 && peer.written == 0); Cleared();
    for (i = 0; i < sizeof(badResponse) / sizeof(*badResponse); i++) {
        peer = Setup(badResponse[i]); assert(Run(&peer, "localhost", body, sizeof(body)) != 0); Cleared();
    }
    strcpy(extra, response); strcat(extra, "x");
    for (fragment = 1; fragment <= strlen(extra); fragment++) {
        peer = Setup(extra); peer.fragment = fragment;
        assert(Run(&peer, "localhost", NULL, 0) != 0); Cleared();
    }
    memset(maximum, 0xa5, sizeof(maximum)); peer = Setup(response);
    assert(Run(&peer, "localhost", maximum, sizeof(maximum)) == 0);
    assert(peer.written > 4096 && memcmp(peer.output + peer.written - 4096, maximum, 4096) == 0);
    peer = Setup(response); assert(Run(&peer, "localhost", maximum, 4097) != 0 && peer.written == 0); Cleared();
    peer = Setup(response); assert(Run(&peer, "localhost", NULL, 1) != 0 && peer.written == 0); Cleared();
    peer = Setup(response); assert(Run(&peer, "localhost", body, 0) != 0 && peer.written == 0); Cleared();
    peer = Setup(response); peer.failWrite = 2; assert(Run(&peer, "localhost", body, sizeof(body)) != 0); Cleared();
    peer = Setup(response); peer.zeroWrite = 1; assert(Run(&peer, "localhost", NULL, 0) != 0); Cleared();
    peer = Setup(response); peer.failRead = 2; assert(Run(&peer, "localhost", NULL, 0) != 0); Cleared();
    peer = Setup(response); peer.oversizedRead = 1; assert(Run(&peer, "localhost", NULL, 0) != 0); Cleared();
    peer = Setup(response); peer.oversizedWrite = 1; assert(Run(&peer, "localhost", NULL, 0) != 0); Cleared();
    peer = Setup(response); peer.block = 1; peer.waitLimit = 0; assert(Run(&peer, "localhost", NULL, 0) != 0); Cleared();
    peer = Setup(response); peer.stall = 1; peer.waitLimit = 1; assert(Run(&peer, "localhost", NULL, 0) != 0); Cleared();
    peer = Setup(response); peer.closeResult = MBEDTLS_ERR_SSL_CONN_EOF; assert(Run(&peer, "localhost", NULL, 0) != 0); Cleared();
    io.arg = &peer; io.read = Read; io.write = Write; io.wait = NULL;
    assert(WtHttp_Exchange(&exchange, io, "localhost", NULL, 0) != 0); Cleared();
    assert(WtHttp_Exchange(NULL, io, "localhost", NULL, 0) != 0);
    puts("Shared HTTP fragmentation, partial writes, closure, cancellation and bounds passed");
    return 0;
}
