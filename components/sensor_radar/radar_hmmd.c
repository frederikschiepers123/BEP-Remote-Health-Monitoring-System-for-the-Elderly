#define LOG_TAG "HMMD"
#include "log.h"

#include "radar_driver.h"
#include "board_pico2wh.h"
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

/*
 * Waveshare HMMD (Human Micro-Motion Detection) 24 GHz radar driver — second
 * radar behind the radar_driver_t v-table (CLAUDE.md §7.4, ADR-0007). Same
 * RadarSample, same rmms/<uuid>/radar topic, same §9.2 schema; radar_task is
 * unaware which radar is attached. Selected by /cfg/sensors.json "radar":"hmmd".
 *
 * Protocol is taken from the previous BAP group's working MicroPython driver,
 * which CLAUDE.md §18 designates the behavioural specification
 * (bestanden_vorige_BAP/.../lib/hmmd_mpy.py — Waveshare HMMD "Report Mode"):
 *
 *   Header : F4 F3 F2 F1
 *   Length : 2 bytes, LITTLE-endian (length of the payload only)
 *   Payload:
 *     [0]      presence       1 B  (0x00 = no target, 0x01 = target)
 *     [1..2]   distance_raw   2 B  little-endian
 *     [3..34]  gate energies  16 × 2 B little-endian (motion magnitude per gate)
 *   Tail   : F8 F7 F6 F5
 *
 * There is NO checksum — the fixed header + fixed tail + length delimit and
 * validate the frame. The module must first be put into Report Mode with a
 * one-shot command (sent in init).
 *
 * IMPORTANT capability difference vs the MR60BHA2: the HMMD reports presence,
 * distance, and per-gate motion energy ONLY — it has NO respiration or heart
 * rate, and no breath-phase signal. So this driver always emits breath_rpm = 0
 * and heart_bpm = 0 (→ null on the wire, §9.2.2) and leaves
 * resp_motion_amp_valid = false, so the ADR-0006 breath-hold feature stays
 * inert. This is exactly the graceful degradation radar_driver.h documents for
 * "a radar with no phase stream"; on an HMMD sensor module the mirror's
 * heart/breath tiles simply have no data, while presence + distance (ADL /
 * presence monitoring, the generic module's job per ADR-0001) work fully.
 */

/* ── Framing constants ───────────────────────────────────────────────────── */

static const uint8_t HMMD_HEADER[4] = {0xF4, 0xF3, 0xF2, 0xF1};
static const uint8_t HMMD_TAIL[4]   = {0xF8, 0xF7, 0xF6, 0xF5};

/* One-shot command that switches the module into Report Mode (verbatim from the
 * reference driver). Sent once in init; the module then streams report frames. */
static const uint8_t HMMD_CMD_REPORT_MODE[] = {
    0xFD, 0xFC, 0xFB, 0xFA, 0x08, 0x00, 0x12, 0x00,
    0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x04, 0x03, 0x02, 0x01,
};

#define HMMD_MAX_PAYLOAD   64U   /* real frame is 35 B (1 + 2 + 32); headroom   */
#define HMMD_MIN_PAYLOAD    3U   /* need at least presence + 2-byte distance     */

/* Distance unit: the report frame carries a 2-byte little-endian raw distance.
 * The reference's distance_to_meters() multiplied it by a 0.7 m "gate length",
 * but on this LD2410-class silicon the field is the detection distance in
 * CENTIMETRES, which yields physically plausible indoor values; we convert
 * cm→mm. TODO(spec): confirm the unit on the bench against a tape measure and
 * strip this marker (CLAUDE.md §16 idiom). The downstream plausibility filter
 * (ADR-0005) gates absurd distances regardless. */
#define HMMD_DIST_MM_PER_RAW  10U

/* A latched value is stale (excluded from the next sample) after this long
 * without an update — same policy as radar_bha2.c. */
#define RADAR_STALE_MS      5000U

