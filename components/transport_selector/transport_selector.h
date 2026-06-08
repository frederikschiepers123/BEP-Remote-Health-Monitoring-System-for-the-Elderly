#ifndef TRANSPORT_SELECTOR_H
#define TRANSPORT_SELECTOR_H

/* Transport-selection FSM — implements CLAUDE.md §2.2.
 *
 * State machine:
 *   BOOT → USB_PROBE (≤ 8 s: CDC enum + TLS handshake + MQTT CONNACK)
 *        → USB_ACTIVE   (preferred when USB is present)
 *        → WIFI_ACTIVE  (failover when USB probe fails or USB unplugs)
 *
 * Swap policy:
 *   USB_ACTIVE  → WIFI_ACTIVE: USB unplug + 3 missed keepalives
 *   WIFI_ACTIVE → USB_ACTIVE:  tud_cdc_connected() becomes true
 *
 * On swap: save TLS session ticket on the departing context so the dormant
 * transport can resume quickly (RFC 5077, ~10 ms vs ~80 ms full handshake).
 *
 * The MQTT client is signalled via TRANSPORT_STREAM_CHANGED_BIT in
 * g_transport_event_group after each swap so it can re-CONNECT on the new
 * stream.
 *
 * Lock order: g_selector_mutex guards g_active_tls_stream.
 *   Callers of transport_selector_active_stream() must not hold any other
 *   mutex at the time of the call.
 */

#include "err.h"
#include "identity.h"
#include "tls_context.h"  /* for stream_t */

#include "FreeRTOS.h"
#include "event_groups.h"
#include "semphr.h"

/* Bit set in g_transport_event_group when the active stream has changed.
 * The MQTT task waits on this bit and re-CONNECT after a swap. */
#define TRANSPORT_STREAM_CHANGED_BIT  ((EventBits_t)(1u << 0))

/* Event group shared between transport_selector and the MQTT task.
 * Created by transport_selector_init(); MQTT task must not create it. */
extern EventGroupHandle_t g_transport_event_group;

/* Initialise the selector: store identity reference, create synchronisation
 * primitives.  Must be called from app_main before the selector task is
 * created.  Returns ERR_NO_MEM if FreeRTOS object creation fails. */
err_t transport_selector_init(const Identity *id);

/* FreeRTOS task entry point.
 * Create via: xTaskCreateAffinitySet(transport_selector_task, "selector",
 *                 2048, NULL, 6, (1u << 0), &selector_handle);
 * (priority 6, core 0 — see CLAUDE.md §7.1) */
void transport_selector_task(void *arg);

/* Returns the currently active post-TLS stream_t (i.e. ctx->tls_stream of
 * whichever TlsContext is active).  Returns NULL if no transport is ready.
 * Thread-safe: acquires g_selector_mutex internally. */
stream_t *transport_selector_active_stream(void);

#endif /* TRANSPORT_SELECTOR_H */
