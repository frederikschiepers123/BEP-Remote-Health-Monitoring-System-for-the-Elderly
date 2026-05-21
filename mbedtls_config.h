#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

/* ── Protocol versions ────────────────────────────────────────────────────── */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_TLS1_3

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
#define MBEDTLS_NET_C       /* only used on TCP path; CDC path has custom BIO */

/* Session tickets (RFC 5077) for fast resumption on transport swap. */
#define MBEDTLS_SSL_SESSION_TICKETS

/* Disable renegotiation — sessions are torn down and re-established. */
#undef MBEDTLS_SSL_RENEGOTIATION

/* ── RNG ──────────────────────────────────────────────────────────────────── */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
/* RP2350 has a hardware TRNG; wire it via mbedtls_entropy_add_source. */
#define MBEDTLS_NO_PLATFORM_ENTROPY   /* we supply our own source */

/* ── Misc ─────────────────────────────────────────────────────────────────── */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_MD_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_ERROR_C   /* human-readable error strings */

/* Memory footprint tuning for RP2350. */
#define MBEDTLS_MPI_MAX_SIZE    48   /* 384 bits — enough for P-256 */
#define MBEDTLS_SSL_MAX_CONTENT_LEN 4096

/* ── Validation ───────────────────────────────────────────────────────────── */
#include "mbedtls/check_config.h"

#endif /* MBEDTLS_CONFIG_H */
