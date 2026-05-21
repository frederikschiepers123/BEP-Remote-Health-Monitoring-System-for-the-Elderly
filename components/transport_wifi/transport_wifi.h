#ifndef TRANSPORT_WIFI_H
#define TRANSPORT_WIFI_H

/* Wi-Fi transport initialisation and link management.
 *
 * Initialises the CYW43 chip, brings up the lwIP stack under FreeRTOS,
 * performs DHCP, and exposes helpers for connecting to an AP and checking
 * link status.
 *
 * Credentials are read from /cfg/wifi.json at connect time.
 * Broker discovery is via mDNS (_mqtt._tcp.local) with a static-IP fallback
 * from /cfg/broker.json.  UDP broadcast discovery is explicitly prohibited
 * (see CLAUDE.md §8.2 and docs/technical-audit.md §D.2).
 *
 * Lock order: none externally visible.  Internal state is protected by a
 * FreeRTOS mutex (g_wifi_mutex) private to transport_wifi.c.
 */

#include "err.h"
#include <stdbool.h>

/* Initialise the CYW43 driver and lwIP stack.
 * Must be called once at boot, before any other transport_wifi_* call.
 * Country set to CYW43_COUNTRY_NETHERLANDS as required by CLAUDE.md §8.2. */
err_t transport_wifi_init(void);

/* Connect to the AP whose credentials are in /cfg/wifi.json.
 * Blocks the calling task using vTaskDelay (no spin-wait).
 * Applies exponential back-off 1 s → 32 s, then constant 32 s.
 * Returns ERR_OK once DHCP is assigned, or ERR_NOT_FOUND if the config
 * file is absent, or ERR_IO on repeated association failure. */
err_t transport_wifi_connect_ssid(const char *ssid, const char *psk);

/* Returns true if the Wi-Fi link is currently up (IP assigned). */
bool transport_wifi_is_linked(void);

#endif /* TRANSPORT_WIFI_H */
