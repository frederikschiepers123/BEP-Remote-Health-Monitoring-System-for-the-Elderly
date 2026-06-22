#define LOG_TAG "BHA2"
#include "log.h"

#include "radar_driver.h"
#include "board_pico2wh.h"
#include "breath_freq.h"
#include "err.h"

/* These resolve to the pico-sdk / FreeRTOS kernel on target, and to the
 * minimal stubs under test/host/stubs on the host unit-test build. */
#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/*
 * Seeed MR60BHA2 driver — 60 GHz heartbeat + breath detection.
 *
 * Protocol (bench-confirmed against the live module; CLAUDE.md §16 Q2 is
 * RESOLVED — MR60BHA2 does NOT share the Andar/AI-Thinker 0x53 0x59 framing
 * with the C1001):
 *
 *   Header (8 bytes):
 *     [0x01][SEQ_H][SEQ_L][LEN_H][LEN_L][TYPE_H][TYPE_L][HDR_CKSUM]
 *     HDR_CKSUM = ~XOR(header[0..6])
 *
 *   Data: LEN bytes (LEN is big-endian)
 *
 *   Data checksum: 1 byte = ~XOR(data[0..LEN-1])
 *
 * Frame types we consume (little-endian floats inside the payload):
 *   0x0A14  breath rate              4 B  (1 float, breaths/min)
 *   0x0A15  heart rate               4 B  (1 float, BPM)
 *   0x0A16  distance                 8 B  (u32 flag + float cm; flag==1 valid)
 *   0x0F09  human detected           1 B  (bool)
 *   0x0A13  phase data               12 B (3 floats: [total][breath][heart])
 *           — the breath phase (offset 4) is the chest-displacement signal we
 *             use for phase-based breath-hold detection (ADR-0006).  Float
 *             order confirmed against the Seeed/ESPHome driver; a wrong guess
 *             is fail-safe (heart phase keeps swinging during a hold, so it
 *             would simply never flag one).
 *
 * Frames we tolerate but skip:
 *   Anything else (firmware-version-dependent)
 *
 * The driver doesn't request frames — the module pushes them at its own
 * cadence (~1 Hz for breath/heart, faster for phase). read_sample() processes
 * whatever's in the UART RX FIFO up to the timeout, then returns the latched
 * latest values. presence is true if any valid frame arrived in the last
 * RADAR_STALE_MS, or if 0x0F09 explicitly said so. distance_mm = 0 sentinel
 * for "no valid target".
 */

/* ── Tunables ────────────────────────────────────────────────────────────── */

#define BHA2_HEADER_LEN     8U
#define BHA2_MAX_PAYLOAD   64U
#define BHA2_SOF           0x01U

#define TYPE_PHASE    0x0A13U
#define TYPE_BREATH   0x0A14U
#define TYPE_HEART    0x0A15U
#define TYPE_DISTANCE 0x0A16U
#define TYPE_HUMAN    0x0F09U

/* A latched value is considered stale (and excluded from the next sample)
 * after this many ms without an update. */
#define RADAR_STALE_MS    5000U

/* Breath-phase ring (ADR-0006 + the breath_freq.c rate estimator).  The module
 * streams the 0x0A13 phase frame at several Hz; we keep a rolling history of the
 * BREATH phase serving TWO consumers:
 *   - amplitude (peak-to-peak over the last AMP_WIN_MS) → breath-hold detection;
 *   - rate (Goertzel over the last BREATH_FREQ_WIN_MS in breath_freq.c) → the
 *     published breath_rpm, replacing the radar's own 0x0A14 BR which
 *     over-reports at slow breathing.
 * The ring is time-bounded to KEEP_MS (>= BREATH_FREQ_WIN_MS) by phase_push;
 * each consumer sub-selects its own shorter window. */
#define BHA2_AMP_WIN_MS        6000U  /* amplitude window (one slow breath cycle) */
#define BHA2_PHASE_KEEP_MS     32000U /* ring retention; >= BREATH_FREQ_WIN_MS    */
#define BHA2_PHASE_CAP         384U   /* holds KEEP_MS up to ~12 Hz phase rate    */
#define BHA2_PHASE_MIN_N       8U     /* fewer samples ⇒ amplitude not trusted   */
#define BHA2_PHASE_FRESH_MS    1500U  /* newest sample older than this ⇒ invalid  */
#define BHA2_PHASE_MIN_SPAN_MS 4000U  /* amplitude window must span this          */

