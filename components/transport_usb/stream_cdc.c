#define LOG_TAG "CDC"
#include "stream_cdc.h"

#include "log.h"
#include "err.h"
#include "tls_context.h"

#include "tusb.h"
#include "FreeRTOS.h"
#include "task.h"

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* CDC interface index for the MQTT data stream. */
#define CDC_DATA_ITF  ((uint8_t)0u)

/* Minimum poll interval when waiting for data or TX space (1 ms tick). */
#define CDC_POLL_TICKS  ((TickType_t)1u)

/* ── vtable callbacks ────────────────────────────────────────────────────── */

/*
 * read() — copy up to len bytes from the CDC0 RX ring buffer into buf.
 *
 * Blocks (with vTaskDelay) until at least one byte arrives or timeout_ms
 * elapses.  Returns the number of bytes copied on success (>0), 0 on timeout
 * (treat as EAGAIN by the caller), or a negative err_t on hard failure.
 *
 * No spin-wait: each poll yields to the scheduler for CDC_POLL_TICKS.
 */
static int cdc_read(void *ctx, uint8_t *buf, size_t len, uint32_t timeout_ms)
{
    (void)ctx;

    if (!buf || len == 0u) {
        return (int)ERR_INVALID_ARG;
    }

    TickType_t deadline = xTaskGetTickCount() +
                          pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        uint32_t avail = tud_cdc_n_available(CDC_DATA_ITF);
        if (avail > 0u) {
            uint32_t to_read = (avail < (uint32_t)len) ? avail : (uint32_t)len;
            uint32_t got     = tud_cdc_n_read(CDC_DATA_ITF, buf, to_read);
            if (got > 0u) {
                return (int)got;
            }
        }

        /* Check timeout before sleeping. */
        if (timeout_ms == 0u || xTaskGetTickCount() >= deadline) {
            return 0; /* no data yet, caller treats as EAGAIN */
        }

        vTaskDelay(CDC_POLL_TICKS);
    }
}

/*
 * write() — push len bytes from buf into the CDC0 TX ring buffer and flush.
 *
 * Returns len on success, or a negative err_t if the interface is not
 * connected or if the write fails after flushing.
 */
static int cdc_write(void *ctx, const uint8_t *buf, size_t len)
{
    (void)ctx;

    if (!buf || len == 0u) {
        return (int)ERR_INVALID_ARG;
    }

    if (!tud_cdc_n_connected(CDC_DATA_ITF)) {
        LOG_W("write: CDC0 not connected");
        return (int)ERR_IO;
    }

    uint32_t written = tud_cdc_n_write(CDC_DATA_ITF, buf, (uint32_t)len);
    tud_cdc_n_write_flush(CDC_DATA_ITF);

    if (written != (uint32_t)len) {
        LOG_W("write: partial (%lu/%lu)", (unsigned long)written,
              (unsigned long)len);
        return (int)ERR_IO;
    }

    return (int)written;
}

/*
 * close() — flush any pending TX data and let the host know the connection
 * is ending.  TinyUSB CDC does not have an explicit close; we just flush.
 */
static void cdc_close(void *ctx)
{
    (void)ctx;
    if (tud_cdc_n_connected(CDC_DATA_ITF)) {
        tud_cdc_n_write_flush(CDC_DATA_ITF);
    }
    LOG_I("CDC0 stream closed");
}

/* ── public API ─────────────────────────────────────────────────────────── */

err_t stream_cdc_init(stream_t *out)
{
    if (!out) {
        return ERR_INVALID_ARG;
    }

    /* TinyUSB must already be initialised by the caller (tud_init()). */
    if (!tud_inited()) {
        LOG_E("tud_init() not called before stream_cdc_init()");
        return ERR_NOT_INIT;
    }

    out->read  = cdc_read;
    out->write = cdc_write;
    out->close = cdc_close;
    out->ctx   = NULL; /* stateless; TinyUSB owns the ring buffers */

    LOG_I("CDC0 stream_t initialised");
    return ERR_OK;
}

bool stream_cdc_connected(void)
{
    return tud_cdc_n_connected(CDC_DATA_ITF);
}
