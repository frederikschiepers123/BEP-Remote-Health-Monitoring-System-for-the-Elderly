#define LOG_TAG "JSON_ENC"
#include "json_encode.h"

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Payload size budget per §9.2.3: ≤ 256 bytes. snprintf returns the number
 * of bytes that would have been written.  Callers treat return >= cap as
 * an error (truncation). */

int json_encode_env(char *buf, size_t cap,
                    uint64_t ts_us, int64_t wall_ms, uint32_t seq, uint8_t q,
                    const JsonEnvBody *v)
{
    return snprintf(buf, cap,
        "{\"ts_us\":%llu,\"wall_ms\":%lld,\"seq\":%lu,\"q\":%u,"
        "\"v\":{\"temp_c\":%.3f,\"hum_pct\":%.3f,\"pres_hpa\":%.3f}}",
        (unsigned long long)ts_us,
        (long long)wall_ms,
        (unsigned long)seq,
        (unsigned)q,
        (double)v->temp_c,
        (double)v->hum_pct,
        (double)v->pres_hpa);
}

int json_encode_radar(char *buf, size_t cap,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq, uint8_t q,
                      const JsonRadarBody *v)
{
    /* distance_mm, breath_bpm, heart_bpm are nullable (null when -1). */
    char dist_str[16];
    char breath_str[16];
    char heart_str[16];

    if (v->distance_mm < 0) {
        (void)snprintf(dist_str,   sizeof(dist_str),   "null");
    } else {
        (void)snprintf(dist_str,   sizeof(dist_str),   "%d",   v->distance_mm);
    }
    if (v->breath_bpm < 0.0f) {
        (void)snprintf(breath_str, sizeof(breath_str), "null");
    } else {
        (void)snprintf(breath_str, sizeof(breath_str), "%.1f", (double)v->breath_bpm);
    }
    if (v->heart_bpm < 0.0f) {
        (void)snprintf(heart_str,  sizeof(heart_str),  "null");
    } else {
        (void)snprintf(heart_str,  sizeof(heart_str),  "%.1f", (double)v->heart_bpm);
    }

    return snprintf(buf, cap,
        "{\"ts_us\":%llu,\"wall_ms\":%lld,\"seq\":%lu,\"q\":%u,"
        "\"v\":{\"presence\":%s,\"distance_mm\":%s,"
        "\"breath_bpm\":%s,\"heart_bpm\":%s}}",
        (unsigned long long)ts_us,
        (long long)wall_ms,
        (unsigned long)seq,
        (unsigned)q,
        v->presence ? "true" : "false",
        dist_str, breath_str, heart_str);
}

int json_encode_light(char *buf, size_t cap,
                      uint64_t ts_us, int64_t wall_ms, uint32_t seq, uint8_t q,
                      const JsonLightBody *v)
{
    return snprintf(buf, cap,
        "{\"ts_us\":%llu,\"wall_ms\":%lld,\"seq\":%lu,\"q\":%u,"
        "\"v\":{\"lux\":%.1f}}",
        (unsigned long long)ts_us,
        (long long)wall_ms,
        (unsigned long)seq,
        (unsigned)q,
        (double)v->lux);
}

int json_encode_status(char *buf, size_t cap, bool online)
{
    return snprintf(buf, cap, "%s", online ? "online" : "offline");
}