/* TEMP capture for breath-algorithm design (remove after): when 1, emit
 * parseable "CAP," CSV lines on the dev console for every breath-phase,
 * raw-BPM, and distance frame, so scripts/analyze_breath.py can compare
 * candidate frequency estimators against the radar's own BPM offline.
 *   CAP,P,<t_ms>,<total>,<breath>,<heart>   — phase waveform (native cadence)
 *   CAP,B,<t_ms>,<raw_bpm>                   — radar's breath rate
 *   CAP,D,<t_ms>,<distance_mm>               — valid distance reading        */
#define BHA2_CAPTURE 1

/* ── Driver context ──────────────────────────────────────────────────────── */

typedef struct {
    uart_inst_t *uart;
    bool         initialised;

    /* Latched last-known values + when they were updated (boot ms). */
    float    last_breath;
    uint32_t last_breath_at_ms;
    float    last_heart;
    uint32_t last_heart_at_ms;
    uint32_t last_distance_mm;
    uint32_t last_distance_at_ms;
    bool     human_flag;
    uint32_t human_flag_at_ms;
    uint32_t last_any_frame_at_ms;

    /* Rolling window of breath-phase samples (amplitude + rate estimate). */
    float    phase_val[BHA2_PHASE_CAP];
    uint32_t phase_at_ms[BHA2_PHASE_CAP];
    uint16_t phase_head;     /* index of oldest entry (cap > 255 ⇒ 16-bit) */
    uint16_t phase_count;

    /* Phase-derived breath rate (breath_freq.c) — the published breath_rpm. */
    float    last_freq_rpm;
    bool     last_freq_valid;
    uint32_t last_freq_at_ms;  /* time of the last VALID estimate (staleness)  */
    uint32_t last_est_ms;      /* throttle: re-estimate at most ~2 Hz          */
} Bha2Ctx;

static Bha2Ctx s_bha2_ctx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint8_t xor_cksum(const uint8_t *buf, size_t n) {
    uint8_t x = 0;
    for (size_t i = 0; i < n; i++) x ^= buf[i];
    return (uint8_t)~x;
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0]
         | (uint32_t)((uint32_t)p[1] << 8)
         | (uint32_t)((uint32_t)p[2] << 16)
         | (uint32_t)((uint32_t)p[3] << 24);
}

static float le_float(const uint8_t *p) {
    uint32_t raw = le32(p);
    float f;
    memcpy(&f, &raw, sizeof(f));
    return f;
}

/* Wait for one byte with timeout. Uses vTaskDelay(1) when the FIFO is empty
 * so lower-priority tasks (and the cyw43_arch_lwip_threadsafe_background
 * alarm path) actually get CPU. taskYIELD() is a no-op here because the
 * reader task is the highest-priority task on its core; that would let it
 * spin-saturate the core and starve everything else (observed: MQTT publishes
 * hang within seconds once the reader task is the priority-3 holder).
 *
 * 1 tick = 1 ms with the default configTICK_RATE_HZ. At 115200 baud that's
 * up to 11 bytes per delay, well within the Pico's 32-entry UART RX FIFO. */
static bool read_byte(uart_inst_t *uart, uint8_t *out, uint32_t timeout_ms) {
    uint32_t deadline = now_ms() + timeout_ms;
    while (!uart_is_readable(uart)) {
        if (now_ms() >= deadline) return false;
        vTaskDelay(1);
    }
    *out = (uint8_t)uart_getc(uart);
    return true;
}

/* ── Breath-phase amplitude ring (ADR-0006) ──────────────────────────────── */

static void phase_reset(Bha2Ctx *c) {
    c->phase_head  = 0;
    c->phase_count = 0;
}

