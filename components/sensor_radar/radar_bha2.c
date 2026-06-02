#define LOG_TAG "BHA2"
#include "log.h"

#include "radar_driver.h"
#include "board_pico2wh.h"
#include "err.h"

#include "hardware/uart.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#include "FreeRTOS.h"
#include "task.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/*
 * Seeed MR60BHA2 radar driver — 60 GHz heartbeat + breath detection.
 *
 * Frame format (Andar/AI-Thinker mmWave family):
 *   [0x53][0x59][CON][CMD][LEN_H][LEN_L][payload...][CKSUM][0x54][0x43]
 *
 * Checksum = 8-bit sum of all bytes from 0x53 through end of payload.
 *
 * TODO(spec): The command table below is based on the Andar framing family
 * which the DFRobot audit confirmed.  MR60BHA2-specific commands must be
 * verified on bench before treating this driver as production-ready.
 * See CLAUDE.md §3.2 note on shared framing and §16 Q3.
 *
 * Ghost detection heuristic: if presence=true but heart_bpm == 0 and
 * breath_rpm == 0, set q=2 (degraded).
 */

/* ── Frame constants ─────────────────────────────────────────────────────── */

#define FRAME_HEADER_0  0x53U
#define FRAME_HEADER_1  0x59U
#define FRAME_TAIL_0    0x54U
#define FRAME_TAIL_1    0x43U

#define FRAME_MAX_PAYLOAD  64U

/* Known command identifiers (TODO(spec): verify on bench for BHA2) */
#define CON_PRESENCE    0x80U
#define CMD_PRESENCE    0x81U
#define CON_BREATH      0x81U
#define CMD_BREATH      0x82U
#define CON_HEART       0x85U
#define CMD_HEART       0x82U

/* ── Driver context ──────────────────────────────────────────────────────── */

typedef struct {
    uart_inst_t *uart;
    bool         initialised;
} Bha2Ctx;

static Bha2Ctx s_bha2_ctx;

/* ── UART byte-level helpers ─────────────────────────────────────────────── */

static bool uart_read_byte_timeout(uart_inst_t *uart, uint8_t *out,
                                   uint32_t timeout_ms)
{
    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_ms;
    while (!uart_is_readable(uart)) {
        if (to_ms_since_boot(get_absolute_time()) >= deadline) {
            return false;
        }
        taskYIELD();
    }
    *out = (uint8_t)uart_getc(uart);
    return true;
}

/* ── Frame parser state machine ──────────────────────────────────────────── */

typedef enum {
    FRAME_STATE_WAIT_HEADER0 = 0,
    FRAME_STATE_WAIT_HEADER1,
    FRAME_STATE_CON,
    FRAME_STATE_CMD,
    FRAME_STATE_LEN_H,
    FRAME_STATE_LEN_L,
    FRAME_STATE_PAYLOAD,
    FRAME_STATE_CKSUM,
    FRAME_STATE_TAIL0,
    FRAME_STATE_TAIL1,
} FrameState;

typedef struct {
    uint8_t con;
    uint8_t cmd;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    uint16_t payload_len;
    uint8_t cksum;
} ParsedFrame;

/**
 * Read bytes from UART until a complete Andar frame is received or timeout.
 *
 * @param uart       UART instance.
 * @param frame_out  Output frame (filled on success).
 * @param timeout_ms Total time budget.
 * @return ERR_OK, ERR_TIMEOUT, or ERR_IO (checksum mismatch).
 */
static err_t recv_frame(uart_inst_t *uart, ParsedFrame *frame_out,
                        uint32_t timeout_ms)
{
    FrameState state    = FRAME_STATE_WAIT_HEADER0;
    uint16_t   pay_idx  = 0;
    uint16_t   pay_len  = 0;
    uint8_t    cksum    = 0;    /* running 8-bit sum */
    uint8_t    con      = 0;
    uint8_t    cmd      = 0;
    uint8_t    payload[FRAME_MAX_PAYLOAD];
    uint8_t    byte;

    uint32_t deadline = to_ms_since_boot(get_absolute_time()) + timeout_ms;

    while (true) {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now >= deadline) {
            return ERR_TIMEOUT;
        }
        uint32_t remaining = deadline - now;
        if (!uart_read_byte_timeout(uart, &byte, remaining > 10 ? 10 : remaining)) {
            continue;
        }

        switch (state) {
        case FRAME_STATE_WAIT_HEADER0:
            if (byte == FRAME_HEADER_0) {
                cksum = byte;
                state = FRAME_STATE_WAIT_HEADER1;
            }
            break;

        case FRAME_STATE_WAIT_HEADER1:
            if (byte == FRAME_HEADER_1) {
                cksum = (uint8_t)(cksum + byte);
                state = FRAME_STATE_CON;
            } else {
                /* Resync */
                cksum = 0;
                state = FRAME_STATE_WAIT_HEADER0;
            }
            break;

        case FRAME_STATE_CON:
            con    = byte;
            cksum  = (uint8_t)(cksum + byte);
            state  = FRAME_STATE_CMD;
            break;

        case FRAME_STATE_CMD:
            cmd   = byte;
            cksum = (uint8_t)(cksum + byte);
            state = FRAME_STATE_LEN_H;
            break;

        case FRAME_STATE_LEN_H:
            pay_len = (uint16_t)((uint16_t)byte << 8);
            cksum   = (uint8_t)(cksum + byte);
            state   = FRAME_STATE_LEN_L;
            break;

        case FRAME_STATE_LEN_L:
            pay_len = (uint16_t)(pay_len | byte);
            cksum   = (uint8_t)(cksum + byte);
            pay_idx = 0;
            if (pay_len == 0) {
                state = FRAME_STATE_CKSUM;
            } else if (pay_len > FRAME_MAX_PAYLOAD) {
                LOG_W("BHA2 frame payload too long: %u", pay_len);
                state = FRAME_STATE_WAIT_HEADER0;
            } else {
                state = FRAME_STATE_PAYLOAD;
            }
            break;

        case FRAME_STATE_PAYLOAD:
            if (pay_idx < FRAME_MAX_PAYLOAD) {
                payload[pay_idx] = byte;
            }
            cksum = (uint8_t)(cksum + byte);
            pay_idx++;
            if (pay_idx >= pay_len) {
                state = FRAME_STATE_CKSUM;
            }
            break;

        case FRAME_STATE_CKSUM: {
            /* Verify checksum */
            uint8_t expected = cksum;  /* accumulated sum up to end of payload */
            if (byte != expected) {
                LOG_W("BHA2 checksum mismatch: got 0x%02X expected 0x%02X",
                      byte, expected);
                state = FRAME_STATE_WAIT_HEADER0;
                break;
            }
            state = FRAME_STATE_TAIL0;
            break;
        }

        case FRAME_STATE_TAIL0:
            if (byte == FRAME_TAIL_0) {
                state = FRAME_STATE_TAIL1;
            } else {
                state = FRAME_STATE_WAIT_HEADER0;
            }
            break;

        case FRAME_STATE_TAIL1:
            if (byte == FRAME_TAIL_1) {
                /* Complete frame received */
                frame_out->con         = con;
                frame_out->cmd         = cmd;
                frame_out->payload_len = pay_len;
                if (pay_len > 0) {
                    memcpy(frame_out->payload, payload,
                           (pay_len < FRAME_MAX_PAYLOAD) ? pay_len
                                                         : FRAME_MAX_PAYLOAD);
                }
                return ERR_OK;
            }
            state = FRAME_STATE_WAIT_HEADER0;
            break;
        }
    }
}

