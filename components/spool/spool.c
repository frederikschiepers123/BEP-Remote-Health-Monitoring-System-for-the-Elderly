#define LOG_TAG "SPOOL"

/* Non-volatile outbound FIFO — see spool.h and ADR-0003.
 *
 * The "logic half" of this file (ring index math, CRC, record validation, the
 * mount scan, push/peek/ack/overflow) is host-testable; only the three flash
 * HAL functions differ between the on-target build and the host test build
 * (the latter uses a RAM-backed fake flash that enforces NOR program rules).
 * A raw XIP read cannot be redirected by a stub header — it is a pointer
 * dereference, not a function call — so the HAL split here uses HOST_TEST
 * rather than the header-shadow pattern the sensor drivers use for I²C/UART.
 */

#include "pico/stdlib.h"      /* PICO_FLASH_SIZE_BYTES, XIP_BASE (target)        */
#include "hardware/flash.h"   /* FLASH_SECTOR_SIZE, FLASH_PAGE_SIZE, flash_*     */
#if !HOST_TEST
#include "pico/flash.h"       /* flash_safe_execute, PICO_OK                     */
#endif

#include "flash_map.h"        /* SPOOL_FLASH_OFFSET, SPOOL_FLASH_SIZE            */
#include "spool.h"
#include "log.h"

#include <string.h>
#include <stddef.h>
#include <assert.h>

/* ── On-flash geometry ───────────────────────────────────────────────────── */

#define SPOOL_MAGIC       0x524D5332u           /* "RMS2" — record-valid marker
                                                  * (bumped: radar body layout
                                                  * changed for resp_motion,
                                                  * ADR-0006)                     */
#define SPOOL_SLOT_SIZE   FLASH_PAGE_SIZE        /* one record per flash page     */
#define SPOOL_S           (FLASH_SECTOR_SIZE / SPOOL_SLOT_SIZE)   /* slots/sector */
#define SPOOL_N           (SPOOL_FLASH_SIZE / SPOOL_SLOT_SIZE)    /* total slots  */
/* Reserve one sector so the sector head is entering is always reclaimable
 * (its records are delivered or dropped — never still-needed). */
#define SPOOL_CAP         (SPOOL_N - SPOOL_S)    /* max undelivered records       */

/* The record must fit in one page, and its on-flash layout must be stable. */
static_assert(sizeof(SpoolRecord) <= SPOOL_SLOT_SIZE, "record larger than a page");
static_assert(sizeof(SpoolRecord) == 56, "SpoolRecord layout changed unexpectedly");
static_assert(offsetof(SpoolRecord, crc32) == 52, "crc32 must follow the CRC'd region");
static_assert(sizeof(SpoolBody) == 16, "SpoolBody must be 16 bytes");
static_assert(SPOOL_N % SPOOL_S == 0u, "ring must be a whole number of sectors");

/* ── Ring state (transport_task only; PUBACK cb never touches it) ─────────── */

static uint64_t s_head_ws = 0;   /* next write_seq to assign (head_slot = %N)    */
static uint64_t s_tail_ws = 0;   /* oldest undelivered write_seq                 */
static volatile uint32_t s_dropped_total = 0;
static bool     s_mounted = false;

/* ── Flash HAL ───────────────────────────────────────────────────────────── */

#if HOST_TEST

/* RAM-backed fake flash. Enforces NOR semantics: erase sets 0xFF, program may
 * only clear bits (1→0). An attempt to set a 0 bit back to 1 (i.e. programming
 * a slot that was not erased) trips the assert — catching any logic bug that
 * would corrupt real flash. Persists across spool_mount() calls so tests can
 * simulate a reboot / power loss by re-mounting over the same image. */
static uint8_t g_fake_flash[SPOOL_FLASH_SIZE];

void spool_host_flash_reset(void) { memset(g_fake_flash, 0xFF, sizeof g_fake_flash); }
uint8_t *spool_host_flash_ptr(void) { return g_fake_flash; }