static void phase_push(Bha2Ctx *c, float v, uint32_t t) {
    if (!isfinite(v)) return;
    if (c->phase_count == BHA2_PHASE_CAP) {           /* full: drop oldest */
        c->phase_head = (uint16_t)((c->phase_head + 1U) % BHA2_PHASE_CAP);
        c->phase_count--;
    }
    uint16_t idx = (uint16_t)((c->phase_head + c->phase_count) % BHA2_PHASE_CAP);
    c->phase_val[idx]   = v;
    c->phase_at_ms[idx] = t;
    c->phase_count++;
    /* Time-bound the ring to KEEP_MS so the rate estimator sees a fixed history
     * regardless of frame rate. Both consumers sub-select from this. */
    while (c->phase_count > 0U &&
           (uint32_t)(t - c->phase_at_ms[c->phase_head]) > BHA2_PHASE_KEEP_MS) {
        c->phase_head = (uint16_t)((c->phase_head + 1U) % BHA2_PHASE_CAP);
        c->phase_count--;
    }
}

/* Peak-to-peak amplitude of the breath-phase over the last AMP_WIN_MS (a
 * sub-window of the ring; ring eviction itself happens in phase_push).  Sets
 * *valid only when that window is well-formed: enough samples, the newest is
 * fresh, and it spans a slow breath cycle (so a slow breath isn't mistaken for
 * a hold).  A stalled phase stream (UART dropout) ages out → *valid=false → no
 * false hold. */
static float phase_amplitude(Bha2Ctx *c, uint32_t now, bool *valid) {
    *valid = false;
    float lo = 0.0f, hi = 0.0f;
    uint32_t oldest = 0, newest = 0;
    int cnt = 0;
    for (uint16_t i = 0; i < c->phase_count; i++) {
        uint16_t idx = (uint16_t)((c->phase_head + i) % BHA2_PHASE_CAP);
        uint32_t ts = c->phase_at_ms[idx];
        if ((uint32_t)(now - ts) > BHA2_AMP_WIN_MS) {
            continue;                       /* older than the amplitude window */
        }
        float x = c->phase_val[idx];
        if (cnt == 0) {
            lo = hi = x; oldest = newest = ts;
        } else {
            if (x < lo) lo = x;
            if (x > hi) hi = x;
            if (ts > newest) newest = ts;
            if (ts < oldest) oldest = ts;
        }
        cnt++;
    }
    if (cnt < (int)BHA2_PHASE_MIN_N) {
        return 0.0f;
    }
    if ((uint32_t)(now - newest) > BHA2_PHASE_FRESH_MS) {
        return 0.0f;
    }
    if ((newest - oldest) < BHA2_PHASE_MIN_SPAN_MS) {
        return 0.0f;
    }
    *valid = true;
    return hi - lo;
}

/* ── Frame dispatch ──────────────────────────────────────────────────────── */

static void handle_frame(Bha2Ctx *c, uint16_t type, const uint8_t *data,
                         uint16_t len) {
    uint32_t t = now_ms();
    c->last_any_frame_at_ms = t;

    switch (type) {
    case TYPE_BREATH:
        if (len >= 4) {
            c->last_breath = le_float(data);
            c->last_breath_at_ms = t;
#if BHA2_CAPTURE
            LOG_I("CAP,B,%lu,%.3f", (unsigned long)t, (double)c->last_breath);
#endif
        }
        break;

    case TYPE_HEART:
        if (len >= 4) {
            c->last_heart = le_float(data);
            c->last_heart_at_ms = t;
        }
        break;

    case TYPE_DISTANCE:
        if (len >= 8) {
            uint32_t flag = le32(data);
            float cm = le_float(data + 4);
            if (flag == 1u && cm > 0.0f) {
                c->last_distance_mm = (uint32_t)(cm * 10.0f);
                c->last_distance_at_ms = t;
#if BHA2_CAPTURE
                LOG_I("CAP,D,%lu,%lu", (unsigned long)t,
                      (unsigned long)c->last_distance_mm);
#endif
            } else {
                /* "no valid target" — clear distance immediately so we don't
                 * keep reporting a stale one while the radar says nobody. */
                c->last_distance_mm = 0;
                c->last_distance_at_ms = t;
            }
        }
        break;

    case TYPE_HUMAN:
        if (len >= 1) {
            c->human_flag = (data[0] != 0);
            c->human_flag_at_ms = t;
        }
        break;

    case TYPE_PHASE:
        /* 3 little-endian floats: [total][breath][heart].  The breath phase
         * (offset 4) is the chest-displacement signal; ring it for the
         * amplitude used by phase-based breath-hold detection (ADR-0006). */
        if (len >= 12) {
            float ph_breath = le_float(data + 4);
            phase_push(c, ph_breath, t);
#if BHA2_CAPTURE
            LOG_I("CAP,P,%lu,%.4f,%.4f,%.4f", (unsigned long)t,
                  (double)le_float(data), (double)ph_breath,
                  (double)le_float(data + 8));
#endif
        }
        break;

    default:
        /* Unknown types: just count them as activity for the staleness
         * heuristic (handled by last_any_frame_at_ms above). */
        break;
    }
}

