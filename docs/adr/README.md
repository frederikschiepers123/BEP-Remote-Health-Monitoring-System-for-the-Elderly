# Architecture Decision Records

Each significant architectural decision is recorded here as a short ADR.

## Format

```
# ADR-NNN: Title

**Status:** Accepted | Superseded by ADR-NNN | Deprecated
**Date:** YYYY-MM-DD

## Context
What forced a decision?

## Decision
What was decided?

## Consequences
What becomes easier or harder?
```

## Index

| # | Title | Status |
|---|-------|--------|
| [0001](0001-light-sensor-bh1750.md) | Replace GL5516 LDR with Rohm BH1750FVI for ambient light | Accepted |
| [0002](0002-wifi-sole-transport.md) | Wi-Fi is the sole v1 transport — USB-CDC dropped | Accepted |
| [0003](0003-nv-flash-spool-and-time-sync.md) | Non-volatile flash spool + tablet time-sync (lossless uplink) | Accepted |
| [0004](0004-localhost-plain-listener-app-ipc.md) | Localhost-only plain :1883 listener for on-tablet app IPC | Accepted |
| [0005](0005-mcu-side-radar-filtering.md) | MCU-side radar plausibility filtering (supervisor-directed) | Accepted |

## When to write an ADR

Per CLAUDE.md §4: any new dependency, any change to `board_pico2wh.h`, any new
MQTT topic or command, any change to the security model, or any deviation from
the bring-up order in §15 requires an ADR before the code is written.
