# ADR-0003: Non-volatile flash spool + tablet time-sync (lossless uplink)

**Status:** Accepted
**Date:** 2026-06-09

## Context

The system was extended with a hard requirement: **no sensor data may be lost
during a Wi-Fi or MQTT-broker outage**, and every buffered measurement must
carry its **original timestamp**. The downstream SBC (Radxa Dragon Q6A; a Pine64
ROCKPro64 stands in for the demo) is the local gateway to the hospital FHIR
endpoint, with its own ≥24 h store-and-forward queue and `Observation.identifier`
idempotency (see `docs/CLAUDE_radxa.md` §8.6/§10 and
`docs/sbc-failover-and-idempotency.md`). For that end-to-end chain to actually be
lossless and idempotent, the firmware must hold up its end:

1. Buffer outbound samples in **non-volatile** storage across an outage (and a
   power cut), in strict FIFO order, with each sample's original timestamp.
2. Deliver with **MQTT QoS 1**, retrying until the broker's **PUBACK**, before a
   record is cleared.
3. Emit a **stable, unique `seq`** per sample — stable across re-sends *and*
   across reboots — so the SBC's `Observation.identifier` dedup never (a) drops
   genuinely new data as a duplicate, nor (b) lets a network hiccup create a
   duplicate record.

Two firmware facts made this a real change, not a tweak:

- The firmware kept **no retry buffer at all**. `transport_task` drained each
  producer queue to the *latest* sample, dropped the rest, published once, and
  ignored the result. CLAUDE.md §2.3 even described the (aspirational) buffer as
  "an in-RAM ring buffer" — which would not survive a power cut.
- `wall_ms` was **always the `-1` sentinel**: the `time/set` JSON parser existed
  but nothing subscribed to the topic. This is the long-open §16-Q6 (no RTC on
  the Pico 2).

## Decision

Add a **non-volatile flash spool** (`components/spool/`) and **tablet
time-sync**, and rework `transport_task` around them. This **supersedes the
CLAUDE.md §2.3 "in-RAM ring buffer" statement**.

### Flash spool (`components/spool/`)
- A dedicated **1 MB raw-flash circular log**, carved immediately below the
  existing 256 KB littlefs region. The layout now lives in one place,
  `components/board/flash_map.h`, shared by `storage.c` and `spool.c` with a
  `static_assert` that the two regions tile exactly. (1 MB → 4096 record slots →
  capacity 4080 undelivered ≈ **17 min @ 4 rec/s**, exceeding the 15-min
  requirement; firmware image is ~0.5 MB, far below the 2.75 MB it may occupy.)
- **One fixed 56-byte record per 256-byte flash page** (the RP2350 program
  granularity). A whole 4 KB sector is erased before its pages are reused, and
  *eagerly* once all its records are delivered. Records are self-describing
  (magic + monotonic `write_seq` + CRC-32); the FIFO head/tail are reconstructed
  by scanning at mount, so **no separate, wear-prone pointer sector** exists.
- **At-least-once, power-safe.** A record is cleared only on its PUBACK. A crash
  mid-program leaves a torn page that fails CRC and is skipped; a stale
  delivered page may be re-sent after reboot (bounded to ≈ one sector by eager
  reclaim, deduped downstream on `seq`). Flash bytes are never reprogrammed
  (whole-sector erase only) — enforced in the host test's fake-flash model.
- **Store-and-forward always:** every sample is persisted before it is published
  (the only policy that survives a power cut *during* an outage). Wear at the
  ~4 rec/s aggregate cadence is ≈ one sector-erase/sector/lap ≈ **~4 years** at
  100 k cycles — acceptable for an always-mains v1.
- **Overflow drops the OLDEST** sector-full (logged via `LOG_W`, counted in
  `spool_dropped_total()` — never silent, per §13.6) so the newest sample is
  always kept.
- **Radar stays decimated to ~1 Hz** into the spool (its driver runs at 10 Hz):
  10× the data would blow the wear budget and the 15-min capacity for no
  clinical benefit at this scale, and changes the delivered wire cadence.

### Transport rework (`transport_mqtt.c`)
- `transport_task` is a ~10 Hz pump: it **ingests** every producer sample into
  the spool (env/air/light drain fully; radar decimated), stamped with `ts_us`,
  `wall_ms`, and the per-topic `seq`; and **drains** the spool head to the broker
  with **one publish in flight** (publish → await PUBACK → `spool_ack` → next).
  In-flight = 1 makes strict-FIFO ordering and the ack bookkeeping trivially
  correct; a wider window is a documented post-v1 throughput lever.