static void hal_read(uint32_t off, void *buf, size_t n) {
    memcpy(buf, &g_fake_flash[off - SPOOL_FLASH_OFFSET], n);
}
static err_t hal_program(uint32_t off, const void *buf, size_t n) {
    uint8_t *dst = &g_fake_flash[off - SPOOL_FLASH_OFFSET];
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < n; i++) {
        assert(((dst[i] & src[i]) == src[i]) && "illegal flash program (slot not erased)");
        dst[i] = dst[i] & src[i];
    }
    return ERR_OK;
}
static err_t hal_erase(uint32_t off) {
    memset(&g_fake_flash[off - SPOOL_FLASH_OFFSET], 0xFF, FLASH_SECTOR_SIZE);
    return ERR_OK;
}

#else  /* on-target: same flash_safe_execute discipline as components/storage */

#define SPOOL_FLASH_OP_TIMEOUT_MS 5000

typedef struct { uint32_t addr; const uint8_t *buf; size_t size; } prog_args_t;

static void do_program(void *p) {
    const prog_args_t *a = (const prog_args_t *)p;
    flash_range_program(a->addr, a->buf, a->size);
}
static void do_erase(void *p) {
    flash_range_erase(*(const uint32_t *)p, FLASH_SECTOR_SIZE);
}

static void hal_read(uint32_t off, void *buf, size_t n) {
    memcpy(buf, (const void *)(XIP_BASE + off), n);
}
static err_t hal_program(uint32_t off, const void *buf, size_t n) {
    prog_args_t a = { .addr = off, .buf = (const uint8_t *)buf, .size = n };
    int rc = flash_safe_execute(do_program, &a, SPOOL_FLASH_OP_TIMEOUT_MS);
    if (rc != PICO_OK) { LOG_E("flash program failed: %d", rc); return ERR_IO; }
    return ERR_OK;
}
static err_t hal_erase(uint32_t off) {
    int rc = flash_safe_execute(do_erase, &off, SPOOL_FLASH_OP_TIMEOUT_MS);
    if (rc != PICO_OK) { LOG_E("flash erase failed: %d", rc); return ERR_IO; }
    return ERR_OK;
}

#endif

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static uint32_t crc32_calc(const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
        }
    }
    return ~crc;
}

static uint32_t slot_off(uint32_t slot) {
    return (uint32_t)SPOOL_FLASH_OFFSET + slot * (uint32_t)SPOOL_SLOT_SIZE;
}

static bool rec_valid(const SpoolRecord *r) {
    return r->magic == SPOOL_MAGIC &&
           crc32_calc(r, offsetof(SpoolRecord, crc32)) == r->crc32;
}

/* True if the slot has never been written since its last sector erase. */
static bool slot_erased(uint32_t slot) {
    uint32_t magic;
    hal_read(slot_off(slot), &magic, sizeof magic);
    return magic == 0xFFFFFFFFu;
}

static err_t erase_sector_of(uint32_t slot) {
    uint32_t sector_first = (slot / SPOOL_S) * SPOOL_S;
    return hal_erase(slot_off(sector_first));   /* slot_off(first slot) == sector base */
}

/* Drop oldest sector-fulls until one more push keeps us within capacity.
 * Advances the tail (logical drop); the dropped pages are physically reclaimed
 * when the head laps back to them. Logged — never silent (§13.6). */
static void ensure_room_for_one(void) {
    while ((s_head_ws - s_tail_ws) >= (uint64_t)SPOOL_CAP) {
        uint64_t old_tail = s_tail_ws;
        uint64_t next     = ((s_tail_ws / SPOOL_S) + 1u) * SPOOL_S;   /* next sector */
        uint32_t n        = (uint32_t)(next - old_tail);
        s_tail_ws = next;
        s_dropped_total += n;
        LOG_W("spool full: dropped %u oldest records (write_seq %llu..%llu); "
              "dropped_total=%u",
              n, (unsigned long long)old_tail, (unsigned long long)(next - 1u),
              s_dropped_total);
    }
}

/* ── Public API ──────────────────────────────────────────────────────────── */

