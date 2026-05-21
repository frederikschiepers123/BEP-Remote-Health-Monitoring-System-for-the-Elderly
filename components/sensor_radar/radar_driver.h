#ifndef RADAR_DRIVER_H
#define RADAR_DRIVER_H

/* Abstract radar driver interface.
 *
 * Both the Seeed MR60BHA2 (60 GHz) and DFRobot C1001 (24 GHz) are supported
 * behind this common vtable.  Selection is by config flag in /cfg/sensors.json
 * — not by runtime UART probing.
 *
 * See CLAUDE.md §7.4 and §3.2 for the design rationale and note on shared
 * Andar/AI-Thinker framing (bench verification pending).
 */

#include "err.h"

#include "FreeRTOS.h"
#include "queue.h"

#include "hardware/uart.h"

#include <stdint.h>
#include <stdbool.h>

/* ── Lowest-common-denominator sample ────────────────────────────────────── */

typedef struct {
    float    breath_rpm;    /* Breaths per minute; valid when q != 3 */
    float    heart_bpm;     /* Heart rate BPM; valid when q != 3 */
    uint32_t distance_mm;   /* Distance to target in mm; 0 = not reported */
    bool     presence;      /* True if a person is detected */
    uint8_t  q;             /* 0=ok, 2=degraded (ghost/noise), 3=invalid */
} RadarSample;

/* ── Driver vtable ───────────────────────────────────────────────────────── */

typedef struct {
    /**
     * Initialise the driver and configure the UART.
     *
     * @param ctx  Opaque driver context (allocated statically in driver .c).
     * @param uart UART instance to use (BOARD_RADAR_UART_INST).
     * @return ERR_OK on success.
     */
    err_t (*init)(void *ctx, uart_inst_t *uart);

    /**
     * Read one sample from the radar.
     *
     * Must be self-contained: send a request frame (if required), wait for
     * the response, parse it, and return.
     *
     * @param ctx        Opaque driver context.
     * @param out        Output sample to fill.
     * @param timeout_ms Maximum time to wait for a complete response.
     * @return ERR_OK on success, ERR_TIMEOUT if no frame arrived, ERR_IO on
     *         hardware error.
     */
    err_t (*read_sample)(void *ctx, RadarSample *out, uint32_t timeout_ms);

    /**
     * Release resources (UART, buffers).  Called on transport swap or shutdown.
     */
    err_t (*close)(void *ctx);

    /** Human-readable name for logging: "MR60BHA2" or "C1001". */
    const char *name;

    /** Opaque context pointer passed to all vtable functions. */
    void *ctx;
} radar_driver_t;

/* ── Driver constructors (implemented in radar_bha2.c / radar_c1001.c) ──── */

/** Return a pointer to the static MR60BHA2 driver struct. */
radar_driver_t *radar_bha2_driver(void);

/** Return a pointer to the static C1001 driver struct. */
radar_driver_t *radar_c1001_driver(void);

/* ── Config-driven selection (implemented in radar_select.c) ─────────────── */

/**
 * Read /cfg/sensors.json, parse the "radar" key ("bha2" or "c1001"), and
 * return the corresponding driver.
 *
 * @return Pointer to a static driver struct, or NULL if config is missing
 *         or unknown.
 */
radar_driver_t *radar_select_from_config(void);

/* ── Radar task (implemented in radar_task.c) ────────────────────────────── */

/** Queue of RadarSample produced by radar_task; consumed by transport_task. */
extern QueueHandle_t q_radar;

/**
 * Initialise the radar component.  Creates q_radar.
 * Must be called before the scheduler starts.
 */
err_t radar_task_init(void);

/** FreeRTOS task entry point.  Never returns. */
void radar_task(void *arg);

#endif /* RADAR_DRIVER_H */
