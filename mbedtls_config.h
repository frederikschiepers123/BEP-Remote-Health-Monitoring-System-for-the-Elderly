#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* The pico-sdk's altcp_tls_mbedtls.c (lwIP↔mbedTLS glue) was written against
 * mbedTLS 2.x and reaches into fields that became private in mbedTLS 3.x
 * (mbedtls_ssl_context::out_left, mbedtls_ssl_session::start, ...). Without
 * this define the SDK's own glue fails to compile. Same workaround the SDK's
 * example projects use. */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* The SDK's altcp_tls_mbedtls.c session helper references
 * mbedtls_ssl_session::start, which only exists when MBEDTLS_HAVE_TIME is on.
 * Without this the SDK glue fails to compile. We don't rely on a real RTC
 * (see CLAUDE.md §16 Q6); the application provides mbedtls_ms_time() via
 * MBEDTLS_PLATFORM_MS_TIME_ALT, backed by pico's time_us_64(). */
#define MBEDTLS_PLATFORM_C   /* prerequisite for MS_TIME_ALT (and good hygiene) */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT

/* ── Protocol versions ────────────────────────────────────────────────────── */
/* TLS 1.2 only. CLAUDE.md §10.2 sets TLS 1.2 as the minimum and 1.3 as
 * "preferred", but pico-sdk 2.x's pico_mbedtls does NOT compile the TLS 1.3
 * implementation sources (ssl_tls13_{client,server,generic}.c) — only
 * ssl_tls13_keys.c — so enabling MBEDTLS_SSL_PROTO_TLS1_3 leaves the 1.3
 * handshake symbols undefined at link. To adopt 1.3 later, add those sources
 * to the mbedtls target, then re-enable the macro here. */
#define MBEDTLS_SSL_PROTO_TLS1_2

/* ── Key exchange — ECDHE-ECDSA only ─────────────────────────────────────── */
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED    /* P-256 only */
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED

/* Disable RSA and DHE entirely. */
#undef MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#undef MBEDTLS_KEY_EXCHANGE_DHE_RSA_ENABLED
#undef MBEDTLS_RSA_C
#undef MBEDTLS_PKCS1_V15
#undef MBEDTLS_PKCS1_V21

/* ── Cipher suites ────────────────────────────────────────────────────────── */
/* TLS 1.2: ECDHE-ECDSA-AES128-GCM-SHA256, ECDHE-ECDSA-AES256-GCM-SHA384   */
/* TLS 1.3: TLS_AES_128_GCM_SHA256, TLS_AES_256_GCM_SHA384                 */
#define MBEDTLS_AES_C
#define MBEDTLS_GCM_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C   /* SHA-384 is a subset */

/* No CBC, no RC4, no DES, no SHA-1 ciphers. */
#undef MBEDTLS_CIPHER_MODE_CBC
#undef MBEDTLS_ARC4_C
#undef MBEDTLS_DES_C

/* ── Certificates ─────────────────────────────────────────────────────────── */
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C

/* ── TLS core ─────────────────────────────────────────────────────────────── */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C   /* needed by lwIP altcp */
/* No MBEDTLS_NET_C: it is the POSIX BSD-socket BIO and does not build on the
 * Pico. Both transports drive mbedTLS through custom BIO callbacks (§8.3):
 * lwIP on the Wi-Fi path, TinyUSB CDC on the USB path. */

/* Session tickets (RFC 5077) for fast resumption on transport swap. */
#define MBEDTLS_SSL_SESSION_TICKETS

/* Disable renegotiation — sessions are torn down and re-established. */
#undef MBEDTLS_SSL_RENEGOTIATION

/* ── RNG ──────────────────────────────────────────────────────────────────── */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
/* No OS / platform entropy on bare metal — RP2350 has hardware RNG paths
 * (TRNG + ROSC mixed by pico_rand). MBEDTLS_ENTROPY_HARDWARE_ALT tells
 * mbedTLS to call our `mbedtls_hardware_poll()` (see mqtt_connect.c for the
 * bring-up implementation; promote to a proper component when this leaves
 * test/bringup). Without this, `mbedtls_ctr_drbg_seed` fails inside
 * `altcp_tls_create_config_client_2wayauth` with no actionable error. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* ── Misc ─────────────────────────────────────────────────────────────────── */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MD_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_ERROR_C   /* human-readable error strings */

/* Memory footprint tuning for RP2350. */
#define MBEDTLS_MPI_MAX_SIZE    48   /* 384 bits — enough for P-256 */
#define MBEDTLS_SSL_MAX_CONTENT_LEN 4096

/* NOTE: do not #include "mbedtls/check_config.h" here. mbedTLS's build_info.h
 * runs it after the config_adjust_*.h headers derive the *_CAN_* / PSA_WANT_*
 * capability macros. Including it from the user config runs it too early and
 * produces spurious "not all prerequisites" errors. */

#endif /* MBEDTLS_CONFIG_H */
