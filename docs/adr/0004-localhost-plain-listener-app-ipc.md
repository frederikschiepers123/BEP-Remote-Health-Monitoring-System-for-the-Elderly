# ADR-0004: Localhost-only plain MQTT listener for on-device app IPC

**Status:** Accepted
**Date:** 2026-06-09

## Context

CLAUDE.md §19.1 is explicit: the broker has **only** the `:8883` mTLS listener —
"There is no second listener. If a future change needs one, it is an ADR." This
counters an audit finding against the previous group (a plain `:1884` listener
that bypassed mTLS — `docs/technical-audit.md`).

The tablet-side screen-wake app (`HealthMonitorWakeTest`, Yasmina) and the
radar-presence→screen bridge (`scripts/tablet_presence_screen.py`,
docs/presence_screen_coupling.md) communicate over **plain MQTT on
`127.0.0.1:1883`** with a tiny `display` ON/OFF contract. They are both
on-device tablet processes; mTLS + per-client certs would be dead weight for a
loopback IPC channel between two apps on the same device.

`demo_start.sh` → `refresh_broker.sh` restarts mosquitto with the generated
`:8883`-only config (and `pkill mosquitto`), which silently removes any
separately-started `:1883` broker — so the app/bridge need `:1883` to be part of
the *same* mosquitto config to survive a demo restart.

## Decision

mosquitto runs **two listeners** under `per_listener_settings true`:

| Listener | Bind | Auth | Purpose |
|---|---|---|---|
| `8883` | `0.0.0.0` (network) | mTLS, `require_certificate true`, `use_identity_as_username`, ACL | **The firmware broker contract** (§19.1). Firmware, mirror, operator, presence-bridge READ side. Unchanged. |
| `1883` | `127.0.0.1` (loopback) | `allow_anonymous true`, no ACL | **On-device app IPC only.** `display` ON/OFF between the wake app and the presence bridge. No firmware traffic. |

The `:1883` listener is bound to `127.0.0.1`, so it is **not reachable from the
network** — only processes on the tablet can use it.

## Why this is not the §19.1 / audit anti-pattern

The audit's and §19.1's concern is a **network-facing plain listener that lets a
LAN host bypass mTLS** to publish/subscribe firmware topics. This listener is
categorically different:

- **Not network-reachable** — `127.0.0.1` only. A LAN attacker cannot connect.
- **No firmware traffic** — the firmware speaks `:8883` mTLS exclusively and
  never touches `:1883`. The `rmms/<uuid>/...` tree and its ACL live entirely on
  `:8883`. `:1883` carries only the `display` IPC topic.
- **Distinct trust domain** — it is the tablet's own apps talking to each other,
  equivalent to a Unix socket; it is not part of the device↔broker security
  contract.

So the mTLS contract (every byte off the *device* is mTLS — §2.1) is intact:
nothing leaves the tablet over `:1883`.

## Consequences

- One mosquitto config serves both the firmware and the screen-wake app; a demo
  restart no longer drops the app's broker.
- If the app/bridge ever need to run on a *different* host than the broker, this
  loopback assumption breaks and the channel must move to `:8883` mTLS (a new
  decision) — they are co-located on the tablet today, so it holds.
- `per_listener_settings true` means each listener's auth/ACL is independent;
  the `:8883` security posture is unchanged by adding `:1883`.

## References

- CLAUDE.md §19.1 (single-listener rule), §2.1 (every byte off-device is mTLS).
- `docs/presence_screen_coupling.md` (what uses `:1883`).
- `scripts/provision_ca.sh` (generates the two-listener `mosquitto.conf`).
