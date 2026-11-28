#include "platform/nds/net.h"

#include <errno.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <mbedtls/ssl.h>

#include "platform/nds/wifi.h"
#include "wonder_trade/tls.h"

/* lwIP's struct linger, which DSWiFi's sys/socket.h does not declare. */
typedef struct CpLinger { int onoff, seconds; } CpLinger;

typedef struct Connection {
    WtTls tls;
    int fd;
    uint64_t deadline, lastProgress;
    const volatile int *cancel;
    int detail;
} Connection;

/* Kept off the worker stack, like the game's transport. One exchange at a time. */
static Connection sConnection = { .fd = -1 };
static int sVisitSupported;

static uint64_t Now(void) { return systemCounterGetTicks(); }
static uint32_t Since(uint64_t start) { return systemCounterTicksToMsec((u32)(Now() - start)); }
static uint64_t After(unsigned ms) { return Now() + systemCounterMsecsToTicks(ms); }

static int Continue(Connection *connection)
{
    uint64_t now = Now();
    return !(connection->cancel && *connection->cancel) && now < connection->deadline
        && now - connection->lastProgress < systemCounterMsecsToTicks(CP_NET_STALL_MS);
}

static int Send(void *arg, const unsigned char *bytes, size_t size)
{
    Connection *connection = arg;
    int count;
    if (!Continue(connection)) return MBEDTLS_ERR_SSL_TIMEOUT;
    count = send(connection->fd, bytes, size, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (count < 0) { connection->detail = errno; return MBEDTLS_ERR_SSL_INTERNAL_ERROR; }
    if (count > 0) connection->lastProgress = Now();
    return count;
}

static int Receive(void *arg, unsigned char *bytes, size_t size)
{
    Connection *connection = arg;
    int count;
    if (!Continue(connection)) return MBEDTLS_ERR_SSL_TIMEOUT;
    count = recv(connection->fd, bytes, size, 0);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_READ;
    if (count < 0) { connection->detail = errno; return MBEDTLS_ERR_SSL_INTERNAL_ERROR; }
    if (count > 0) connection->lastProgress = Now();
    return count;
}

/* Retry WANT_READ/WANT_WRITE until the deadline, letting lwIP and the UI run. */
static int Wait(void *arg, int result)
{
    Connection *connection = arg;
    if ((result != MBEDTLS_ERR_SSL_WANT_READ && result != MBEDTLS_ERR_SSL_WANT_WRITE) || !Continue(connection)) return 0;
    cothread_yield();
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

static int Connect(Connection *connection)
{
    struct sockaddr_in address;
    int one = 1;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)WONDER_TRADE_PORT);
    address.sin_addr.s_addr = htonl((uint32_t)WONDER_TRADE_IPV4);
    connection->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connection->fd < 0) { connection->detail = errno; return CP_NET_ERROR_SOCKET; }
    if (ioctl(connection->fd, FIONBIO, &one) != 0) { connection->detail = errno; return CP_NET_ERROR_SOCKET; }
    if (connect(connection->fd, (struct sockaddr *)&address, sizeof(address)) == 0) return 0;
    if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) { connection->detail = errno; return CP_NET_ERROR_CONNECT; }
    while (Continue(connection)) {
        fd_set writable;
        struct timeval poll = { 0, 0 };
        int ready, error = 0;
        socklen_t length = sizeof(error);
        FD_ZERO(&writable);
        FD_SET(connection->fd, &writable);
        ready = select(connection->fd + 1, NULL, &writable, NULL, &poll);
        if (ready < 0) { connection->detail = errno; return CP_NET_ERROR_CONNECT; }
        if (ready > 0) {
            if (getsockopt(connection->fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0) error = errno;
            connection->detail = error;
            return error == 0 ? 0 : CP_NET_ERROR_CONNECT;
        }
        cothread_yield();
    }
    return connection->cancel && *connection->cancel ? CP_NET_ERROR_CANCELLED : CP_NET_ERROR_TIMEOUT;
}

