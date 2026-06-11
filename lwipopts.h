#ifndef LWIPOPTS_H
#define LWIPOPTS_H

/* ── OS integration ───────────────────────────────────────────────────────── */
/* NO_SYS=1 + pico_cyw43_arch_lwip_threadsafe_background — the hardware-proven
 * model (the bring-up runs the full Wi-Fi + mTLS + MQTT + sensors path on it).
 * The NO_SYS=0 / sys_freertos path got cyw43_arch_init working (the mailbox
 * fix below, kept in history) but cyw43_arch_wifi_connect then DEADLOCKED on
 * this pico-sdk rev — cyw43 is not SMP-clean here. So the firmware uses
 * threadsafe_background and pins every task to core 0 (app_main.c); lwIP is
 * serviced by the cyw43 background poll, guarded by cyw43_arch_lwip_begin/end.
 * No tcpip thread, no mailboxes, no core locking. */
#define NO_SYS                          1
#define LWIP_SOCKET                     0
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
/* The default (~8) is too small once altcp_tls (mbedTLS) + the lwIP MQTT raw
 * API + the cyw43/DHCP timers all run: lwIP panics mid-TLS-handshake with
 * "sys_timeout: pool MEMP_SYS_TIMEOUT is empty" (proven during bring-up —
 * see memory project-mtls-mqtt-proven). The transport_task uses the exact
 * same altcp_tls + lwIP-MQTT path, so the production lwipopts needs it too. */
#define MEMP_NUM_SYS_TIMEOUT            24
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

/* ── mDNS — resolver only (we query *.local, we do NOT advertise) ──────────
 * The single load-bearing flag is LWIP_DNS_SUPPORT_MDNS_QUERIES: it makes
 * dns_gethostbyname() route any "*.local" name to a multicast query at
 * 224.0.0.251:5353 instead of to the unicast DNS server. lwIP issues that
 * query from an ephemeral UDP source port, so RFC 6762 §6.7-compliant
 * responders (avahi, Bonjour) reply by *unicast* back to that port — the
 * response lands on our own unicast MAC, so no IGMP / multicast-MAC-filter
 * setup is needed for the common case. resolve_broker_host() needs no code
 * change; it already calls dns_gethostbyname (sensors_publish.c).
 *
 * LWIP_MDNS_RESPONDER is the *advertise* side (apps/mdns). We do not link it
 * (no target pulls pico_lwip_mdns), so it is off. Do not confuse the two:
 * the responder does not let us resolve names, and turning it on would need
 * LWIP_IGMP=1 just to compile (mdns.c #error). See CLAUDE.md §8.2.            */
#define LWIP_DNS_SUPPORT_MDNS_QUERIES   1
#define LWIP_MDNS_RESPONDER             0

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
#define LWIP_TCPIP_CORE_LOCKING         0   /* NO_SYS=1: no tcpip thread to lock */

/* ── Miscellaneous ────────────────────────────────────────────────────────── */
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_STATS                      0
#define LWIP_IPV6                       0

#endif /* LWIPOPTS_H */
