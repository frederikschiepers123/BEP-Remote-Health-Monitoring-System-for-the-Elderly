#ifndef TRANSPORT_MQTT_H
#define TRANSPORT_MQTT_H

/* Wi-Fi + mTLS + MQTT transport task (CLAUDE.md §7.1 transport_task).
 *
 * Owns the one network path the v1 firmware has: CYW43 Wi-Fi → lwIP TCP →
 * altcp_tls (mbedTLS, ECDSA P-256, static cert chain) → lwIP MQTT v3.1.1.
 * USB-CDC is not a transport in v1 (ADR-0002); everything goes over Wi-Fi.
 *
 * The task is the SINGLE consumer of the producer queues (q_env, q_air,
 * q_radar, q_light).  Each sample is stamped with the §9.2.1 envelope
 * (ts_us + wall_ms + per-topic seq) and appended to the non-volatile spool
 * (components/spool, ADR-0003); the task then drains the spool FIFO toward the
 * broker, removing a record only once its QoS-1 PUBACK arrives.  This is what
 * makes a Wi-Fi/broker outage (and power loss) lossless.  The latest value of
 * each sensor is mirrored into the OLED shadow (ui_oled_set_*).
 *
 * The task also subscribes to rmms/<uuid>/time/set and maintains the wall-clock
 * anchor used to stamp wall_ms (§9.2.5 / §16-Q6); before the first sync,
 * wall_ms is the -1 sentinel.
 *
 * Resilience (matches the proven bring-up):
 *   - Broker address resolved from /cfg/broker.json: IP literal first, else
 *     host via lwIP DNS/mDNS, re-resolved on every reconnect attempt so the
 *     device auto-connects when the tablet/responder returns.
 *   - Reconnect with exponential backoff (1 s → 30 s).
 *   - LWT rmms/<uuid>/status = "offline" (retained, QoS 1); "online" published
 *     retained on each CONNACK.
 *   - Wi-Fi failure is non-fatal: sensor tasks keep filling queues and the
 *     OLED keeps updating; publishing resumes when the link returns.
 *
 * Runs on core 0, priority TASK_PRI_TRANSPORT (§7.1).  Uses
 * cyw43_arch_lwip_begin/end (TCP/IP core lock) around every lwIP call — safe
 * under sys_freertos because LWIP_TCPIP_CORE_LOCKING is enabled. */

#include "identity.h"
#include "cfg.h"
#include "err.h"

/* Store references to identity + per-deployment config, plus a callback the
 * task invokes once Wi-Fi bring-up has finished.  Call from app_main before
 * creating the task.  The pointed-to structs must outlive the task.
 *
 * start_sensors: invoked by transport_task exactly once, AFTER the Wi-Fi
 * association (and MQTT setup) completes — success or failure — to create the
 * producer/UI tasks.  This is deliberate: the cyw43 association must run with
 * no other Core-0 tasks contending the threadsafe_background async-context, so
 * transport_task is the only application task alive during bring-up and starts
 * the rest itself.  May be NULL (then no tasks are started post-bring-up). */
err_t transport_mqtt_init(const Identity *id,
                          const CfgWifi *wifi,
                          const CfgBroker *broker,
                          void (*start_sensors)(void));

/* FreeRTOS task entry point.  Never returns.  arg unused. */
void transport_task(void *arg);

#endif /* TRANSPORT_MQTT_H */
