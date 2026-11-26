#include "wonder_trade/tls.h"

#include <string.h>
#include <mbedtls/platform.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

static int WtTls_CheckPin(const WtTls *tls, const mbedtls_x509_crt *cert)
{
    unsigned char digest[32];
    size_t size;
    int matches;

    if (psa_hash_compute(PSA_ALG_SHA_256, cert->pk_raw.p, cert->pk_raw.len,
            digest, sizeof(digest), &size) != PSA_SUCCESS || size != sizeof(digest)) {
        return 0;
    }
    matches = memcmp(digest, tls->pins[0], sizeof(digest)) == 0
        || memcmp(digest, tls->pins[1], sizeof(digest)) == 0;
    return matches;
}

static int WtTls_TrustPinnedKey(void *arg, const mbedtls_x509_crt *cert, mbedtls_x509_crt **trusted)
{
    const WtTls *tls = arg;
    int result;

    *trusted = NULL;
    if (!WtTls_CheckPin(tls, cert)) {
        return 0;
    }
    *trusted = mbedtls_calloc(1, sizeof(**trusted));
    if (*trusted == NULL) {
        return MBEDTLS_ERR_X509_ALLOC_FAILED;
    }
    mbedtls_x509_crt_init(*trusted);
    result = mbedtls_x509_crt_parse_der(*trusted, cert->raw.p, cert->raw.len);
    if (result != 0) {
        mbedtls_x509_crt_free(*trusted);
        mbedtls_free(*trusted);
        *trusted = NULL;
    }
    return result;
}

static int WtTls_VerifyLeaf(void *arg, mbedtls_x509_crt *cert, int depth, uint32_t *flags)
{
    WtTls *tls = arg;

    /* The callback can see the leaf and its identical local trust anchor. */
    if (depth > 1 || !WtTls_CheckPin(tls, cert)) {
        return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
    }
    if (depth == 0) {
        tls->pinned = *flags == 0;
    }
    return 0;
}

int WtTls_Open(WtTls *tls, const char *hostname, const unsigned char pins[2][32],
    void *bio, mbedtls_ssl_send_t *send, mbedtls_ssl_recv_t *receive)
{
    int result;

    memset(tls, 0, sizeof(*tls));
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->config);
    memcpy(tls->pins, pins, sizeof(tls->pins));
    if (psa_crypto_init() != PSA_SUCCESS) {
        return MBEDTLS_ERR_SSL_NO_RNG;
    }
    result = mbedtls_ssl_config_defaults(&tls->config, MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        return result;
    }
    mbedtls_ssl_conf_min_tls_version(&tls->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&tls->config, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_authmode(&tls->config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_cb(&tls->config, WtTls_TrustPinnedKey, tls);
    mbedtls_ssl_conf_verify(&tls->config, WtTls_VerifyLeaf, tls);
    result = mbedtls_ssl_setup(&tls->ssl, &tls->config);
    if (result == 0) {
        result = mbedtls_ssl_set_hostname(&tls->ssl, hostname);
    }
    if (result == 0) {
        mbedtls_ssl_set_bio(&tls->ssl, bio, send, receive, NULL);
    }
    return result;
}

int WtTls_Handshake(WtTls *tls)
{
    int result = mbedtls_ssl_handshake(&tls->ssl);

    if (result == 0) {
        if (!tls->pinned || mbedtls_ssl_get_verify_result(&tls->ssl) != 0) {
            return MBEDTLS_ERR_X509_CERT_VERIFY_FAILED;
        }
        tls->ready = 1;
    }
    return result;
}

int WtTls_Write(WtTls *tls, const unsigned char *data, size_t size)
{
    if (!tls->ready) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    return mbedtls_ssl_write(&tls->ssl, data, size);
}

int WtTls_Read(WtTls *tls, unsigned char *data, size_t size)
{
    if (!tls->ready) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    return mbedtls_ssl_read(&tls->ssl, data, size);
}

void WtTls_Close(WtTls *tls)
{
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->config);
    mbedtls_psa_crypto_free();
    mbedtls_platform_zeroize(tls, sizeof(*tls));
}