err_t spool_mount(void) {
    s_dropped_total = 0;

    /* Pass 1: find the highest valid write_seq → that record is the newest. */
    bool found = false;
    uint64_t max_ws = 0;
    for (uint32_t slot = 0; slot < SPOOL_N; slot++) {
        SpoolRecord r;
        hal_read(slot_off(slot), &r, sizeof r);
        if (rec_valid(&r) && (uint32_t)(r.write_seq % SPOOL_N) == slot) {
            if (!found || r.write_seq > max_ws) { max_ws = r.write_seq; found = true; }
        }
    }

    if (!found) {
        s_head_ws = 0;
        s_tail_ws = 0;
        s_mounted = true;
        LOG_I("mounted: empty (cap %u records, %u KB)",
              (unsigned)SPOOL_CAP, (unsigned)(SPOOL_FLASH_SIZE / 1024u));
        return ERR_OK;
    }

    s_head_ws = max_ws + 1u;

    /* Pass 2: walk back from max_ws over the contiguous run of valid records to
     * find the tail. Capped at SPOOL_CAP so a stale pre-drop record can never
     * resurrect more than one buffer's worth. Delivered-but-not-yet-erased
     * records inside this run get re-sent on reboot (bounded; deduped on seq). */
    uint64_t tail = max_ws;
    while (tail > 0u && (max_ws - tail + 1u) < (uint64_t)SPOOL_CAP) {
        uint64_t w = tail - 1u;
        SpoolRecord r;
        hal_read(slot_off((uint32_t)(w % SPOOL_N)), &r, sizeof r);
        if (rec_valid(&r) && r.write_seq == w) { tail = w; } else { break; }
    }
    s_tail_ws = tail;

    /* If a push was interrupted mid-program, the head slot holds a torn record
     * (mid-sector, not erased). Skip past any such dirty page(s) so the next
     * push never programs a non-erased page. We stop at the first erased slot or
     * the next sector boundary (where push's pos-0 erase makes it writable).
     * Only the in-flight write can be torn, so this normally skips one page; the
     * skipped page is reclaimed when the head laps back to it. (A skipped torn
     * page sits between tail and head, so spool_count() may read one high until
     * it drains — see the note on spool_count().) */
    for (uint32_t i = 0; i < SPOOL_S; i++) {
        uint32_t hslot = (uint32_t)(s_head_ws % SPOOL_N);
        if ((hslot % SPOOL_S) == 0u || slot_erased(hslot)) { break; }
        LOG_W("torn head at slot %u — skipping", (unsigned)hslot);
        s_head_ws++;
    }

    s_mounted = true;
    LOG_I("mounted: %u undelivered (write_seq %llu..%llu)",
          spool_count(), (unsigned long long)s_tail_ws,
          (unsigned long long)max_ws);
    return ERR_OK;
}

err_t spool_push(const SpoolRecord *in) {
    if (!s_mounted) { return ERR_NOT_INIT; }

    ensure_room_for_one();

    uint32_t slot = (uint32_t)(s_head_ws % SPOOL_N);

    /* Entering a fresh sector: erase it if it still holds stale/dropped pages.
     * (Eagerly-reclaimed delivered sectors are already 0xFF — skip those.) */
    if ((slot % SPOOL_S) == 0u && !slot_erased(slot)) {
        err_t e = erase_sector_of(slot);
        if (e != ERR_OK) { return e; }
    }

    SpoolRecord r = *in;
    r.magic     = SPOOL_MAGIC;
    r.write_seq = s_head_ws;
    r._pad[0]   = 0;
    r._pad[1]   = 0;
    r.crc32     = crc32_calc(&r, offsetof(SpoolRecord, crc32));

    /* Program the record into the start of an erased page (rest stays 0xFF). */
    uint8_t page[SPOOL_SLOT_SIZE];
    memset(page, 0xFF, sizeof page);
    memcpy(page, &r, sizeof r);
    err_t e = hal_program(slot_off(slot), page, sizeof page);
    if (e != ERR_OK) { return e; }

    s_head_ws++;
    return ERR_OK;
}

