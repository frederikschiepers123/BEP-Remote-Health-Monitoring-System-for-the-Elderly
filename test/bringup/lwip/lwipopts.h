/* lwipopts.h for the bringup_wifi target ONLY (CLAUDE.md §15 step 13).
 *
 * The real firmware uses pico_cyw43_arch_lwip_sys_freertos with the root
 * lwipopts.h (NO_SYS=0, sockets, core-locking). For this bring-up we use
 * pico_cyw43_arch_lwip_threadsafe_background instead — see the comment in
 * test/bringup/CMakeLists.txt for why. That arch wraps pico_lwip_nosys, which
 * needs NO_SYS=1 (no sys_arch.h is provided), and the socket / netconn / core-
 * locking layers must therefore be disabled.
 *
 * This file is on the bringup_wifi target's include path *before* the repo
 * root, so it shadows the root lwipopts.h for this target only. No other
 * target sees it.
 */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ── OS integration (bring-up: no FreeRTOS in the lwIP path) ─────────────── */
#define NO_SYS                          1
#define LWIP_SOCKET                     0
#define LWIP_NETCONN                    0
#define LWIP_TCPIP_CORE_LOCKING         0

/* Match the firmware's err_t (board/err.h, int32_t) so the typedefs coexist. */
#define LWIP_ERR_T                      int32_t

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

/* The default (~8) is too small once altcp_tls (mbedTLS) + lwIP MQTT raw API +
 * cyw43 threadsafe_background poller all run simultaneously. Without the bump
 * lwIP panics with "sys_timeout: pool MEMP_SYS_TIMEOUT is empty" mid-handshake. */
#define MEMP_NUM_SYS_TIMEOUT            24

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

/* ── mDNS — resolver only (we query *.local, we do NOT advertise) ──────────
 * Mirror of root lwipopts.h. LWIP_DNS_SUPPORT_MDNS_QUERIES makes
 * dns_gethostbyname() route "*.local" to a multicast query at
 * 224.0.0.251:5353; RFC 6762 §6.7 responders reply unicast to our ephemeral
 * source port, so no IGMP is needed. resolve_broker_host() is unchanged.
 * The responder (advertise) side stays off — not linked here either.        */
#define LWIP_DNS_SUPPORT_MDNS_QUERIES   1
#define LWIP_MDNS_RESPONDER             0

/* ── TLS / mbedTLS (linked but not exercised by this bring-up) ───────────── */
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

/* ── Debug ────────────────────────────────────────────────────────────────── */
#define LWIP_DEBUG                      0

/* ── Miscellaneous ────────────────────────────────────────────────────────── */
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_STATS                      0
#define LWIP_IPV6                       0

#endif /* LWIPOPTS_H */
