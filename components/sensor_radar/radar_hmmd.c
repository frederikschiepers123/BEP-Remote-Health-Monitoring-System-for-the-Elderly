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
 * Seeed 24 GHz "HMMD" (Human Micro-Motion Detection) radar driver.
 *
 * This is the SECOND radar behind the radar_driver_t v-table (CLAUDE.md §7.4):
 * a new radar_*.c file plus one line in radar_select.c, with NO change to the
 * task, the MQTT topics, or the §9.2 payload schema.  It exists so the same
 * firmware image runs on a sensor module populated with the previous group's
 * HMMD module instead of the MR60BHA2, selected by /cfg/sensors.json "radar":
 * "hmmd".  See ADR-0007.
 *
 * Framing (the Andar/AI-Thinker / Seeed 24 GHz protocol — the one CLAUDE.md
 * §3.2 explicitly notes the MR60BHA2 does NOT use, i.e. this is the OTHER
 * family):
 *
 *   [0x53][0x59] [CTRL] [CMD] [LEN_H][LEN_L] ── DATA(LEN) ── [CKSUM] [0x54][0x43]
 *
 *   LEN   is big-endian (length of DATA only).
 *   CKSUM is (sum of every byte from 0x53 through the last DATA byte) & 0xFF.
 *   Tail  is the literal 0x54 0x43.
 *
 * The frame ENVELOPE above is the well-documented, stable part and is what the
 * host unit tests exercise rigorously.  The (CTRL, CMD) → field assignment
 * below is firmware-revision-dependent on these modules; the codes are the
 * best-known Seeed mapping, defined as named constants and marked TODO(spec)
 * so a bench session can correct one number without touching the parser.  This
 * mirrors how the BHA2 driver was treated before its bench bring-up resolved
 * §16 Q2.
 *
 * The module pushes report frames at its own cadence; read_sample() drains
 * whatever is in the UART RX FIFO up to the timeout and returns the latched
 * latest values, exactly like radar_bha2.c.
 *
 * Graceful degradation: an HMMD module has no per-sample chest-displacement
 * (breath-phase) stream, so this driver never produces a resp_motion amplitude
 * — it leaves resp_motion_amp_valid = false and the ADR-0006 breath-hold
 * feature stays inert downstream, exactly as radar_driver.h documents for "a
 * radar with no phase stream".  Likewise heart rate is only emitted if the
 * module actually reports it (24 GHz micro-motion variants often do not); the
 * field then stays 0 → null on the wire, which §9.2.2 explicitly permits.
 */

/* ── Framing constants ───────────────────────────────────────────────────── */

#define HMMD_SOF1          0x53U
#define HMMD_SOF2          0x59U
#define HMMD_EOF1          0x54U
#define HMMD_EOF2          0x43U
#define HMMD_MAX_PAYLOAD   64U

/* (CTRL, CMD) report frames we consume.
 * TODO(spec): bench-confirm these codes against the live HMMD module and strip
 * this marker (CLAUDE.md §16 idiom).  A wrong code is fail-safe: the frame is
 * simply skipped (its bytes still count as activity for presence staleness),
 * so a mis-decoded build degrades to "presence only", never to bad vitals. */
#define HMMD_CTRL_PRESENCE  0x80U
#define HMMD_CMD_PRESENCE   0x01U   /* DATA[0]: 0 = absent, 1 = present        */
#define HMMD_CMD_MOTION     0x02U   /* DATA[0]: 0 none, 1 still, 2 active      */
#define HMMD_CMD_BODYPARAM  0x03U   /* DATA[0]: 0..100 motion magnitude        */
#define HMMD_CMD_DISTANCE   0x0AU   /* DATA: BE16 centimetres to target        */

#define HMMD_CTRL_RESP      0x81U
#define HMMD_CMD_RESP_RATE  0x05U   /* DATA[0]: respiration, breaths/min       */

#define HMMD_CTRL_HEART     0x85U
#define HMMD_CMD_HEART_RATE 0x03U   /* DATA[0]: heart rate, BPM                */

/* A latched value is considered stale (and excluded from the next sample)
 * after this many ms without an update — identical policy to radar_bha2.c. */