/* ── Driver vtable implementation ────────────────────────────────────────── */

static err_t bha2_init(void *ctx, uart_inst_t *uart)
{
    Bha2Ctx *c = (Bha2Ctx *)ctx;
    c->uart = uart;

    uart_init(uart, BOARD_RADAR_BAUD);
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);
    gpio_set_function(BOARD_RADAR_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BOARD_RADAR_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(uart, true);

    c->initialised = true;
    LOG_I("BHA2 init OK at %u baud", BOARD_RADAR_BAUD);
    return ERR_OK;
}

static err_t bha2_read_sample(void *ctx, RadarSample *out, uint32_t timeout_ms)
{
    Bha2Ctx *c = (Bha2Ctx *)ctx;
    if (!c->initialised || !out) {
        return ERR_INVALID_ARG;
    }

    /* Initialise output to safe defaults. */
    out->presence    = false;
    out->distance_mm = 0;
    out->breath_rpm  = 0.0f;
    out->heart_bpm   = 0.0f;
    out->q           = 3;   /* invalid until we get a valid frame */

    /*
     * The MR60BHA2 transmits frames autonomously; we read the next frame
     * from the UART FIFO within the timeout budget.
     *
     * We accept one frame per call and map it to the RadarSample fields.
     * Multiple frames (presence, breath, heart) may arrive; in steady state
     * the caller calls us at ~2 Hz and processes one frame at a time.
     */
    ParsedFrame frame;
    err_t err = recv_frame(c->uart, &frame, timeout_ms);
    if (err == ERR_TIMEOUT) {
        return ERR_TIMEOUT;
    }
    if (err != ERR_OK) {
        return err;
    }

    out->q = 0;

    if (frame.con == CON_PRESENCE && frame.cmd == CMD_PRESENCE) {
        if (frame.payload_len >= 1) {
            out->presence = (frame.payload[0] != 0x00U);
        }
    } else if (frame.con == CON_BREATH && frame.cmd == CMD_BREATH) {
        if (frame.payload_len >= 1) {
            out->presence   = true;
            out->breath_rpm = (float)frame.payload[0];
        }
    } else if (frame.con == CON_HEART && frame.cmd == CMD_HEART) {
        if (frame.payload_len >= 1) {
            out->presence  = true;
            out->heart_bpm = (float)frame.payload[0];
        }
    } else {
        /* Unknown frame — not an error, just not mapped */
        LOG_D("BHA2 unmapped frame con=0x%02X cmd=0x%02X", frame.con, frame.cmd);
        out->q = 0;
        return ERR_OK;
    }

    /* Ghost detection: presence asserted but no vitals */
    if (out->presence && out->heart_bpm == 0.0f && out->breath_rpm == 0.0f) {
        out->q = 2;  /* degraded */
    }

    return ERR_OK;
}

static err_t bha2_close(void *ctx)
{
    Bha2Ctx *c = (Bha2Ctx *)ctx;
    if (c->initialised && c->uart) {
        uart_deinit(c->uart);
        c->initialised = false;
    }
    return ERR_OK;
}

/* ── Public constructor ──────────────────────────────────────────────────── */

static radar_driver_t s_bha2_driver = {
    .init        = bha2_init,
    .read_sample = bha2_read_sample,
    .close       = bha2_close,
    .name        = "MR60BHA2",
    .ctx         = &s_bha2_ctx,
};

radar_driver_t *radar_bha2_driver(void)
{
    s_bha2_ctx.initialised = false;
    s_bha2_ctx.uart        = NULL;
    return &s_bha2_driver;
}
