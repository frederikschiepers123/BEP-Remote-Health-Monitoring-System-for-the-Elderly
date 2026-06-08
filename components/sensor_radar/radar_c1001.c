#define LOG_TAG "C1001"
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
 * DFRobot C1001 radar driver — 24 GHz presence + vitals.
 *
 * Frame format (Andar/AI-Thinker mmWave family — confirmed by audit §A.4):
 *   [0x53][0x59][CON][CMD][LEN_H][LEN_L][payload...][CKSUM][0x54][0x43]
 *
 * The C1001 uses a request/response model: the host sends a query frame and
 * the radar responds with a data frame.
 *
 * Command table (from audit §A.4):
 *   Presence:   CON=0x80 CMD=0x81
 *   Movement:   CON=0x80 CMD=0x82
 *   Heart rate: CON=0x85 CMD=0x82
 *   Breath rate: CON=0x81 CMD=0x82
 *
 * Ghost detection: if presence=true and heart_bpm < 30, set q=2 (degraded).
 * This matches the heuristic documented in the previous group's audit.
 */

/* ── Frame constants ─────────────────────────────────────────────────────── */

#define FRAME_HEADER_0  0x53U
#define FRAME_HEADER_1  0x59U
#define FRAME_TAIL_0    0x54U
#define FRAME_TAIL_1    0x43U

#define FRAME_MAX_PAYLOAD  64U

/* Query commands */
#define CON_PRESENCE    0x80U
#define CMD_PRESENCE    0x81U
#define CON_MOVEMENT    0x80U
#define CMD_MOVEMENT    0x82U
#define CON_HEART       0x85U
#define CMD_HEART       0x82U
#define CON_BREATH      0x81U
#define CMD_BREATH      0x82U

/* ── Driver context ──────────────────────────────────────────────────────── */

typedef struct {
    uart_inst_t *uart;
    bool         initialised;
} C1001Ctx;

static C1001Ctx s_c1001_ctx;

/* ── UART helpers ────────────────────────────────────────────────────────── */

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

/* ── Frame builder ───────────────────────────────────────────────────────── */

/**
 * Build a query frame into buf (no payload bytes in query).
 * Returns the total frame length.
 */
static size_t build_query_frame(uint8_t *buf, size_t buf_size,
                                uint8_t con, uint8_t cmd)
{
    if (buf_size < 10) { return 0; }

    /* Header, CON, CMD, length = 0 */
    buf[0] = FRAME_HEADER_0;
    buf[1] = FRAME_HEADER_1;
    buf[2] = con;
    buf[3] = cmd;
    buf[4] = 0x00U;  /* LEN_H */
    buf[5] = 0x00U;  /* LEN_L */

    /* Checksum: sum of all bytes from buf[0] to buf[5] */
    uint8_t cksum = 0;
    for (int i = 0; i < 6; i++) {
        cksum = (uint8_t)(cksum + buf[i]);
    }
    buf[6] = cksum;
    buf[7] = FRAME_TAIL_0;
    buf[8] = FRAME_TAIL_1;
    return 9;
}

/* ── Frame receiver (same state machine as BHA2) ─────────────────────────── */

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
    uint8_t  con;
    uint8_t  cmd;
    uint8_t  payload[FRAME_MAX_PAYLOAD];
    uint16_t payload_len;
} ParsedFrame;