#define RADAR_STALE_MS      5000U

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
} HmmdCtx;

static HmmdCtx s_hmmd_ctx;

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static inline uint32_t now_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] << 8 | (uint16_t)p[1]);
}

/* Wait for one byte with timeout. Uses vTaskDelay(1) when the FIFO is empty so
 * lower-priority tasks (and the cyw43_arch_lwip_threadsafe_background alarm
 * path) get CPU — see the rationale in radar_bha2.c read_byte(). */
static bool read_byte(uart_inst_t *uart, uint8_t *out, uint32_t timeout_ms) {
    uint32_t deadline = now_ms() + timeout_ms;
    while (!uart_is_readable(uart)) {
        if (now_ms() >= deadline) return false;
        vTaskDelay(1);
    }
    *out = (uint8_t)uart_getc(uart);
    return true;
}

/* ── Frame dispatch ──────────────────────────────────────────────────────── */

static void handle_frame(HmmdCtx *c, uint8_t ctrl, uint8_t cmd,
                         const uint8_t *data, uint16_t len) {
    uint32_t t = now_ms();
    c->last_any_frame_at_ms = t;

    if (ctrl == HMMD_CTRL_PRESENCE) {
        switch (cmd) {
        case HMMD_CMD_PRESENCE:
            if (len >= 1) {
                c->human_flag = (data[0] != 0);
                c->human_flag_at_ms = t;
            }
            break;
        case HMMD_CMD_DISTANCE:
            if (len >= 2) {
                uint32_t cm = be16(data);
                if (cm > 0u) {
                    c->last_distance_mm = cm * 10u;
                    c->last_distance_at_ms = t;
                } else {
                    /* explicit "no target" — clear so we don't report a stale
                     * distance while the module says nobody (mirrors BHA2). */
                    c->last_distance_mm = 0;
                    c->last_distance_at_ms = t;
                }
            }
            break;
        default:
            /* MOTION / BODYPARAM and friends: count as activity only. */
            break;
        }
    } else if (ctrl == HMMD_CTRL_RESP && cmd == HMMD_CMD_RESP_RATE) {
        if (len >= 1) {
            c->last_breath = (float)data[0];
            c->last_breath_at_ms = t;
        }
    } else if (ctrl == HMMD_CTRL_HEART && cmd == HMMD_CMD_HEART_RATE) {
        if (len >= 1) {
            c->last_heart = (float)data[0];
            c->last_heart_at_ms = t;
        }
    }
    /* Any other (CTRL, CMD): tolerated, skipped — already counted as activity. */
}

/* ── Frame parser ────────────────────────────────────────────────────────── */

/* Pull bytes from the UART until either the deadline hits or one complete frame
 * is parsed and handled. Returns true if a frame was handled, false on timeout.
 * Resync on any checksum / tail / framing error and keep going. */
