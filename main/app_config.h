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
#define TASK_PRI_LIGHT      3
#define TASK_PRI_RADAR      4
#define TASK_PRI_UI         2
#define TASK_PRI_TRANSPORT  5
#define TASK_PRI_SELECTOR   6

/* ── Task stack sizes (in words = 4 bytes each) ──────────────────────────── */

#define TASK_STACK_APP_MAIN   512U    /*  2 KB */
#define TASK_STACK_ENV        512U    /*  2 KB */
#define TASK_STACK_RADAR      1024U   /*  4 KB */
#define TASK_STACK_LIGHT      256U    /*  1 KB */
#define TASK_STACK_UI         1024U   /*  4 KB */
#define TASK_STACK_TRANSPORT  1536U   /*  6 KB */
#define TASK_STACK_SELECTOR   512U    /*  2 KB */

/* ── Watchdog task IDs ───────────────────────────────────────────────────── */

typedef enum {
    WDT_TASK_APP_MAIN = 0,
    WDT_TASK_ENV,
    WDT_TASK_RADAR,
    WDT_TASK_LIGHT,
    WDT_TASK_UI,
    WDT_TASK_TRANSPORT,
    WDT_TASK_COUNT      /* must be last */
} WdtTaskId;

/* ── FreeRTOS queue depths ───────────────────────────────────────────────── */

#define Q_ENV_DEPTH      4U
#define Q_RADAR_DEPTH    4U
#define Q_LIGHT_DEPTH    4U

/* ── MQTT topic root ─────────────────────────────────────────────────────── */

/* All topics: rmms/<uuid>/... — see CLAUDE.md §9.1 */
#define MQTT_TOPIC_ROOT  "rmms"

/* ── MQTT keepalive seconds ──────────────────────────────────────────────── */

#define MQTT_KEEPALIVE_USB   30U   /* seconds */
#define MQTT_KEEPALIVE_WIFI  60U   /* seconds */

/* ── USB probe timeout ───────────────────────────────────────────────────── */

#define USB_PROBE_TIMEOUT_MS  8000U   /* 8 s per CLAUDE.md §2.2 */

#endif /* APP_CONFIG_H */