/* ── Frame parser ────────────────────────────────────────────────────────── */

/* Pull bytes from the UART until either the deadline hits or one complete
 * frame is parsed and handled. Returns true if at least one frame was
 * processed in this call (handler invoked), false on timeout. Resync on any
 * checksum or framing error and keep going. */
static bool process_one_frame(Bha2Ctx *c, uint32_t deadline_ms) {
    uint8_t  header[BHA2_HEADER_LEN];
    uint8_t  data[BHA2_MAX_PAYLOAD];

    /* Hunt for SOF. */
    for (;;) {
        if (now_ms() >= deadline_ms) return false;
        uint32_t slice = deadline_ms - now_ms();
        if (slice > 10) slice = 10;
        if (!read_byte(c->uart, &header[0], slice)) continue;
        if (header[0] == BHA2_SOF) break;
        /* otherwise: keep hunting */
    }

    /* Read the remaining 7 header bytes. */
    for (size_t i = 1; i < BHA2_HEADER_LEN; i++) {
        if (now_ms() >= deadline_ms) return false;
        if (!read_byte(c->uart, &header[i], deadline_ms - now_ms())) return false;
    }

    uint8_t expected_hdr = xor_cksum(header, 7);
    if (expected_hdr != header[7]) {
        LOG_W("hdr cksum mismatch (got 0x%02x exp 0x%02x)", header[7], expected_hdr);
        return false;   /* resync on next call */
    }

    uint16_t pay_len = be16(&header[3]);
    uint16_t type    = be16(&header[5]);

    if (pay_len > BHA2_MAX_PAYLOAD) {
        LOG_W("payload too long: %u", (unsigned)pay_len);
        return false;
    }

    for (uint16_t i = 0; i < pay_len; i++) {
        if (now_ms() >= deadline_ms) return false;
        if (!read_byte(c->uart, &data[i], deadline_ms - now_ms())) return false;
    }

    /* Data checksum byte. */
    uint8_t got_cksum = 0;
    if (now_ms() >= deadline_ms) return false;
    if (!read_byte(c->uart, &got_cksum, deadline_ms - now_ms())) return false;

    uint8_t expected_data = xor_cksum(data, pay_len);
    if (got_cksum != expected_data) {
        LOG_W("data cksum mismatch (got 0x%02x exp 0x%02x) type=0x%04x",
              got_cksum, expected_data, (unsigned)type);
        return false;
    }

    handle_frame(c, type, data, pay_len);
    return true;
}

/* ── Vtable ──────────────────────────────────────────────────────────────── */

static err_t bha2_init(void *ctx, uart_inst_t *uart) {
    Bha2Ctx *c = (Bha2Ctx *)ctx;
    if (c->initialised) return ERR_OK;

    c->uart = uart;
    uart_init(uart, BOARD_RADAR_BAUD);
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);
    gpio_set_function(BOARD_RADAR_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BOARD_RADAR_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(uart, true);

    c->last_breath = 0.0f;
    c->last_heart  = 0.0f;
    c->last_distance_mm = 0;
    c->human_flag = false;
    c->last_breath_at_ms = c->last_heart_at_ms = c->last_distance_at_ms =
        c->human_flag_at_ms = c->last_any_frame_at_ms = 0;
    c->last_freq_rpm = 0.0f;
    c->last_freq_valid = false;
    c->last_freq_at_ms = 0;
    c->last_est_ms = 0;
    phase_reset(c);

    c->initialised = true;
    LOG_I("init OK on uart %p @ %u baud (BHA2 protocol)", (void *)uart,
          (unsigned)BOARD_RADAR_BAUD);
    return ERR_OK;
}