static bool process_one_frame(HmmdCtx *c, uint32_t deadline_ms) {
    /* Hunt for the 2-byte header 0x53 0x59 with a 1-byte sliding window so
     * runs like 0x53 0x53 0x59 still sync correctly. */
    uint8_t prev = 0, cur = 0;
    bool have_prev = false;
    for (;;) {
        if (now_ms() >= deadline_ms) return false;
        uint32_t slice = deadline_ms - now_ms();
        if (slice > 10) slice = 10;
        if (!read_byte(c->uart, &cur, slice)) continue;
        if (have_prev && prev == HMMD_SOF1 && cur == HMMD_SOF2) break;
        prev = cur;
        have_prev = true;
    }

    /* CTRL, CMD, LEN_H, LEN_L. */
    uint8_t hdr[4];
    for (size_t i = 0; i < 4; i++) {
        if (now_ms() >= deadline_ms) return false;
        if (!read_byte(c->uart, &hdr[i], deadline_ms - now_ms())) return false;
    }
    uint8_t  ctrl    = hdr[0];
    uint8_t  cmd     = hdr[1];
    uint16_t pay_len = be16(&hdr[2]);

    if (pay_len > HMMD_MAX_PAYLOAD) {
        LOG_W("payload too long: %u", (unsigned)pay_len);
        return false;
    }

    uint8_t data[HMMD_MAX_PAYLOAD];
    for (uint16_t i = 0; i < pay_len; i++) {
        if (now_ms() >= deadline_ms) return false;
        if (!read_byte(c->uart, &data[i], deadline_ms - now_ms())) return false;
    }

    /* Checksum byte: sum of header[0..1] + CTRL + CMD + LEN_H + LEN_L + DATA. */
    uint8_t got_cksum = 0;
    if (now_ms() >= deadline_ms) return false;
    if (!read_byte(c->uart, &got_cksum, deadline_ms - now_ms())) return false;

    uint32_t sum = (uint32_t)HMMD_SOF1 + (uint32_t)HMMD_SOF2
                 + (uint32_t)ctrl + (uint32_t)cmd
                 + (uint32_t)hdr[2] + (uint32_t)hdr[3];
    for (uint16_t i = 0; i < pay_len; i++) sum += (uint32_t)data[i];
    uint8_t expected = (uint8_t)(sum & 0xFFu);
    if (got_cksum != expected) {
        LOG_W("cksum mismatch (got 0x%02x exp 0x%02x) ctrl=0x%02x cmd=0x%02x",
              got_cksum, expected, ctrl, cmd);
        return false;
    }

    /* Tail 0x54 0x43. */
    uint8_t tail[2];
    for (size_t i = 0; i < 2; i++) {
        if (now_ms() >= deadline_ms) return false;
        if (!read_byte(c->uart, &tail[i], deadline_ms - now_ms())) return false;
    }
    if (tail[0] != HMMD_EOF1 || tail[1] != HMMD_EOF2) {
        LOG_W("bad tail (got 0x%02x 0x%02x)", tail[0], tail[1]);
        return false;
    }

    handle_frame(c, ctrl, cmd, data, pay_len);
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

    c->last_breath = 0.0f;
    c->last_heart  = 0.0f;
    c->last_distance_mm = 0;
    c->human_flag = false;
    c->last_breath_at_ms = c->last_heart_at_ms = c->last_distance_at_ms =
        c->human_flag_at_ms = c->last_any_frame_at_ms = 0;

    c->initialised = true;
    LOG_I("init OK on uart %p @ %u baud (HMMD protocol)", (void *)uart,
          (unsigned)BOARD_RADAR_BAUD);
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

    bool breath_fresh   = (c->last_breath_at_ms   != 0) && (t - c->last_breath_at_ms   < RADAR_STALE_MS);
    bool heart_fresh    = (c->last_heart_at_ms    != 0) && (t - c->last_heart_at_ms    < RADAR_STALE_MS);
    bool distance_fresh = (c->last_distance_at_ms != 0) && (t - c->last_distance_at_ms < RADAR_STALE_MS);
    bool human_fresh    = (c->human_flag_at_ms    != 0) && (t - c->human_flag_at_ms    < RADAR_STALE_MS);
    bool frames_fresh   = (c->last_any_frame_at_ms != 0)
                       && (t - c->last_any_frame_at_ms < RADAR_STALE_MS);

    out->breath_rpm  = breath_fresh   ? c->last_breath      : 0.0f;
    out->heart_bpm   = heart_fresh    ? c->last_heart       : 0.0f;
    out->distance_mm = distance_fresh ? c->last_distance_mm : 0u;
    out->presence    = human_fresh ? c->human_flag : frames_fresh;

    /* No breath-phase stream on this radar — leave the ADR-0006 amplitude
     * unreported so the breath-hold feature degrades to inert downstream. */
    out->resp_motion_amp       = 0.0f;
    out->resp_motion_amp_valid = false;

    /* Quality — same ladder as radar_bha2.c:
     *  3 invalid  — no frame this call AND no fresh latched vitals
     *  2 degraded — presence but no fresh breath/heart (ghost / startup)
     *  0 ok       — at least one of breath_rpm / heart_bpm is current        */
    if (!any && !breath_fresh && !heart_fresh) {
        out->q = 3;
        return ERR_TIMEOUT;
    }
    if (out->presence && !breath_fresh && !heart_fresh) {
        out->q = 2;
    } else {
        out->q = 0;
    }
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
