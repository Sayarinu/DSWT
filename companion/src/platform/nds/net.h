#ifndef COMPANION_PLATFORM_NDS_NET_H
#define COMPANION_PLATFORM_NDS_NET_H

#include <stddef.h>
#include <stdint.h>

#include "wonder_trade/http.h"

/* One POST /wt/v1 exchange over DSWiFi's lwIP sockets: a non-blocking TCP
 * connection to the configured raw IPv4 address (dswt-connection.h), the
 * pinned TLS client with the configured hostname as SNI (wonder_trade/tls.h),
 * and the bounded HTTP exchange (wonder_trade/http.h). Waits yield to other
 * cothreads, so run this on the worker (platform/nds/worker.h). The companion
 * never uses GET /v1/ping.
 *
 * TLS needs Mbed TLS's allocator and NV seed set up first; the worker does
 * that. The caller decodes the reply with WtWire_Decode. */

#define CP_NET_CONNECT_MS 10000
#define CP_NET_TLS_MS 30000
#define CP_NET_HTTP_MS 15000
/* A connection that moves no bytes for this long is abandoned. */
#define CP_NET_STALL_MS 10000

enum CpNetError {
    CP_NET_ERROR_ARGUMENT = -70,
    CP_NET_ERROR_SOCKET = -71,
    CP_NET_ERROR_CONNECT = -72,
    CP_NET_ERROR_TIMEOUT = -73,
    CP_NET_ERROR_CANCELLED = -74,
    CP_NET_ERROR_TLS = -75,
    CP_NET_ERROR_HTTP = -76,
    CP_NET_ERROR_NOT_CONNECTED = -77
};

typedef struct CpNetExchange {
    /* Results: the reply body (exchange.body, exchange.bodySize) and timings. */
    WtHttpExchange exchange;
    uint32_t connectMs, handshakeMs, httpMs;
    /* The failing Mbed TLS or socket error, for diagnostics. */
    int detail;
    /* Whether SO_LINGER {1, 0} was accepted for an abortive close. */
    int lingerSet;
    const char *stage;
} CpNetExchange;

/* POST the request frame and keep the reply in exchange. cancel, when set,
 * is polled while waiting. Zero on success, otherwise enum CpNetError. */
int CpNet_Post(CpNetExchange *, const unsigned char *request, size_t size, const volatile int *cancel);

#endif
