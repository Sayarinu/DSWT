#ifndef WONDER_TRADE_TLS_H
#define WONDER_TRADE_TLS_H

#include <stddef.h>
#include <stdint.h>
#include <mbedtls/ssl.h>

#define WT_TLS_HEAP_SIZE 65536
#define WT_TLS_PIN_SIZE 32

/* One active connection: PSA and the allocator/seed callbacks are process-wide. */
typedef struct WtTls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    unsigned char pins[2][WT_TLS_PIN_SIZE];
    int pinned;
    int ready;
} WtTls;

int WtTls_Open(WtTls *tls, const char *hostname, const unsigned char pins[2][32],
    void *bio, mbedtls_ssl_send_t *send, mbedtls_ssl_recv_t *receive);
int WtTls_Handshake(WtTls *tls);
int WtTls_Write(WtTls *tls, const unsigned char *data, size_t size);
int WtTls_Read(WtTls *tls, unsigned char *data, size_t size);
void WtTls_Close(WtTls *tls);

#endif
