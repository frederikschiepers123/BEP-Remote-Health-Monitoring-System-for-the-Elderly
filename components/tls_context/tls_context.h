#ifndef TLS_CONTEXT_H
#define TLS_CONTEXT_H

/* Uniform mTLS context for both USB-CDC and Wi-Fi transports.
 *
 * Both transports use the same mbedTLS configuration, the same cert chain,
 * and the same cipher suites.  The only difference is the underlying
 * stream_t backend (CDC0 or lwIP TCP socket).
 *
 * Security constraints (CLAUDE.md §10.2):
 *   - ECDSA P-256 only.
 *   - TLS 1.2 minimum, TLS 1.3 preferred.
 *   - Cipher suites: ECDHE-ECDSA-AES128-GCM-SHA256 and
 *                    ECDHE-ECDSA-AES256-GCM-SHA384.
 *   - No RSA, no CBC, no SHA-1.
 *   - Server cert ALWAYS validated against project CA — no bypass,
 *     not even in debug builds.
 *   - SNI set from /cfg/broker.json hostname, never hardcoded.
 *   - Session tickets (RFC 5077) enabled for fast transport-swap resumption.
 *   - Renegotiation disabled.
 *
 * Lock order: TlsContext is not shared between tasks.  The transport task
 * owns it exclusively.
 */

#include "err.h"
#include "identity.h"

#include "mbedtls/ssl.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── stream_t abstraction ────────────────────────────────────────────── */

/* Abstract byte-stream interface.  Both CDC and TCP implement this vtable.
 * TlsContext wraps an underlying stream_t for the TLS BIO callbacks, and
 * after the handshake, exposes itself as a stream_t for MQTT. */
typedef struct {
    /* Read up to len bytes into buf.  timeout_ms = 0 means non-blocking.
     * Returns bytes read (>0), 0 on timeout/EAGAIN, or negative err_t. */
    int  (*read)(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms);

    /* Write exactly len bytes from buf.
     * Returns len on success or negative err_t on failure. */
    int  (*write)(void *ctx, const uint8_t *buf, size_t len);

    /* Close and release resources.  May be called at most once. */
    void (*close)(void *ctx);

    /* Opaque context pointer passed as the first argument to each callback. */
    void *ctx;
} stream_t;

/* ── TlsContext ──────────────────────────────────────────────────────── */

typedef struct {
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_x509_crt         ca_crt;    /* project CA */
    mbedtls_x509_crt         dev_crt;   /* device certificate */
    mbedtls_pk_context       dev_key;   /* device ECDSA P-256 key */
    mbedtls_ctr_drbg_context drbg;
    mbedtls_entropy_context  entropy;
    mbedtls_ssl_session      session;   /* saved session ticket for resumption */

    /* Underlying byte-transport set by tls_context_handshake(). */
    stream_t                *transport;

    /* After a successful handshake this context acts as a stream_t itself:
     * MQTT writes to tls_stream, which encrypts and writes to transport. */
    stream_t                 tls_stream;

    bool                     handshake_done;
} TlsContext;

/* Initialise a TlsContext: load certs/key from id, seed PRNG, configure
 * cipher suites and TLS version constraints.
 * Must be called once at boot (heap allowed here per CLAUDE.md §13.4). */
err_t tls_context_init(TlsContext *ctx, const Identity *id);

/* Run the TLS handshake over transport.  On success, ctx->tls_stream is
 * filled and ready for use by the MQTT client.
 * If a saved session ticket is present it is offered for fast resumption. */
err_t tls_context_handshake(TlsContext *ctx, stream_t *transport);

/* Save current session ticket for later resumption (call before close). */
err_t tls_context_save_session(TlsContext *ctx);

/* Free all mbedTLS resources.  ctx must not be used afterwards. */
void  tls_context_free(TlsContext *ctx);

#endif /* TLS_CONTEXT_H */
