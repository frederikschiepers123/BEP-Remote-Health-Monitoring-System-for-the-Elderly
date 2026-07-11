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
| [0006](0006-phase-based-breath-hold-detection.md) | Phase-based breath-hold (apnea) detection (resp_motion) | Accepted |
| [0007](0007-hmmd-radar-second-driver.md) | Second radar driver — Waveshare HMMD 24 GHz behind the v-table | Accepted |
| 0008 | Phase-derived breath rate via Goertzel (no file — see note) | Reverted |
| [0009](0009-led-pin-correction.md) | POWER/WIFI status-LED pins corrected to match the as-built PCB | Accepted |

**ADR-0008 note:** the Goertzel breath-rate estimator landed with commit
`6329d47` and was reverted in `4376965` after bench testing showed it
sub-harmonic-locks to ~half the true rate on real irregular breathing. The
radar's own 0x0A14 breath-rate output is used instead. The number is retired,
not reused; the design and revert rationale live in the git history of those
two commits.

## When to write an ADR

Per CLAUDE.md §4: any new dependency, any change to `board_pico2wh.h`, any new
MQTT topic or command, any change to the security model, or any deviation from
the bring-up order in §15 requires an ADR before the code is written.
