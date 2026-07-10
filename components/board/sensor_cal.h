#ifndef SENSOR_CAL_H
#define SENSOR_CAL_H

/* Per-board sensor calibration trim.
 *
 * Generic additive offsets ("deltas") applied to each reading inside the
 * driver's raw read_sample() — the one seam shared by the production tasks
 * (via the v-table adapters) and the bring-up benches (which call the raw
 * read directly), so a trim shows up identically on the wire, the OLED, the
 * mirror, and a bench console.  All values are PLACEHOLDERS at 0 (no trim)
 * until a board is calibrated against a reference instrument:
 *
 *     reported = measured + CAL_<sensor>_<field>_DELTA
 *
 * These stack with — and do not replace — the physical corrections that
 * already exist elsewhere:
 *   - env temp self-heating: ENV_TEMP_SELF_HEAT_OFFSET_C (env_driver.h),
 *     subtracted at the env_driver_t v-table adapter.
 *   - radar heart/breath: RADAR_HEART_CAL_OFFSET_BPM /
 *     RADAR_BREATH_CAL_OFFSET_RPM (radar_filter.h), subtracted in the
 *     plausibility filter.  The radar therefore has no generic trim here.
 * The ENS160 AQI is an index (1–5) computed by the chip, not a measurement —
 * it takes no trim either.
 */

#ifdef HOST_TEST

/* Host unit tests (§14.1) assert datasheet-pure decode values; the trims are
 * pinned to 0 here so tuning a board can never break the tests. */
#define CAL_ENV_TEMP_DELTA_C    0.0f
#define CAL_ENV_HUM_DELTA_PCT   0.0f
#define CAL_ENV_PRES_DELTA_HPA  0.0f
#define CAL_AIR_CO2_DELTA_PPM   0
#define CAL_AIR_TVOC_DELTA_PPB  0
#define CAL_LIGHT_LUX_DELTA     0.0f

#else /* target build — tune per board below */

#define CAL_ENV_TEMP_DELTA_C    0.0f  /* °C   added to temp_c                     */
#define CAL_ENV_HUM_DELTA_PCT   0.0f  /* %RH  added to humidity_pct               */
#define CAL_ENV_PRES_DELTA_HPA  0.0f  /* hPa  added to pressure_hpa               */
#define CAL_AIR_CO2_DELTA_PPM   0     /* ppm  added to co2_ppm  (clamped 0–65535) */
#define CAL_AIR_TVOC_DELTA_PPB  0     /* ppb  added to tvoc_ppb (clamped 0–65535) */
#define CAL_LIGHT_LUX_DELTA     0.0f  /* lux  added to lux      (clamped at 0)    */

#endif /* HOST_TEST */

#endif /* SENSOR_CAL_H */