/* ── Driver context ──────────────────────────────────────────────────────── */

typedef struct {
    uart_inst_t *uart;
    bool         initialised;

    bool     human_flag;
    uint32_t human_flag_at_ms;
    uint32_t last_distance_mm;
    uint32_t last_distance_at_ms;
    uint32_t last_any_frame_at_ms;
} HmmdCtx;

static HmmdCtx s_hmmd_ctx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

/* Wait for one byte with timeout. vTaskDelay(1) when the FIFO is empty so
 * lower-priority tasks get CPU — see the rationale in radar_bha2.c read_byte(). */
static bool read_byte(uart_inst_t *uart, uint8_t *out, uint32_t timeout_ms) {
    uint32_t deadline = now_ms() + timeout_ms;
    while (!uart_is_readable(uart)) {
        if (now_ms() >= deadline) return false;
        vTaskDelay(1);
    }
    *out = (uint8_t)uart_getc(uart);
    return true;
}

/* ── Frame parser ────────────────────────────────────────────────────────── */

/* Pull bytes until either the deadline hits or one complete report frame is
 * parsed and latched. Returns true if a frame was handled, false on timeout.
 * Resync on any framing error (bad tail / oversized length) and keep going. */
static bool process_one_frame(HmmdCtx *c, uint32_t deadline_ms) {
    /* Hunt for the 4-byte header F4 F3 F2 F1 with a sliding match index. */
    size_t idx = 0;
    while (idx < 4) {
        if (now_ms() >= deadline_ms) return false;
        uint32_t slice = deadline_ms - now_ms();
        if (slice > 10) slice = 10;
        uint8_t b;
        if (!read_byte(c->uart, &b, slice)) continue;
        if (b == HMMD_HEADER[idx]) {
            idx++;
        } else {
            /* Restart the match; a byte equal to HEADER[0] starts a new run. */
            idx = (b == HMMD_HEADER[0]) ? 1u : 0u;
        }
    }

    /* Length (2 bytes, little-endian). */
    uint8_t len_b[2];
    for (size_t i = 0; i < 2; i++) {
        if (now_ms() >= deadline_ms) return false;
        if (!read_byte(c->uart, &len_b[i], deadline_ms - now_ms())) return false;
    }
    uint16_t pay_len = le16(len_b);
    if (pay_len < HMMD_MIN_PAYLOAD || pay_len > HMMD_MAX_PAYLOAD) {
        LOG_W("implausible payload length: %u", (unsigned)pay_len);
        return false;
    }

    /* Payload. */
    uint8_t data[HMMD_MAX_PAYLOAD];
    for (uint16_t i = 0; i < pay_len; i++) {
        if (now_ms() >= deadline_ms) return false;
        if (!read_byte(c->uart, &data[i], deadline_ms - now_ms())) return false;
    }

    /* Tail F8 F7 F6 F5. */
    uint8_t tail[4];
    for (size_t i = 0; i < 4; i++) {
        if (now_ms() >= deadline_ms) return false;
        if (!read_byte(c->uart, &tail[i], deadline_ms - now_ms())) return false;
    }
    if (memcmp(tail, HMMD_TAIL, sizeof(tail)) != 0) {
        LOG_W("bad tail (0x%02x 0x%02x 0x%02x 0x%02x)",
              tail[0], tail[1], tail[2], tail[3]);
        return false;
    }

    /* Parse: presence + distance. Gate energies (data[3..]) are motion
     * magnitude; RadarSample has no field for them, so they are validated by
     * length only and not decoded. */
    uint32_t t = now_ms();
    c->last_any_frame_at_ms = t;

    c->human_flag       = (data[0] != 0);
    c->human_flag_at_ms = t;

    uint32_t raw = le16(&data[1]);
    if (c->human_flag && raw > 0u) {
        c->last_distance_mm    = raw * HMMD_DIST_MM_PER_RAW;
        c->last_distance_at_ms = t;
    } else {
        /* No target (or no range) — clear distance so we don't report a stale
         * one while the module says nobody (mirrors radar_bha2.c). */
        c->last_distance_mm    = 0;
        c->last_distance_at_ms = t;
    }
    return true;
}

