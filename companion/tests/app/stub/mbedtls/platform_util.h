#ifndef COMPANION_TEST_STUB_MBEDTLS_PLATFORM_UTIL_H
#define COMPANION_TEST_STUB_MBEDTLS_PLATFORM_UTIL_H

/* The one Mbed TLS function boot.c and recovery.c use; the session model
 * test defines it, so the host tests need no Mbed TLS build. */
#include <stddef.h>

void mbedtls_platform_zeroize(void *buffer, size_t size);

#endif
