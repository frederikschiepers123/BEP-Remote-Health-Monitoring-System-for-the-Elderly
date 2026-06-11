#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* Application-wide configuration constants.
 *
 * Task priorities, stack sizes, watchdog IDs, queue depths, and MQTT topic
 * roots.  All task and queue parameters are defined here so they appear in
 * one place and can be audited together.
 *
 * Priority note (CLAUDE.md §7.1):
 *   Higher numeric value = higher FreeRTOS priority.
 *   selector_task (6) > transport_task (5) > radar (4) > env/light (3) >
 *   app_main/ui (2).
 */

/* ── Task priorities ─────────────────────────────────────────────────────── */

#define TASK_PRI_APP_MAIN   2
#define TASK_PRI_ENV        3
#define TASK_PRI_AIR        3
#define TASK_PRI_LIGHT      3
#define TASK_PRI_RADAR      4
#define TASK_PRI_UI         2
#define TASK_PRI_TRANSPORT  5

/* ── Task stack sizes (in words = 4 bytes each) ──────────────────────────── */

#define TASK_STACK_APP_MAIN   512U    /*  2 KB */
#define TASK_STACK_ENV        512U    /*  2 KB */
#define TASK_STACK_AIR        512U    /*  2 KB */
#define TASK_STACK_RADAR      1024U   /*  4 KB */
#define TASK_STACK_LIGHT      512U    /*  2 KB */
#define TASK_STACK_UI         1024U   /*  4 KB */
/* transport_task runs cyw43 + lwIP + altcp_tls + mqtt; needs generous stack. */
#define TASK_STACK_TRANSPORT  4096U   /* 16 KB */

/* ── Wi-Fi association bring-up ───────────────────────────────────────────────
 * The cyw43 association must run with NO task that touches the
 * threadsafe_background async-context (lwIP/cyw43) contending it (the join
 * handshake's cyw43_poll() is serviced from a low-prio IRQ that only does work
 * when it can take the context lock uncontended — with the other Core-0 tasks
 * running it never completes and the connect hangs). transport_task is the only
 * cyw43-touching task during bring-up; it spawns the producer tasks afterwards
 * (app_main.c app_start_sensor_tasks, invoked via the transport_mqtt_init
 * callback). The ui task is network-inert (OLED over I²C only) so it runs
 * during bring-up — to render the diagnostic — without contending the context.
 *
 * cyw43_arch_wifi_connect_timeout_ms legitimately fails on transient conditions
 * (rc=-2 NONET: beacon not seen yet), so the connect is retried a bounded number
 * of times. rc=-7 (BADAUTH, wrong PSK) is terminal — we stop retrying on it. */
#define WIFI_CONNECT_ATTEMPTS       4U      /* total association attempts        */
#define WIFI_CONNECT_TIMEOUT_MS     30000U  /* per-attempt cyw43 connect timeout */
#define WIFI_CONNECT_RETRY_DELAY_MS 2000U   /* backoff between attempts          */

/* ── Watchdog task IDs ───────────────────────────────────────────────────── */

typedef enum {
    WDT_TASK_APP_MAIN = 0,
    WDT_TASK_ENV,
    WDT_TASK_AIR,
    WDT_TASK_RADAR,
    WDT_TASK_LIGHT,
    WDT_TASK_UI,
    WDT_TASK_TRANSPORT,
    WDT_TASK_COUNT      /* must be last */
} WdtTaskId;

/* ── FreeRTOS queue depths ───────────────────────────────────────────────── */

#define Q_ENV_DEPTH      4U
#define Q_AIR_DEPTH      4U
#define Q_RADAR_DEPTH    4U
#define Q_LIGHT_DEPTH    4U

/* ── MQTT topic root ─────────────────────────────────────────────────────── */

/* All topics: rmms/<uuid>/... — see CLAUDE.md §9.1 */
#define MQTT_TOPIC_ROOT  "rmms"

/* ── MQTT keepalive seconds ──────────────────────────────────────────────── */

/* Wi-Fi is the sole transport in v1 (ADR-0002 — USB-CDC dropped). */
#define MQTT_KEEPALIVE_WIFI  60U   /* seconds */

/* ── NV spool drain / delivery (ADR-0003) ────────────────────────────────────
 * Every sample is persisted to the flash spool and published QoS 1 from there;
 * a record is cleared only after its PUBACK. */

/* Outstanding unacked publishes during a drain. v1 = 1 (publish head, await
 * PUBACK, advance) → trivially correct strict FIFO. Raising this needs the
 * contiguous-prefix ack logic noted in ADR-0003; do not bump blindly. */
#define SPOOL_MAX_IN_FLIGHT     1U

/* Reserved upper bound on outstanding publishes for a future multi-in-flight
 * drain. v1 drains one record per pump (in-flight=1, event-driven in
 * transport_mqtt.c drain_step), so this is NOT yet wired in — kept as the knob
 * a windowed drain (with contiguous-prefix ack) would use. The transport loop
 * pumps faster while a backlog drains; the watchdog is kicked at the loop top. */
#define SPOOL_DRAIN_BURST       64U

/* How long to wait for a PUBACK before giving up on a record. On timeout the
 * transport drops the MQTT session (so a stale publish can't alias the next
 * record) and re-sends the head after reconnect. Must stay BELOW lwIP's
 * MQTT_REQ_TIMEOUT (30 s) so the firmware never re-publishes into a request lwIP
 * still has pending. */
#define PUB_ACK_TIMEOUT_MS      10000U

/* Transport housekeeping cadence (link health, reconnect, time-sync, OLED).
 * Ingest runs faster (the ~10 Hz pump) so producer queues never overflow. */
#define TRANSPORT_LOOP_MS       1000U

/* Radar is decimated to this interval on the way into the spool (its producer
 * runs at 10 Hz). Matches the ~1 Hz wire cadence and bounds flash wear. */
#define RADAR_SPOOL_INTERVAL_MS 1000U

#endif /* APP_CONFIG_H */