/* ── Vtable ──────────────────────────────────────────────────────────────── */

static err_t hmmd_init(void *ctx, uart_inst_t *uart) {
    HmmdCtx *c = (HmmdCtx *)ctx;
    if (c->initialised) return ERR_OK;

    c->uart = uart;
    uart_init(uart, BOARD_RADAR_BAUD);
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);
    gpio_set_function(BOARD_RADAR_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BOARD_RADAR_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(uart, true);

    c->human_flag = false;
    c->last_distance_mm = 0;
    c->human_flag_at_ms = c->last_distance_at_ms = c->last_any_frame_at_ms = 0;

    /* Put the module into Report Mode, then let it settle (reference waits
     * ~200 ms; 1 tick ≈ 1 ms at the default configTICK_RATE_HZ). */
    uart_write_blocking(uart, HMMD_CMD_REPORT_MODE, sizeof(HMMD_CMD_REPORT_MODE));
    vTaskDelay(200);

    c->initialised = true;
    LOG_I("init OK on uart %p @ %u baud (HMMD report-mode protocol)",
          (void *)uart, (unsigned)BOARD_RADAR_BAUD);
    return ERR_OK;
}

static err_t hmmd_read_sample(void *ctx, RadarSample *out, uint32_t timeout_ms) {
    HmmdCtx *c = (HmmdCtx *)ctx;
    if (!c->initialised) return ERR_NOT_INIT;

    uint32_t deadline = now_ms() + timeout_ms;
    bool any = false;
    while (now_ms() < deadline) {
        if (process_one_frame(c, deadline)) {
            any = true;
        } else {
            break;
        }
    }

    uint32_t t = now_ms();
    memset(out, 0, sizeof(*out));

    bool distance_fresh = (c->last_distance_at_ms != 0) && (t - c->last_distance_at_ms < RADAR_STALE_MS);
    bool human_fresh    = (c->human_flag_at_ms    != 0) && (t - c->human_flag_at_ms    < RADAR_STALE_MS);
    bool frames_fresh   = (c->last_any_frame_at_ms != 0)
                       && (t - c->last_any_frame_at_ms < RADAR_STALE_MS);

    out->presence    = human_fresh ? c->human_flag : frames_fresh;
    out->distance_mm = distance_fresh ? c->last_distance_mm : 0u;

    /* No respiration / heart / breath-phase on this radar — always unreported
     * (→ null on the wire). The breath-hold feature degrades to inert. */
    out->breath_rpm            = 0.0f;
    out->heart_bpm             = 0.0f;
    out->resp_motion_amp       = 0.0f;
    out->resp_motion_amp_valid = false;

    /* Quality: 3 invalid when nothing arrived and nothing fresh remains, else 0
     * ok. There is no q=2 "present-without-vitals ghost" rung as on the BHA2 —
     * this radar has no vitals by design, so a valid presence/distance frame is
     * simply ok; gross-implausibility gating is the filter's job (ADR-0005). */
    if (!any && !human_fresh) {
        out->q = 3;
        return ERR_TIMEOUT;
    }
    out->q = 0;
    return ERR_OK;
}

static err_t hmmd_close(void *ctx) {
    HmmdCtx *c = (HmmdCtx *)ctx;
    if (c->initialised) {
        uart_deinit(c->uart);
        c->initialised = false;
    }
    return ERR_OK;
}

static radar_driver_t s_hmmd_driver = {
    .init        = hmmd_init,
    .read_sample = hmmd_read_sample,
    .close       = hmmd_close,
    .name        = "HMMD",
    .ctx         = &s_hmmd_ctx,
};

radar_driver_t *radar_hmmd_driver(void) { return &s_hmmd_driver; }
