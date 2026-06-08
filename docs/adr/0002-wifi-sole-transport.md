# ADR-0002: Wi-Fi is the sole v1 transport — USB-CDC dropped

**Status:** Accepted
**Date:** 2026-06-08

## Context

CLAUDE.md §2.1/§2.2 originally specified a **dual transport**: USB-CDC as the
*primary* link to the tablet (direct cable, MCU ↔ tablet) with Wi-Fi as
*failover*, both carrying uniform mTLS, selected by a runtime FSM (§2.2). The
entire `stream_t` abstraction (§8.3) — `mqtt_client` over `tls_context` over
`stream_cdc`/`stream_tcp` — exists to make the MQTT/TLS layers agnostic to
which physical transport carries their bytes, so USB and Wi-Fi can be swapped
with session-ticket resumption.

Two things became true during this phase:

1. **The project decided not to use the USB link at all.** The tablet ↔ MCU
   path is Wi-Fi only; there is no USB-MQTT bridge on the tablet, no cable in
   the deployment, and no requirement that motivates one. (Earlier revisions
   called USB "deferred"; it is now simply *out of scope* for v1.)

2. **Only the Wi-Fi path was ever proven on hardware.** The working bench
   firmware connects over Wi-Fi using lwIP's `altcp_tls` + lwIP's built-in
   MQTT client (`pico_lwip_mqtt`). The custom `stream_t` stack
   (`tls_context` + `mqtt_client` + `stream_tcp`/`stream_cdc`), written for
   transport-uniformity, was never exercised against a broker.

The `stream_t` abstraction's whole reason to exist — *one* TLS/MQTT stack over
*two* transports — evaporates when there is only one transport.

## Decision

**Wi-Fi (mTLS, ECDSA P-256, static certs) is the sole transport in v1.**

- The production firmware (`main/`) implements the §7 task model on the
  **proven lwIP `altcp_tls` + lwIP-MQTT path**, under `sys_freertos` (the
  keystone cyw43-init hang for that arch is fixed — see
  `lwipopts.h` and the `project-cyw43-sys-freertos-hang` note). A single
  `transport_task` owns cyw43 + Wi-Fi + TLS + MQTT and is the sole consumer of
  the sensor producer queues.
- The USB-CDC transport, the tablet-side USB-MQTT bridge, and the USB↔Wi-Fi
  selection FSM are **not built**. The `transport_usb`, `transport_selector`,
  `tls_context`, and `mqtt_client` components, plus the `stream_cdc` backend,
  remain in the tree but are **dormant** — no longer linked into
  `sensor_module`.
- Developer logging uses a standalone USB-serial console (`pico_stdio_usb`,
  §12), which is a debug aid, **not** a tablet data link.

This is recorded in CLAUDE.md §2.1/§2.2/§8.1/§8.2/§8.3/§9.4/§10/§12/§17.

## Consequences

**Positive**
- The firmware runs entirely on a hardware-proven transport; no large unverified
  TLS/MQTT stack sits on the critical path.
- Simpler boot: the transport FSM collapses to "connect Wi-Fi, stay there." No
  probe budget, no swap policy, no session-ticket resumption to validate.
- Fewer moving parts on the tablet (no USB host permissions, no Termux USB
  plugin, no byte-pipe bridge process — see CLAUDE.md §16 Q3, now moot for v1).

**Negative / risks**
- No transport redundancy: if Wi-Fi drops, the device cannot publish until it
  reassociates. Mitigated by an in-RAM retry path and reconnect-with-backoff;
  the LWT marks the device offline so consumers notice within keepalive×1.5.
- Dead code: ~1,500 lines of the custom `stream_t` stack are now unused. Kept
  (not deleted) so the USB design is recoverable, but they are a maintenance
  smell. A follow-up may delete them once USB is firmly ruled out for good.

## Reviving USB-CDC later

The `stream_t` seam is intact, so a future USB-CDC transport could slot in
behind the same MQTT/TLS layers — **but** the current firmware's MQTT/TLS is
lwIP-native (`altcp_tls` + `pico_lwip_mqtt`), not the custom `stream_t` stack.
Reviving USB would mean either (a) finishing and proving the custom stack and
moving Wi-Fi onto it too, or (b) bridging USB-CDC into lwIP. Either is a
post-v1 decision and warrants its own ADR. Until then, USB-CDC is out of scope.

## References

- CLAUDE.md §2.1 (transports), §2.2 (selection FSM), §8 (transport details).
- ADR-0001 (the two sensor-module variants).
- Memory: `project-cyw43-sys-freertos-hang` (why `sys_freertos` is viable now).