- The PUBACK callback runs in the lwIP thread and only flips a state flag; all
  spool operations stay in `transport_task`, so the spool needs **no mutex**.

### Time-sync (resolves §16-Q6)
- After CONNACK the task subscribes to `rmms/<uuid>/time/set` and maintains a
  wall-clock offset; `wall_now_ms()` stamps each record at ingest. Before the
  first sync, records carry `wall_ms = -1` and the SBC substitutes a
  receive-time estimate (§9.6). Time comes **only** from the tablet — no
  NTP-from-WAN (the audit's failure mode). The inbound payload is handed to
  `transport_task` via a small queue so the offset is only ever written there.

### seq persistence (idempotency linchpin, §9.2.1)
- Per-topic `seq` counters are persisted to `/state/last_seq.json` periodically
  and, on boot, **resumed a whole checkpoint interval ahead** of the last saved
  value. This guarantees a reboot never reuses a `seq` (it only leaves a
  harmless gap), so post-reboot samples can't collide with a pre-reboot
  `Observation.identifier` and be wrongly dropped as duplicates by the FHIR
  server. Re-sends from the spool carry the record's **stored** `seq`, so a
  duplicate delivery maps to the *same* identifier and is deduped.

### Multicore note
Under FreeRTOS SMP, `flash_safe_execute` self-coordinates (it spawns a transient
high-priority lockout task on the other core), so runtime spool writes need **no
manual `multicore_lockout` setup**. The cost is one short-lived task create per
flash op (a few per second) — accepted; the CLAUDE.md §13.4 "no malloc in steady
state" rule concerns first-party allocations, not this SDK-internal mechanism.

## Consequences

**Positive**
- A Wi-Fi/broker outage (and a power cut) no longer loses data: samples persist
  in flash, replay in order on reconnect, and clear only on PUBACK.
- Buffered samples carry their real timestamp; with a `time/set` sync, FHIR
  Observations can be `final` rather than perpetually `preliminary`.
- The end-to-end idempotency story closes: stable, gap-monotonic `seq` →
  stable `Observation.identifier` → server-side conditional update/ignore.
- Host-tested (`test/host/test_spool.c`, 9 cases): FIFO, wrap-around,
  overflow-drop, CRC/torn rejection, and power-loss remount, with the fake-flash
  model asserting NOR program rules.

**Negative / risks**
- **Flash op vs radar UART:** a flash erase/program runs with interrupts disabled
  (and parks the other core), a window the pre-spool firmware did not have at this
  frequency. With polled radar UART RX, bytes arriving during that window can
  overflow the UART FIFO and corrupt a radar frame. Impact is low and accepted for
  v1: erases are infrequent (~one per sector reclaimed), the parser rejects a
  corrupt frame (sets `q`/drops it), the radar streams continuously so the next
  frame recovers, and radar is decimated to ~1 Hz anyway. Moving the radar to
  interrupt + ring-buffer RX (so no single read is lost to an IRQ-off window) is
  the post-v1 mitigation if frame integrity ever matters.
- Flash wear: store-always at ~4 rec/s ≈ ~4 years to nominal endurance. A future
  in-RAM-fast-path-with-spill design would cut wear ~100× but cannot survive a
  power cut during an outage; rejected for v1.
- In-flight = 1 caps drain throughput at ≈ 1/RTT; a long backlog (≈15 min)
  drains in a few minutes. Raising the in-flight window (with contiguous-prefix
  ack handling) is the lever if needed.
- `spool_count()` may briefly over-read after a torn-page crash until those
  pages drain; treated as an upper bound (documented in the API).

## References

- CLAUDE.md §2.1/§2.3 (transports / buffer), §8.3 (MQTT QoS + subscriptions),
  §9.2.1 (envelope `seq`/`wall_ms`), §9.2.5 (`time/set`), §9.6 (FHIR contract),
  §11 (flash map), §13.6 (never silently drop), §16-Q6 (RTC source).
- `docs/CLAUDE_radxa.md` §6.3/§8.6/§10 — SBC dedup, identifier scheme, SQLite buffer.
- `docs/sbc-failover-and-idempotency.md` — the end-to-end cross-tier contract.
- ADR-0002 (Wi-Fi sole transport — this spool is its lossless-buffer complement).