/* After a complete exchange the server has already closed its side. An
 * abortive close (SO_LINGER {1, 0}, a RST) would free lwIP's connection state
 * at once. DSWiFi 0.4.2 builds lwIP without SO_LINGER, so setsockopt fails and
 * this is an ordinary close; the result is reported (lingerSet) so a later
 * DSWiFi that accepts it shows up. In melonDS the system heap returned to the
 * same level before every connection with the ordinary close. */
static int Close(Connection *connection, int complete)
{
    int set = 0;
    if (connection->fd < 0) return 0;
    if (complete) {
        CpLinger abort = { 1, 0 };
        set = setsockopt(connection->fd, SOL_SOCKET, SO_LINGER, &abort, sizeof(abort)) == 0;
    }
    closesocket(connection->fd);
    connection->fd = -1;
    return set;
}

void CpNet_EndVisit(void)
{
    if (sConnection.fd < 0) return;
    WtTls_Close(&sConnection.tls);
    Close(&sConnection, 1);
}

static int OpenTls(Connection *connection, CpNetExchange *result)
{
    static const unsigned char pins[2][32] = WONDER_TRADE_PINS;
    uint64_t start = Now();
    int status;
    result->stage = "connect";
    connection->lastProgress = start;
    connection->deadline = After(CP_NET_CONNECT_MS);
    status = Connect(connection);
    result->connectMs = Since(start);
    if (status != 0) {
        Close(connection, 0);
        return status;
    }

    result->stage = "tls";
    start = Now();
    connection->lastProgress = start;
    connection->deadline = After(CP_NET_TLS_MS);
    status = WtTls_Open(&connection->tls, WONDER_TRADE_HOSTNAME, pins, connection, Send, Receive);
    if (status == 0) {
        do { status = WtTls_Handshake(&connection->tls); } while (Wait(connection, status));
    }
    result->handshakeMs = Since(start);
    if (status == 0) return 0;
    connection->detail = status;
    WtTls_Close(&connection->tls);
    Close(connection, 0);
    return connection->cancel && *connection->cancel ? CP_NET_ERROR_CANCELLED : CP_NET_ERROR_TLS;
}

int CpNet_Post(CpNetExchange *result, const unsigned char *request, size_t size, const volatile int *cancel)
{
    Connection *connection = &sConnection;
    WtHttpIO io = { connection, WriteHttp, ReadHttp, Wait };
    if (!result || !request || !size) return CP_NET_ERROR_ARGUMENT;
    memset(result, 0, sizeof(*result));
    connection->cancel = cancel;
    if (!CpWifi_Address()) { result->stage = "wifi"; return CP_NET_ERROR_NOT_CONNECTED; }

    for (;;) {
        int reused = connection->fd >= 0;
        int requestedVisit = sVisitSupported;
        connection->detail = 0;
        if (!reused) {
            int status = OpenTls(connection, result);
            if (status != 0) { result->detail = connection->detail; return status; }
        }
        result->stage = "http";
        uint64_t start = Now();
        connection->lastProgress = start;
        connection->deadline = After(CP_NET_HTTP_MS);
        int status = WtHttp_ExchangeVisit(&result->exchange, io, WONDER_TRADE_AUTHORITY,
            request, size, requestedVisit);
        result->httpMs += Since(start);
        if (!status) {
            sVisitSupported = result->exchange.response.visitSupported;
            if (!result->exchange.response.keepAlive) {
                WtTls_Close(&connection->tls);
                result->lingerSet = Close(connection, 1);
            }
            result->stage = "done";
            result->detail = 0;
            return 0;
        }
        result->detail = status;
        CpNet_EndVisit();
        sVisitSupported = 0;
        if (cancel && *cancel) return CP_NET_ERROR_CANCELLED;
        /* A save can outlast an idle connection, or a gateway can revert
         * to close-only. Replay the journalled bytes once using close. */
        if (!reused && !requestedVisit) return CP_NET_ERROR_HTTP;
    }
}
