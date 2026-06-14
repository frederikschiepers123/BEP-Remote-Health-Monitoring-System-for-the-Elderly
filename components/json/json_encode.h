#ifndef JSON_ENCODE_H
#define JSON_ENCODE_H

/* snprintf-based JSON encoders for outbound MQTT payloads.
 *
 * All encoders write into a caller-supplied fixed buffer using snprintf
 * templates — no allocation, no DOM, no library.  They follow the envelope
 * and per-sensor schemas defined in CLAUDE.md §9.2.
 *
 * Return value: number of bytes that would have been written (excluding NUL),
 * identical to snprintf semantics.  If the return value >= cap, the output
 * was truncated.  Callers must check for truncation before publishing.
 *
 * wall_ms sentinel: pass -1 when the RTC is not synced.  Encoders emit
 * the literal -1 in JSON; the Radxa interprets this per §9.6. */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ── Per-sensor v bodies ───────────────────────────────────────────────────
 *
 * These fill only the "v" object, without the outer envelope.
 * Use json_encode_sample() to wrap them with ts_us/wall_ms/seq/q. */

typedef struct {
    float temp_c;
    float hum_pct;
    float pres_hpa;
    bool  pres_valid;    /* false → pres_hpa emitted as null (AHT21 path, §9.2.2) */
} JsonEnvBody;

typedef struct {
    uint16_t co2_ppm;
    uint16_t tvoc_ppb;
    uint8_t  aqi;        /* UBA index 1..5; 0 = no usable reading yet */
} JsonAirBody;

typedef struct {
    bool  presence;
    int   distance_mm;   /* -1 if not measured */
    float breath_bpm;    /* -1.0f if not measured */
    float heart_bpm;     /* -1.0f if not measured */
    int   resp_motion;   /* respiratory motion (ADR-0006): -1 null, 0 false
                          * (possible breath-hold), 1 true (motion present) */
} JsonRadarBody;

typedef struct {
    float lux;
} JsonLightBody;

/* ── Full-sample encoders (envelope + v body) ─────────────────────────────
 *
 * Output example (env):
 *   {"ts_us":1234567,"wall_ms":-1,"seq":42,"q":0,
 *    "v":{"temp_c":21.500,"hum_pct":55.000,"pres_hpa":1013.250}}
 *
 * All fields present, no omissions per §9.2.3. */

int json_encode_env(char *buf, size_t cap,
                    uint64_t ts_us, int64_t wall_ms, uint32_t seq, uint8_t q,
                    const JsonEnvBody *v);

int json_encode_air(char *buf, size_t cap,
                    uint64_t ts_us, int64_t wall_ms, uint32_t seq, uint8_t q,
                    const JsonAirBody *v);

int json_encode_radar(char *buf, size_t cap,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq, uint8_t q,
                      const JsonRadarBody *v);

int json_encode_light(char *buf, size_t cap,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq, uint8_t q,
                      const JsonLightBody *v);

/* ── Status payload ────────────────────────────────────────────────────────
 * Encodes the plain string "online" or "offline" — no JSON envelope needed
 * because rmms/<uuid>/status carries a raw string per CLAUDE.md §9.1. */
int json_encode_status(char *buf, size_t cap, bool online);

#endif /* JSON_ENCODE_H */