static err_t recv_frame(uart_inst_t *uart, ParsedFrame *frame_out,
                        uint32_t timeout_ms)
{
    FrameState state    = FRAME_STATE_WAIT_HEADER0;
    uint16_t   pay_idx  = 0;
    uint16_t   pay_len  = 0;
    uint8_t    cksum    = 0;
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
        if (!uart_read_byte_timeout(uart, &byte, remaining > 10U ? 10U : remaining)) {
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
                cksum = 0;
                state = FRAME_STATE_WAIT_HEADER0;
            }
            break;

        case FRAME_STATE_CON:
            con   = byte;
            cksum = (uint8_t)(cksum + byte);
            state = FRAME_STATE_CMD;
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
                LOG_W("C1001 payload too long: %u", pay_len);
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
            uint8_t expected = cksum;
            if (byte != expected) {
                LOG_W("C1001 checksum mismatch: got 0x%02X expected 0x%02X",
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

/* ── Query helper: send a frame and receive a response ───────────────────── */

static err_t query(uart_inst_t *uart, uint8_t con, uint8_t cmd,
                   ParsedFrame *response, uint32_t timeout_ms)
{
    uint8_t query_buf[16];
    size_t  query_len = build_query_frame(query_buf, sizeof(query_buf), con, cmd);
    if (query_len == 0) {
        return ERR_FAIL;
    }

    /* Flush any stale bytes */
    while (uart_is_readable(uart)) {
        (void)uart_getc(uart);
    }

    /* Send query frame */
    uart_write_blocking(uart, query_buf, query_len);

    /* Receive response */
    return recv_frame(uart, response, timeout_ms);
}

/* ── Driver vtable implementation ────────────────────────────────────────── */

static err_t c1001_init(void *ctx, uart_inst_t *uart)
{
    C1001Ctx *c = (C1001Ctx *)ctx;
    c->uart = uart;

    uart_init(uart, BOARD_RADAR_BAUD);
    uart_set_format(uart, 8, 1, UART_PARITY_NONE);
    gpio_set_function(BOARD_RADAR_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(BOARD_RADAR_RX_PIN, GPIO_FUNC_UART);
    uart_set_fifo_enabled(uart, true);

    c->initialised = true;
    LOG_I("C1001 init OK at %u baud", BOARD_RADAR_BAUD);
    return ERR_OK;
}

static err_t c1001_read_sample(void *ctx, RadarSample *out, uint32_t timeout_ms)
{
    C1001Ctx *c = (C1001Ctx *)ctx;
    if (!c->initialised || !out) {
        return ERR_INVALID_ARG;
    }

    /* Safe defaults */
    out->presence    = false;
    out->distance_mm = 0;
    out->breath_rpm  = 0.0f;
    out->heart_bpm   = 0.0f;
    out->q           = 3;

    /*
     * Query each metric in sequence.  Each query consumes a slice of the
     * timeout budget.  We use timeout_ms / 4 per query so the total budget
     * is respected even if some queries time out.
     */
    uint32_t per_query_ms = timeout_ms / 4U;
    if (per_query_ms < 50U) { per_query_ms = 50U; }

    ParsedFrame resp;
    err_t       err;

    /* 1. Presence */
    err = query(c->uart, CON_PRESENCE, CMD_PRESENCE, &resp, per_query_ms);
    if (err == ERR_OK && resp.con == CON_PRESENCE && resp.cmd == CMD_PRESENCE
            && resp.payload_len >= 1) {
        out->presence = (resp.payload[0] != 0x00U);
    }

    /* 2. Movement / distance */
    err = query(c->uart, CON_MOVEMENT, CMD_MOVEMENT, &resp, per_query_ms);
    if (err == ERR_OK && resp.con == CON_MOVEMENT && resp.cmd == CMD_MOVEMENT
            && resp.payload_len >= 1) {
        /* payload[0] encodes movement state; distance not available from
         * this command on the C1001. */
        (void)resp.payload[0];
    }

    /* 3. Heart rate */
    err = query(c->uart, CON_HEART, CMD_HEART, &resp, per_query_ms);
    if (err == ERR_OK && resp.con == CON_HEART && resp.cmd == CMD_HEART
            && resp.payload_len >= 1) {
        out->heart_bpm = (float)resp.payload[0];
    }

    /* 4. Breath rate */
    err = query(c->uart, CON_BREATH, CMD_BREATH, &resp, per_query_ms);
    if (err == ERR_OK && resp.con == CON_BREATH && resp.cmd == CMD_BREATH
            && resp.payload_len >= 1) {
        out->breath_rpm = (float)resp.payload[0];
    }

    /* Mark valid now that we attempted all queries */
    out->q = 0;

    /* Ghost detection: presence asserted but implausibly low heart rate */
    if (out->presence && out->heart_bpm < 30.0f) {
        out->q = 2;  /* degraded */
        LOG_D("C1001 ghost detection: presence=1 but heart_bpm=%.1f",
              (double)out->heart_bpm);
    }

    return ERR_OK;
}

static err_t c1001_close(void *ctx)
{
    C1001Ctx *c = (C1001Ctx *)ctx;
    if (c->initialised && c->uart) {
        uart_deinit(c->uart);
        c->initialised = false;
    }
    return ERR_OK;
}

/* ── Public constructor ──────────────────────────────────────────────────── */

static radar_driver_t s_c1001_driver = {
    .init        = c1001_init,
    .read_sample = c1001_read_sample,
    .close       = c1001_close,
    .name        = "C1001",
    .ctx         = &s_c1001_ctx,
};

radar_driver_t *radar_c1001_driver(void)
{
    s_c1001_ctx.initialised = false;
    s_c1001_ctx.uart        = NULL;
    return &s_c1001_driver;
}
