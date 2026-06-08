#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ── OS integration ───────────────────────────────────────────────────────── */
#define NO_SYS                          0   /* FreeRTOS mode */
#define LWIP_SOCKET                     1
#define LWIP_NETCONN                    0

/* Make lwIP's err_t identical to the firmware's err_t (board/err.h, int32_t),
 * so the two typedefs coexist in the Wi-Fi transport TUs that include both.
 * Must be a signed type. */
#define LWIP_ERR_T                      int32_t

/* The toolchain's <sys/time.h> already defines struct timeval; use it instead
 * of lwIP's private definition (avoids a redefinition in lwip/sockets.h). */
#define LWIP_TIMEVAL_PRIVATE            0

/* ── Memory ───────────────────────────────────────────────────────────────── */
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   4
#define MEM_SIZE                        (16 * 1024)
#define MEMP_NUM_TCP_PCB                8
#define MEMP_NUM_TCP_PCB_LISTEN         2
#define MEMP_NUM_TCP_SEG                32   /* >= TCP_SND_QUEUELEN (lwIP sanity) */
#define MEMP_NUM_PBUF                   16
#define MEMP_NUM_UDP_PCB                4
#define PBUF_POOL_SIZE                  24
#define PBUF_POOL_BUFSIZE               1500

/* ── TCP ──────────────────────────────────────────────────────────────────── */
#define LWIP_TCP                        1
#define TCP_TTL                         255
#define TCP_WND                         (8 * 1024)
#define TCP_MSS                         1460
#define TCP_SND_BUF                     (8 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * TCP_SND_BUF) / TCP_MSS)
#define LWIP_TCP_KEEPALIVE              1

/* ── UDP / DHCP / DNS ─────────────────────────────────────────────────────── */
#define LWIP_UDP                        1
#define LWIP_DHCP                       1
#define LWIP_DNS                        1
#define DNS_TABLE_SIZE                  4

/* ── mDNS ─────────────────────────────────────────────────────────────────── */
#define LWIP_MDNS_RESPONDER             1
#define MDNS_MAX_SERVICES               2

/* ── TLS / mbedTLS ────────────────────────────────────────────────────────── */
#define LWIP_ALTCP                      1
#define LWIP_ALTCP_TLS                  1
#define LWIP_ALTCP_TLS_MBEDTLS          1

/* ── Checksums ────────────────────────────────────────────────────────────── */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1

/* ── Debug (disabled in release; override per-component as needed) ────────── */
#define LWIP_DEBUG                      0
#define TCP_DEBUG                       LWIP_DBG_OFF
#define ETHARP_DEBUG                    LWIP_DBG_OFF
#define PBUF_DEBUG                      LWIP_DBG_OFF
#define IP_DEBUG                        LWIP_DBG_OFF
#define TCPIP_DEBUG                     LWIP_DBG_OFF
#define DHCP_DEBUG                      LWIP_DBG_OFF

/* ── Thread safety ────────────────────────────────────────────────────────── */
#define LWIP_TCPIP_CORE_LOCKING         1

/* ── Miscellaneous ────────────────────────────────────────────────────────── */
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_STATS                      0
#define LWIP_IPV6                       0

#endif /* LWIPOPTS_H */
