#define LOG_TAG "RADAR_SEL"
#include "log.h"

#include "radar_driver.h"
#include "storage.h"
#include "err.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * Reads /cfg/sensors.json and returns the matching radar_driver_t.
 *
 * For v1, the JSON file is treated as a plain ASCII string for simplicity
 * (only "bha2" is supported), until a JSON parsing integration is added.
 * The string must match exactly (case-sensitive, no trailing whitespace).
 *
 * See CLAUDE.md §3.2 and §7.4 for design rationale.
 */

#define SENSORS_CFG_PATH  "/cfg/sensors.json"
#define CFG_BUF_SIZE      32U

radar_driver_t *radar_select_from_config(void)
{
    uint8_t buf[CFG_BUF_SIZE];
    size_t  len = 0;

    err_t err = storage_read(SENSORS_CFG_PATH, buf, sizeof(buf) - 1, &len);
    if (err == ERR_NOT_FOUND) {
        LOG_E("Radar config not found at %s — cannot select driver",
              SENSORS_CFG_PATH);
        return NULL;
    }
    if (err != ERR_OK) {
        LOG_E("Failed to read %s: %d", SENSORS_CFG_PATH, err);
        return NULL;
    }

    /* NUL-terminate for safe string comparison */
    buf[len] = '\0';

    if (strcmp((const char *)buf, "bha2") == 0) {
        LOG_I("Radar driver selected: MR60BHA2 (BHA2)");
        return radar_bha2_driver();
    }

    LOG_E("Unknown radar type in config: \"%s\" (expected \"bha2\")",
          (const char *)buf);
    return NULL;
}
