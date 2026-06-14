#ifndef RADAR_DRIVER_H
#define RADAR_DRIVER_H

/* Abstract radar driver interface.
 *
 * Two radars sit behind this common vtable, selected by the "radar" flag in
 * /cfg/sensors.json (not by runtime UART probing):
 *   - "bha2": Seeed MR60BHA2 (60 GHz, heart + breath + breath-phase)
 *   - "hmmd": Seeed 24 GHz HMMD micro-motion module (ADR-0007)
 * Both produce the same RadarSample; radar_task and the §9.2 wire schema are
 * unaware of which is attached.  The vtable is the extensibility seam for any
 * further radar: a new radar_*.c file plus one line in radar_select.c.
 *
 * See CLAUDE.md §7.4 and §3.2 for the design rationale.
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

    /* Respiratory chest-motion, for phase-based breath-hold detection
     * (ADR-0006).  The breath RATE is a windowed frequency that cannot fall
     * during a hold; the radar's raw breath-PHASE (chest displacement) does
     * flatten, so its recent amplitude tells motion from no-motion.  These
     * two field-pairs carry DIFFERENT things at the two stages of the pipe:
     *
     *   On a RAW driver sample (radar_driver_t read_sample output):
     *     resp_motion_amp       = recent breath-phase peak-to-peak amplitude
     *                             (driver units); 0 when not computed.
     *     resp_motion_amp_valid = true when the driver had enough fresh phase
     *                             samples to compute it.  A radar with no
     *                             phase stream leaves this false → feature
     *                             stays inert (graceful degradation).
     *
     *   On a FILTERED sample (radar_filter_apply output → queue/spool/wire):
     *     resp_motion           = chest motion present (false = possible
     *                             breath-hold).  Only meaningful when valid.
     *     resp_motion_valid     = the decision is meaningful (present +
     *                             distance-locked + a valid amplitude); when
     *                             false the wire emits resp_motion: null. */
    float    resp_motion_amp;       /* raw stage only */
    bool     resp_motion_amp_valid; /* raw stage only */
    bool     resp_motion;           /* filtered stage only */
    bool     resp_motion_valid;     /* filtered stage only */
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

    /** Human-readable name for logging, e.g. "MR60BHA2". */
    const char *name;

    /** Opaque context pointer passed to all vtable functions. */
    void *ctx;
} radar_driver_t;

/* ── Driver constructors ─────────────────────────────────────────────────── */

/** Return a pointer to the static MR60BHA2 driver struct (radar_bha2.c). */
radar_driver_t *radar_bha2_driver(void);

/** Return a pointer to the static 24 GHz HMMD driver struct (radar_hmmd.c). */
radar_driver_t *radar_hmmd_driver(void);

/* ── Config-driven selection (implemented in radar_select.c) ─────────────── */

/**
 * Read /cfg/sensors.json, parse the "radar" key ("bha2" | "hmmd"), and return
 * the corresponding driver.
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
