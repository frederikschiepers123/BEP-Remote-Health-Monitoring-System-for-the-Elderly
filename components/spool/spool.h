#ifndef SPOOL_H
#define SPOOL_H

/* Non-volatile outbound FIFO ("spool") for sensor samples — ADR-0003.
 *
 * Purpose (CLAUDE.md requirement, expands §2.3): guarantee no sample is lost
 * during a Wi-Fi or MQTT-broker outage. Every sample the firmware would publish
 * is first appended here, on flash, stamped with its original timestamp. The
 * transport drains the spool FIFO and only removes a record once the broker has
 * acknowledged it (MQTT QoS 1 PUBACK). The buffer survives power loss.
 *
 * Properties:
 *   - Strict FIFO: records are delivered in the order they were produced.
 *   - At-least-once: a record is cleared only after its PUBACK. A crash between
 *     publish and PUBACK re-sends the record (deduped downstream on `seq`, §9.6).
 *   - ≥15 min capacity at the ~4 rec/s aggregate cadence (see flash_map.h).
 *   - Overflow drops the OLDEST records (and logs it, never silent — §13.6).
 *
 * On-flash format: a circular log of fixed-size records, one record per 256-byte
 * flash page (the RP2350 flash program granularity). A whole 4 KB sector is
 * erased before its pages are reused, and eagerly once all its records are
 * delivered. See spool.c for the ring/recovery details.
 *
 * Threading: the spool is single-owner. All of mount/push/peek/ack/drop run in
 * transport_task; the MQTT PUBACK callback only flips a flag and never touches
 * the spool. Hence no internal mutex. spool_count()/spool_dropped_total() are
 * lock-free stat reads (a stale value in a log line is harmless).
 */

#include "err.h"

/* Unified sensor sample bodies (the same structs the producers emit). */
#include "env_driver.h"     /* EnvSample    */
#include "ens160.h"         /* Ens160Sample */
#include "radar_driver.h"   /* RadarSample  */
#include "light_driver.h"   /* LightSample  */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Which sensor produced a record — selects the active union member and the
 * MQTT topic / JSON encoder the transport uses on replay. Stored as a u8. */
typedef enum {
    SPOOL_KIND_ENV   = 1,
    SPOOL_KIND_AIR   = 2,
    SPOOL_KIND_RADAR = 3,
    SPOOL_KIND_LIGHT = 4,
} SpoolKind;

/* Radar body persisted on flash.  NOT the full RadarSample: the driver-internal
 * breath-phase amplitude fields (RadarSample.resp_motion_amp*) never hit flash —
 * only the wire fields do, which also keeps the body within the 16 B union
 * (ADR-0006).  resp_motion is the wire tri-state: -1 null, 0 false, 1 true. */
typedef struct {
    float    breath_rpm;
    float    heart_bpm;
    uint32_t distance_mm;
    bool     presence;
    int8_t   resp_motion;
} SpoolRadarBody;

/* Sensor-specific body. Sized to the largest member (16 B). `raw` gives a
 * byte view and pins the union size for the on-flash layout. */
typedef union {
    EnvSample      env;
    Ens160Sample   air;
    SpoolRadarBody radar;
    LightSample    light;
    uint8_t        raw[16];
} SpoolBody;

/* One on-flash record (56 bytes; stored at the start of a 256-byte flash page,
 * the rest 0xFF). Field order is the wire layout — do NOT reorder without
 * bumping SPOOL_MAGIC (spool.c), as it changes the CRC and breaks old records.
 *
 * `magic`, `write_seq`, and `crc32` are managed by spool_push(); callers fill
 * the envelope fields (ts_us / wall_ms / seq / kind / q) and the body via the
 * spool_make_* builders. `write_seq` is the spool's internal ring-ordering key,
 * distinct from the per-topic envelope `seq` (§9.2.1). */
typedef struct {
    uint32_t  magic;       /* SPOOL_MAGIC when written; 0xFFFFFFFF when erased   */
    uint32_t  seq;         /* per-topic envelope sequence number (§9.2.1)         */
    uint64_t  write_seq;   /* monotonic ring-ordering key (spool-internal)        */
    uint64_t  ts_us;       /* envelope ts_us: monotonic µs since boot             */
    int64_t   wall_ms;     /* envelope wall_ms: epoch ms, or -1 if RTC unsynced   */
    SpoolBody body;        /* sensor sample, selected by `kind`                   */
    uint8_t   kind;        /* SpoolKind                                           */
    uint8_t   q;           /* quality 0=ok 1=stale 2=degraded 3=invalid (§9.2.1)  */
    uint8_t   _pad[2];     /* zeroed for a deterministic CRC                      */
    uint32_t  crc32;       /* CRC-32 over the preceding 52 bytes                  */
} SpoolRecord;

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

/* Scan the flash region, recover the FIFO head/tail, and ready it for use.
 * Call once at boot AFTER storage_mount() (both touch flash via the same
 * flash_safe_execute path). Idempotent; an empty/fresh region mounts clean. */
err_t spool_mount(void);

/* ── Producer side ───────────────────────────────────────────────────────── */

/* Append a record (one flash page program). Assigns magic/write_seq/crc32.
 * If the ring is full of undelivered records, drops the oldest sector-full
 * first (logged; spool_dropped_total() increments) so the newest sample is
 * always stored. Returns ERR_OK, or ERR_IO on a flash failure. */
err_t spool_push(const SpoolRecord *rec);

/* ── Consumer side (transport) ───────────────────────────────────────────── */

/* Copy the oldest undelivered record into *out and its ring key into *out_ws.
 * Returns ERR_OK, or ERR_NOT_FOUND when the spool is empty. Does not remove it. */
err_t spool_peek(SpoolRecord *out, uint64_t *out_ws);

/* Mark the record identified by write_seq `ws` (from spool_peek) as delivered
 * and advance the FIFO tail; may erase a now-fully-delivered sector. `ws` must
 * equal the current tail (strict in-order ack); returns ERR_INVALID_ARG
 * otherwise (e.g. a duplicate/late PUBACK), which the caller can ignore. */
err_t spool_ack(uint64_t ws);

/* ── Builders (fill envelope + body; push assigns magic/write_seq/crc) ────── */

void spool_make_env  (SpoolRecord *r, const EnvSample    *v, uint8_t q,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq);
void spool_make_air  (SpoolRecord *r, const Ens160Sample *v, uint8_t q,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq);
void spool_make_radar(SpoolRecord *r, const RadarSample  *v, uint8_t q,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq);
void spool_make_light(SpoolRecord *r, const LightSample  *v, uint8_t q,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq);

/* ── Stats (lock-free reads) ─────────────────────────────────────────────── */

/* Undelivered records currently buffered. Exact in normal operation; after a
 * crash that left a torn page, it may read up to a few high until those pages
 * drain (they are skipped by spool_peek). Treat as an upper bound for logging. */
uint32_t spool_count(void);
/* Overflow drops since this mount (session-scoped — reset by spool_mount; not
 * persisted across reboots). Never silent: each drop is also LOG_W'd. */
uint32_t spool_dropped_total(void);
uint32_t spool_capacity(void);       /* max undelivered records before overflow  */

#endif /* SPOOL_H */