static err_t bha2_read_sample(void *ctx, RadarSample *out, uint32_t timeout_ms) {
    Bha2Ctx *c = (Bha2Ctx *)ctx;
    if (!c->initialised) return ERR_NOT_INIT;

    uint32_t deadline = now_ms() + timeout_ms;
    bool any = false;
    /* Drain frames while there's time. The module pushes ~1 frame every
     * ~100 ms so a 2 s timeout typically yields a dozen frames; we want the
     * latest of each kind. */
    while (now_ms() < deadline) {
        if (process_one_frame(c, deadline)) {
            any = true;
        } else {
            /* No more bytes available or transient parse error — bail out
             * with whatever we've latched so far. */
            break;
        }
    }

    uint32_t t = now_ms();
    memset(out, 0, sizeof(*out));

    bool heart_fresh    = (c->last_heart_at_ms    != 0) && (t - c->last_heart_at_ms    < RADAR_STALE_MS);
    bool distance_fresh = (c->last_distance_at_ms != 0) && (t - c->last_distance_at_ms < RADAR_STALE_MS);
    bool human_fresh    = (c->human_flag_at_ms    != 0) && (t - c->human_flag_at_ms    < RADAR_STALE_MS);
    bool frames_fresh   = (c->last_any_frame_at_ms != 0)
                       && (t - c->last_any_frame_at_ms < RADAR_STALE_MS);

    /* Breath rate from our own phase-waveform estimator (breath_freq.c), NOT
     * the radar's 0x0A14 BR — that field over-reports at slow breathing. The
     * estimate is throttled to ~2 Hz (it scans up to 30 s of ring each run) and
     * blanked when its spectral peak isn't prominent (just noise / nobody). */
    if (c->last_est_ms == 0 || (uint32_t)(t - c->last_est_ms) >= 500U) {
        BreathFreqResult bf = breath_freq_estimate(
            c->phase_val, c->phase_at_ms, (int)BHA2_PHASE_CAP,
            (int)c->phase_head, (int)c->phase_count, t);
        c->last_freq_rpm   = bf.rpm;
        c->last_freq_valid = bf.valid;
        if (bf.valid) {
            c->last_freq_at_ms = t;
        }
        c->last_est_ms = t;
#if BHA2_CAPTURE
        LOG_I("CAP,E,%lu,%.2f,%.2f,%d", (unsigned long)t,
              (double)bf.rpm, (double)bf.confidence, bf.valid ? 1 : 0);
#endif
    }
    bool breath_avail = c->last_freq_valid &&
                        ((uint32_t)(t - c->last_freq_at_ms) < RADAR_STALE_MS);

    out->breath_rpm  = breath_avail   ? c->last_freq_rpm    : 0.0f;
    out->heart_bpm   = heart_fresh    ? c->last_heart       : 0.0f;
    out->distance_mm = distance_fresh ? c->last_distance_mm : 0u;
    /* Presence: trust the explicit 0x0F09 flag when fresh, otherwise derive
     * it from "anything arrived recently". */
    out->presence    = human_fresh ? c->human_flag : frames_fresh;

    /* Breath-phase amplitude for the filter's hold detection (ADR-0006). */
    bool amp_valid = false;
    out->resp_motion_amp       = phase_amplitude(c, t, &amp_valid);
    out->resp_motion_amp_valid = amp_valid;

    /* Quality:
     *  3 invalid  — no frames at all in this call AND no fresh vital
     *  2 degraded — presence but no breath/heart current (ghost / startup /
     *               estimator still warming up its 30 s window)
     *  0 ok       — at least one of breath_rpm / heart_bpm is current        */
    if (!any && !breath_avail && !heart_fresh) {
        out->q = 3;
        return ERR_TIMEOUT;
    }
    if (out->presence && !breath_avail && !heart_fresh) {
        out->q = 2;
    } else {
        out->q = 0;
    }
    return ERR_OK;
}

static err_t bha2_close(void *ctx) {
    Bha2Ctx *c = (Bha2Ctx *)ctx;
    if (c->initialised) {
        uart_deinit(c->uart);
        c->initialised = false;
    }
    return ERR_OK;
}

static radar_driver_t s_bha2_driver = {
    .init        = bha2_init,
    .read_sample = bha2_read_sample,
    .close       = bha2_close,
    .name        = "MR60BHA2",
    .ctx         = &s_bha2_ctx,
};

radar_driver_t *radar_bha2_driver(void) { return &s_bha2_driver; }
