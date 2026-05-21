#define LOG_TAG "TLS"
#include "tls_context.h"

#include "log.h"
#include "err.h"
#include "storage.h"

#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cache.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"

#include <string.h>
#include <stdint.h>
#include <stddef.h>

/* ── allowed cipher suites ──────────────────────────────────────────── */
/* ECDHE-ECDSA-AES128-GCM-SHA256 and ECDHE-ECDSA-AES256-GCM-SHA384 only.
 * No RSA, no CBC, no SHA-1.  List is NULL-terminated. */
static const int g_ciphersuite_list[] = {
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
    MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
    0
};

/* ── PRNG personalisation string ────────────────────────────────────── */
static const uint8_t g_drbg_pers[] = "rmms-sensor-module-v1";

/* ── mbedTLS BIO callbacks ──────────────────────────────────────────── */

/* Called by mbedTLS to send data on the underlying transport.
 * Return value: bytes sent (>0), or MBEDTLS_ERR_SSL_WANT_WRITE if the
 * transport buffer is temporarily full. */
static int ssl_send(void *ctx_v, const unsigned char *buf, size_t len)
{
    TlsContext *ctx = (TlsContext *)ctx_v;
    if (!ctx->transport || !ctx->transport->write) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    int rc = ctx->transport->write(ctx->transport->ctx, buf, len);
    if (rc == (int)ERR_IO) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    if (rc == 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return rc;
}

/* Called by mbedTLS to receive data from the underlying transport.
 * Returns bytes read (>0), MBEDTLS_ERR_SSL_WANT_READ on timeout/EAGAIN,
 * or MBEDTLS_ERR_NET_RECV_FAILED on hard error. */
static int ssl_recv(void *ctx_v, unsigned char *buf, size_t len)
{
    TlsContext *ctx = (TlsContext *)ctx_v;
    if (!ctx->transport || !ctx->transport->read) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    /* Use a generous timeout so TLS handshake doesn't time out internally;
     * the outer FSM controls the 8-second budget via task deadline. */
    int rc = ctx->transport->read(ctx->transport->ctx, buf, len, 500u);
    if (rc > 0) {
        return rc;
    }
    if (rc == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* Timeout-aware recv: same as ssl_recv but with explicit timeout (ms).
 * mbedTLS uses this variant when MBEDTLS_SSL_PROTO_DTLS is not set but the
 * net context supports timeouts; we provide it for completeness. */
static int ssl_recv_timeout(void *ctx_v, unsigned char *buf, size_t len,
                             uint32_t timeout_ms)
{
    TlsContext *ctx = (TlsContext *)ctx_v;
    if (!ctx->transport || !ctx->transport->read) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    int rc = ctx->transport->read(ctx->transport->ctx, buf, len, timeout_ms);
    if (rc > 0)  return rc;
    if (rc == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

/* ── post-handshake stream_t vtable ─────────────────────────────────── */
/* After the handshake the TlsContext itself acts as a stream_t so the MQTT
 * client can write/read plaintext bytes through it. */

static int tls_stream_read(void *ctx_v, uint8_t *buf, size_t len,
                            uint32_t timeout_ms)
{
    TlsContext *ctx = (TlsContext *)ctx_v;
    (void)timeout_ms; /* mbedTLS timeout is set on the BIO receive callback */
    int rc = mbedtls_ssl_read(&ctx->ssl, buf, len);
    if (rc > 0)  return rc;
    if (rc == MBEDTLS_ERR_SSL_WANT_READ ||
        rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return 0; /* EAGAIN */
    }
    LOG_W("ssl_read error: -0x%04x", (unsigned)(-rc));
    return (int)ERR_TLS;
}

static int tls_stream_write(void *ctx_v, const uint8_t *buf, size_t len)
{
    TlsContext *ctx = (TlsContext *)ctx_v;
    size_t written = 0u;
    while (written < len) {
        int rc = mbedtls_ssl_write(&ctx->ssl, buf + written, len - written);
        if (rc > 0) {
            written += (size_t)rc;
        } else if (rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
                   rc == MBEDTLS_ERR_SSL_WANT_READ) {
            /* Retry — mbedTLS needs another write call with the same data.
             * This is safe because the data pointer and length are unchanged
             * (mbedTLS buffers the record internally on WANT_WRITE). */
            continue;
        } else {
            LOG_W("ssl_write error: -0x%04x", (unsigned)(-rc));
            return (int)ERR_TLS;
        }
    }
    return (int)len;
}

static void tls_stream_close_cb(void *ctx_v)
{
    TlsContext *ctx = (TlsContext *)ctx_v;
    /* Best-effort close_notify; ignore return value as we're tearing down. */
    (void)mbedtls_ssl_close_notify(&ctx->ssl);
    if (ctx->transport && ctx->transport->close) {
        ctx->transport->close(ctx->transport->ctx);
    }
    ctx->handshake_done = false;
}

/* ── helper: log mbedTLS errors ─────────────────────────────────────── */
static void log_mbedtls_err(const char *op, int rc)
{
    char errbuf[128];
    mbedtls_strerror(rc, errbuf, sizeof(errbuf));
    LOG_E("%s failed: -0x%04x (%s)", op, (unsigned)(-rc), errbuf);
}

/* ── public API ─────────────────────────────────────────────────────── */

err_t tls_context_init(TlsContext *ctx, const Identity *id)
{
    if (!ctx || !id) return ERR_INVALID_ARG;

    memset(ctx, 0, sizeof(*ctx));

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->conf);
    mbedtls_x509_crt_init(&ctx->ca_crt);
    mbedtls_x509_crt_init(&ctx->dev_crt);
    mbedtls_pk_init(&ctx->dev_key);
    mbedtls_ctr_drbg_init(&ctx->drbg);
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ssl_session_init(&ctx->session);

    /* Seed the PRNG. */
    int rc = mbedtls_ctr_drbg_seed(&ctx->drbg, mbedtls_entropy_func,
                                    &ctx->entropy,
                                    g_drbg_pers, sizeof(g_drbg_pers) - 1u);
    if (rc != 0) {
        log_mbedtls_err("ctr_drbg_seed", rc);
        goto fail;
    }

    /* Load project CA. */
    rc = mbedtls_x509_crt_parse_der(&ctx->ca_crt, id->ca_der, id->ca_len);
    if (rc != 0) {
        log_mbedtls_err("x509_crt_parse(CA)", rc);
        goto fail;
    }

    /* Load device certificate. */
    rc = mbedtls_x509_crt_parse_der(&ctx->dev_crt,
                                     id->dev_crt_der, id->dev_crt_len);
    if (rc != 0) {
        log_mbedtls_err("x509_crt_parse(dev)", rc);
        goto fail;
    }

    /* Load device private key. */
    rc = mbedtls_pk_parse_key(&ctx->dev_key,
                               id->dev_key_der, id->dev_key_len,
                               NULL, 0u,
                               mbedtls_ctr_drbg_random, &ctx->drbg);
    if (rc != 0) {
        log_mbedtls_err("pk_parse_key", rc);
        goto fail;
    }

    /* TLS configuration: TLS 1.2 minimum, client mode. */
    rc = mbedtls_ssl_config_defaults(&ctx->conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        log_mbedtls_err("ssl_config_defaults", rc);
        goto fail;
    }

    /* Minimum TLS version: 1.2. */
    mbedtls_ssl_conf_min_tls_version(&ctx->conf, MBEDTLS_SSL_VERSION_TLS1_2);

    /* Restrict to approved cipher suites only. */
    mbedtls_ssl_conf_ciphersuites(&ctx->conf, g_ciphersuite_list);

    /* Always verify server certificate against project CA — no bypasses. */
    mbedtls_ssl_conf_authmode(&ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&ctx->conf, &ctx->ca_crt, NULL);

    /* Present device certificate (mTLS). */
    rc = mbedtls_ssl_conf_own_cert(&ctx->conf, &ctx->dev_crt, &ctx->dev_key);
    if (rc != 0) {
        log_mbedtls_err("ssl_conf_own_cert", rc);
        goto fail;
    }

    /* PRNG. */
    mbedtls_ssl_conf_rng(&ctx->conf, mbedtls_ctr_drbg_random, &ctx->drbg);

    /* Session tickets enabled for fast transport-swap resumption (RFC 5077). */
    mbedtls_ssl_conf_session_tickets(&ctx->conf,
                                      MBEDTLS_SSL_SESSION_TICKETS_ENABLED);

    /* Disable renegotiation — tear down and re-establish instead. */
    mbedtls_ssl_conf_renegotiation(&ctx->conf,
                                    MBEDTLS_SSL_RENEGOTIATION_DISABLED);

    /* Set up the SSL context with this config. */
    rc = mbedtls_ssl_setup(&ctx->ssl, &ctx->conf);
    if (rc != 0) {
        log_mbedtls_err("ssl_setup", rc);
        goto fail;
    }

    LOG_I("TLS context initialised (ECDSA P-256, TLS 1.2+, GCM only)");
    return ERR_OK;

fail:
    tls_context_free(ctx);
    return ERR_TLS;
}

err_t tls_context_handshake(TlsContext *ctx, stream_t *transport)
{
    if (!ctx || !transport) return ERR_INVALID_ARG;

    ctx->transport      = transport;
    ctx->handshake_done = false;

    /* Read SNI hostname from /cfg/broker.json.
     * The hostname is the first null-terminated field (see transport_wifi.c). */
    static char sni_host[128];
    {
        static uint8_t broker_buf[128];
        size_t blen = 0u;
        err_t err = storage_read("/cfg/broker.json", broker_buf,
                                  sizeof(broker_buf) - 1u, &blen);
        if (err == ERR_OK && blen > 0u) {
            broker_buf[blen] = '\0';
            size_t hlen = strnlen((const char *)broker_buf,
                                  sizeof(sni_host) - 1u);
            memcpy(sni_host, broker_buf, hlen);
            sni_host[hlen] = '\0';
        } else {
            LOG_W("cannot read broker hostname for SNI; using empty string");
            sni_host[0] = '\0';
        }
    }

    if (sni_host[0] != '\0') {
        int rc = mbedtls_ssl_set_hostname(&ctx->ssl, sni_host);
        if (rc != 0) {
            log_mbedtls_err("ssl_set_hostname", rc);
            return ERR_TLS;
        }
        LOG_D("TLS SNI: %s", sni_host);
    }

    /* Install BIO callbacks delegating to the stream_t transport. */
    mbedtls_ssl_set_bio(&ctx->ssl, ctx,
                        ssl_send, ssl_recv, ssl_recv_timeout);

    /* If we have a saved session ticket, offer it for resumption. */
    {
        int rc = mbedtls_ssl_set_session(&ctx->ssl, &ctx->session);
        if (rc != 0) {
            /* Non-fatal: full handshake will proceed. */
            LOG_D("no saved session to offer (rc=-0x%04x)", (unsigned)(-rc));
        }
    }

    /* Run the handshake. */
    int rc;
    do {
        rc = mbedtls_ssl_handshake(&ctx->ssl);
    } while (rc == MBEDTLS_ERR_SSL_WANT_READ ||
             rc == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (rc != 0) {
        log_mbedtls_err("ssl_handshake", rc);
        return ERR_TLS;
    }

    /* Verify peer certificate — belt-and-suspenders; VERIFY_REQUIRED already
     * makes mbedTLS abort the handshake on cert failure, but we log here. */
    uint32_t flags = mbedtls_ssl_get_verify_result(&ctx->ssl);
    if (flags != 0u) {
        char vbuf[256];
        mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "  ! ", flags);
        LOG_E("cert verification failed: %s", vbuf);
        return ERR_TLS;
    }

    /* Fill tls_stream so MQTT client can use this context as a stream_t. */
    ctx->tls_stream.read  = tls_stream_read;
    ctx->tls_stream.write = tls_stream_write;
    ctx->tls_stream.close = tls_stream_close_cb;
    ctx->tls_stream.ctx   = ctx;

    ctx->handshake_done = true;
    LOG_I("TLS handshake complete (%s)", mbedtls_ssl_get_ciphersuite(&ctx->ssl));
    return ERR_OK;
}

err_t tls_context_save_session(TlsContext *ctx)
{
    if (!ctx || !ctx->handshake_done) return ERR_NOT_INIT;

    mbedtls_ssl_session_free(&ctx->session);
    mbedtls_ssl_session_init(&ctx->session);

    int rc = mbedtls_ssl_get_session(&ctx->ssl, &ctx->session);
    if (rc != 0) {
        log_mbedtls_err("ssl_get_session", rc);
        return ERR_TLS;
    }
    LOG_D("TLS session saved for resumption");
    return ERR_OK;
}

void tls_context_free(TlsContext *ctx)
{
    if (!ctx) return;
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->conf);
    mbedtls_x509_crt_free(&ctx->ca_crt);
    mbedtls_x509_crt_free(&ctx->dev_crt);
    mbedtls_pk_free(&ctx->dev_key);
    mbedtls_ctr_drbg_free(&ctx->drbg);
    mbedtls_entropy_free(&ctx->entropy);
    mbedtls_ssl_session_free(&ctx->session);
    memset(ctx, 0, sizeof(*ctx));
}
