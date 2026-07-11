# ADR-0009: Correct POWER/WIFI status-LED pins to match the as-built PCB

**Status:** Accepted
**Date:** 2026-06-22

## Context

`board_pico2wh.h` originally assigned `BOARD_LED_POWER_PIN = GP14 (pin 19)` and
`BOARD_LED_WIFI_PIN = GP15 (pin 20)`. Bring-up of the assembled PCB showed the
two status LEDs are wired to the **opposite** pins: the "system on" LED sits on
GP15 / pin 20 and the "wifi associated" LED on GP14 / pin 19. With the original
assignment the firmware drove the wrong physical LED for each state.

`board_pico2wh.h` is the authoritative, PCB-owned pin map (CLAUDE.md §3.4/§19),
and every caller reaches the LEDs only through these two macros, so the fix
belongs in the header — not in any per-call workaround. CLAUDE.md §19 requires
an ADR for any `board_pico2wh.h` edit; this records it.

(ADR-0008 — a phase-derived breath-rate estimator — was attempted and reverted
in the same period for sub-harmonic locking on real breathing; that number is
not reused.)

## Decision

Swap the two macros to match the as-built board:
`BOARD_LED_POWER_PIN = 15` (GP15, pin 20) and `BOARD_LED_WIFI_PIN = 14`
(GP14, pin 19). `docs/pin_map.md` is updated to match.

## Consequences

- The power/wifi LEDs now light on the correct physical pins with no code
  changes elsewhere — callers (`app_main`, transport, the bring-up publish
  task, and the OLED IO+HEALTH page) all reference the LEDs only through the
  macros, per the §3.4 "no hardcoded GPIOs outside the header" rule.
- This is a hardware-alignment correction, not a new pin allocation; nothing
  changes behaviourally beyond which LED lights.
- A future PCB respin must either preserve this mapping or update the header and
  this ADR together.
