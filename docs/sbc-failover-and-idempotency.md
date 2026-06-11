# SBC failover & end-to-end idempotency (cross-tier architecture)

This document specifies the **loss-tolerance and idempotency contract across all
three tiers** — sensor firmware → tablet broker → SBC gateway → hospital FHIR —
and shows where each requirement is satisfied. It is the "explicitly documented
system architecture" the requirements call for.

It is a **contract document**. Both tiers are implemented in this monorepo: the
firmware at the repo root (ADR-0003) and the SBC service under [`sbc/`](../sbc/),
a standalone Python project the firmware build never links. The SBC's
authoritative spec is [`sbc/CLAUDE.md`](../sbc/CLAUDE.md); this document
references it rather than restating it. Per CLAUDE.md §9.6, **there is no
FHIR/LOINC code on the MCU** — keeping the SBC in a separate project (not
compiled into the firmware) upholds that. The SBC owns all FHIR concerns.

> **Demo platform note.** Production target is the **Radxa Dragon Q6A**; the
> **Pine64 ROCKPro64** stands in for the demo. The SBC software is
> platform-portable (Ubuntu aarch64, Docker); the substitution changes nothing
> in this contract. (`sbc/CLAUDE.md` §1 records the original
> ROCKPro64→Q6A change; the demo simply runs the same service on the ROCKPro64.)

---

## 1. The three-tier topology

```
  Sensor firmware (this repo)        Tablet            SBC gateway (Radxa/ROCKPro64)     Hospital
  ───────────────────────────       ────────          ─────────────────────────────     ────────
  sensors → NV flash spool   ──mTLS──► Mosquitto ──mTLS──► MQTT subscriber                 FHIR R4
  (≥15 min, FIFO, QoS 1,             broker  :8883        → JSON → typed Sample            endpoint
   wall_ms-stamped)                                       → FHIR R4 Observation            (HAPI dev /
        ▲ retry until PUBACK                              → SQLite store-and-forward        real EHR)
        │                                                   (≥24 h outbound queue)   ──────►  ▲
        └── clears record only on PUBACK                  → POST Bundle (PUT/If-None-Exist)   │
                                                            idempotent on Observation.identifier
```

Two independent store-and-forward buffers, one per "hop that can fail":

| Buffer | Tier | Protects against | Capacity | Spec |
|---|---|---|---|---|
| **Flash spool** | Firmware | Wi-Fi / broker outage, device power loss | **≥15 min** (≈17 min) | ADR-0003 |
| **SQLite queue** | SBC | Hospital FHIR-endpoint / cloud outage | **≥24 h** outbound Observations | `sbc/CLAUDE.md` §10 |

---

## 2. Requirement-by-requirement traceability

### SBC = local gateway with cloud failover
When the hospital FHIR endpoint is unreachable, the SBC buffers locally and
replays on recovery — it does **not** drop. Implemented as the SQLite
`observations` table with `status ∈ {pending, posted, dead_letter}` and an
exponential-backoff retry/dead-letter path (`sbc/CLAUDE.md` §2.1 data-flow,
§9.2 posting, §10 storage). **Never silent-drop** (§9.3 / §10).

### Capacity: ≥24 h of outbound FHIR observations
The SQLite queue must hold **at least 24 h** of un-posted Observations. At the
aggregate cadence (~4 samples/s/device → ~4 Observations/s, ~400–600 B each
including FHIR envelope) that is ≈ 140 MB/device/day — trivially within the
Radxa/ROCKPro64's storage. `sbc/CLAUDE.md` §10.4 retains `samples` 30 days and
`observations` indefinitely, which **exceeds** the 24 h floor; this document
fixes **24 h of outbound (pending) Observations** as the hard minimum the SBC
deployment must guarantee (disk-provisioning check at install, §13).

### Idempotency & deduplication (every resource has a unique `Observation.identifier`)
Every Observation carries
`identifier = { system: "urn:rmms:seq", value: "<uuid>-<sensor>-<seq>" }`
(`sbc/CLAUDE.md` §8.6). The `<seq>` is the firmware's per-topic envelope
sequence number (§9.2.1). This is the single value that makes a measurement
**globally and stably identifiable** across every retry and replay.

### Server-side handling (update-or-ignore, never duplicate)
The SBC POSTs a `Bundle` of `transaction` type whose entries use **conditional
update / create**: `PUT [base]/Observation?identifier=urn:rmms:seq|<uuid>-<sensor>-<seq>`
(equivalently `If-None-Exist`). A FHIR R4 server that honours conditional
semantics will **update or ignore** a resource with a pre-existing identifier
instead of creating a duplicate (`sbc/CLAUDE.md` §8.6, §9.2). **The receiving
FHIR server must enforce this on `Observation.identifier`** — it is a deployment
requirement on the endpoint, documented here for the integration team.

### Ambient data → LOINC **or** SNOMED CT, explicitly documented
### Medical measurements → **strictly LOINC** in `Observation.code`
The mapping table is authoritative in `sbc/CLAUDE.md` §8.2. Summary:

| Field | Class | Code system | Code | Unit | Status |
|---|---|---|---|---|---|
| `heart_bpm` | medical (vital) | **LOINC** | `8867-4` | `/min` | verified |
| `breath_bpm` | medical (vital) | **LOINC** | `9279-1` | `/min` | verified |
| `hum_pct` | ambient | **LOINC** | `19736-7` | `%` | verified |
| `presence` | medical | placeholder `urn:rmms:obs-code` | `presence` | (boolean) | verify (no verified LOINC) |
| `temp_c` | ambient | placeholder `urn:rmms:obs-code` | `ambient_temp_c` | `Cel` | verify (8310-5 is *body* temp) |
| `pres_hpa` | ambient | placeholder `urn:rmms:obs-code` | `atmospheric_pressure_hpa` | `hPa` | verify (3140-1 is *body surface area*) |
| `co2_ppm` | ambient (air) | placeholder (LOINC **or SNOMED CT**) | `co2_ppm` | `ppm` | verify |
| `tvoc_ppb` | ambient (air) | placeholder (LOINC **or SNOMED CT**) | `tvoc_ppb` | `ppb` | verify |
| `aqi` | ambient (air) | placeholder (LOINC **or SNOMED CT**) | `uba_aqi` | (index 1–5) | verify |
| `lux` | ambient | placeholder `urn:rmms:obs-code` | `ambient_lux` | `lx` | verify |
| `distance_mm` | device meta | placeholder `urn:rmms:obs-code` | `subject_distance` | `mm` | verify |

Rule, per the requirements: **medical/vital fields use LOINC** — `heart_bpm` and
`breath_bpm` carry verified LOINC codes. **Ambient fields use a verified LOINC
where one exists** (`hum_pct`), **else a flagged placeholder** (`urn:rmms:obs-code`,
`confirmed=False`) pending a LOINC/SNOMED assignment. The remaining rows have no
*verified* code yet and **require clinical-advisor sign-off** before deployment —
they are flagged, never invented, and a real-but-wrong LOINC is never shipped
(an adversarial review caught `76689-9`/`3140-1` being mis-applied — see
`sbc/CLAUDE.md` §8.2). The safety invariant is asserted in the test suite
(`test_no_unverified_loinc_is_shipped`).

### All vitals + environmental metrics → HL7 FHIR R4 `Observation`
Every measurement becomes exactly one FHIR R4 `Observation` (one per value;
radar's heart/breath/presence are separate Observations), built with
`fhir.resources` Pydantic models so validation happens at construction
(`sbc/CLAUDE.md` §8.1, §8.3, §9.3). Quality → status: `0→final`,
`1/2→preliminary`, `3→dead-letter` (§8.4); a `-1` `wall_ms` forces `preliminary`
(§8.4/§8.5).

---

## 3. The firmware↔SBC seam: how `seq` makes it all idempotent

This is the part ADR-0003 adds, and the reason the SBC's existing dedup is now
correct under firmware outages **and reboots**:

1. **Stable per sample.** The firmware stamps `seq` at *ingest* and stores it in
   the flash spool record. A re-send after a Wi-Fi blip carries the **same**
   `seq` → the **same** `Observation.identifier` → the FHIR server's conditional
   update dedups it. (A network hiccup mid-transmission cannot create a
   duplicate.)

2. **Monotonic with gaps across reboots.** The firmware persists `seq` and, on
   boot, resumes a checkpoint interval **ahead** of the last saved value
   (ADR-0003). So a reboot never *reuses* a `seq`; it only leaves a gap. This
   prevents the dangerous failure mode where post-reboot `seq=0,1,2…` would
   collide with pre-reboot identifiers and the server would **drop genuinely new
   readings as duplicates** (silent data loss).

3. **SBC dedup is compatible.** `sbc/CLAUDE.md` §6.3 drops samples with
   `seq ≤ last seen` per `(uuid, sensor)`. Forward jumps (reboot gaps) satisfy
   `seq > last seen` → accepted. Re-sends (`seq ≤ last seen`) are dropped as
   duplicates — and even if the SBC's in-memory cursor was lost to its own
   restart, the FHIR-side identifier dedup (point 1) is the backstop.

Net: **at-least-once** delivery at every hop, **exactly-once** *effect* at the
FHIR server, with no silent loss in either direction.

---

## 4. Boundaries (unchanged, restated for clarity)

- The firmware emits only the raw `rmms/<uuid>/...` sensor JSON (§9.1/§9.2) and
  subscribes only to `rmms/<uuid>/time/set` (+ `cmd`, when implemented). It
  emits **no FHIR, no LOINC, no patient identity** (§9.6).
- The SBC owns FHIR construction, LOINC/UCUM/SNOMED coding, the
  `device_uuid → patient_id` binding, OAuth to the hospital, the ≥24 h SQLite
  queue, dead-lettering, and conditional-update posting.
- The hospital FHIR server must honour conditional update/create on
  `Observation.identifier`.

## 5. References

- This repo: ADR-0003 (flash spool + time-sync + seq persistence);
  CLAUDE.md §9.1/§9.2/§9.6 (topics, envelope, FHIR contract).
- `sbc/CLAUDE.md` §2 (data flow), §6.3 (dedup), §8 (FHIR mapping),
  §8.6 (identifier scheme), §9 (posting/OAuth), §10 (SQLite), §12 (provisioning).
- FHIR R4 conditional update: <https://hl7.org/fhir/R4/http.html#cond-update>.