err_t spool_peek(SpoolRecord *out, uint64_t *out_ws) {
    if (!s_mounted) { return ERR_NOT_INIT; }

    while (s_tail_ws < s_head_ws) {
        uint32_t slot = (uint32_t)(s_tail_ws % SPOOL_N);
        SpoolRecord r;
        hal_read(slot_off(slot), &r, sizeof r);
        if (rec_valid(&r) && r.write_seq == s_tail_ws) {
            *out = r;
            if (out_ws) { *out_ws = s_tail_ws; }
            return ERR_OK;
        }
        /* A gap at the tail should not occur in normal operation; skip it so a
         * single bad page can never wedge the FIFO. Mirror spool_ack's eager
         * reclaim so skipping across a sector boundary still frees the sector. */
        LOG_W("skipping invalid record at tail write_seq=%llu",
              (unsigned long long)s_tail_ws);
        s_tail_ws++;
        if ((s_tail_ws % SPOOL_S) == 0u) {
            (void)erase_sector_of((uint32_t)((s_tail_ws - 1u) % SPOOL_N));
        }
    }
    return ERR_NOT_FOUND;
}

err_t spool_ack(uint64_t ws) {
    if (!s_mounted) { return ERR_NOT_INIT; }
    if (ws != s_tail_ws) { return ERR_INVALID_ARG; }   /* out-of-order / dup PUBACK */

    s_tail_ws++;

    /* Eager reclaim: completing a sector frees it immediately, bounding how many
     * delivered records linger (and thus how many re-send after a reboot). */
    if ((s_tail_ws % SPOOL_S) == 0u) {
        (void)erase_sector_of((uint32_t)((s_tail_ws - 1u) % SPOOL_N));
    }
    return ERR_OK;
}

/* ── Builders ────────────────────────────────────────────────────────────── */

static void make_common(SpoolRecord *r, SpoolKind kind, uint8_t q,
                        uint64_t ts_us, int64_t wall_ms, uint32_t seq) {
    memset(r, 0, sizeof *r);
    r->kind    = (uint8_t)kind;
    r->q       = q;
    r->ts_us   = ts_us;
    r->wall_ms = wall_ms;
    r->seq     = seq;
    /* magic / write_seq / crc32 are assigned by spool_push(). */
}

void spool_make_env(SpoolRecord *r, const EnvSample *v, uint8_t q,
                    uint64_t ts_us, int64_t wall_ms, uint32_t seq) {
    make_common(r, SPOOL_KIND_ENV, q, ts_us, wall_ms, seq);
    r->body.env = *v;
}
void spool_make_air(SpoolRecord *r, const Ens160Sample *v, uint8_t q,
                    uint64_t ts_us, int64_t wall_ms, uint32_t seq) {
    make_common(r, SPOOL_KIND_AIR, q, ts_us, wall_ms, seq);
    r->body.air = *v;
}
void spool_make_radar(SpoolRecord *r, const RadarSample *v, uint8_t q,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq) {
    make_common(r, SPOOL_KIND_RADAR, q, ts_us, wall_ms, seq);
    /* Persist only the wire fields (SpoolRadarBody), not the driver-internal
     * breath-phase amplitude on RadarSample (ADR-0006). */
    r->body.radar.breath_rpm  = v->breath_rpm;
    r->body.radar.heart_bpm   = v->heart_bpm;
    r->body.radar.distance_mm = v->distance_mm;
    r->body.radar.presence    = v->presence;
    r->body.radar.resp_motion = v->resp_motion_valid
                                    ? (v->resp_motion ? 1 : 0)
                                    : -1;
}
void spool_make_light(SpoolRecord *r, const LightSample *v, uint8_t q,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq) {
    make_common(r, SPOOL_KIND_LIGHT, q, ts_us, wall_ms, seq);
    r->body.light = *v;
}

/* ── Stats ───────────────────────────────────────────────────────────────── */

uint32_t spool_count(void)         { return (uint32_t)(s_head_ws - s_tail_ws); }
uint32_t spool_dropped_total(void) { return s_dropped_total; }
uint32_t spool_capacity(void)      { return (uint32_t)SPOOL_CAP; }
