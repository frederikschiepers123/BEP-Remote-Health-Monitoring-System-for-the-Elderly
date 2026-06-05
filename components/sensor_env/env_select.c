#define LOG_TAG "ENV"
#include "log.h"

#include "env_driver.h"
#include "cfg.h"

env_driver_t *env_select_from_config(void) {
    CfgSensors cs;
    err_t e = cfg_load_sensors(&cs);
    if (e != ERR_OK) {
        LOG_W("cfg_load_sensors failed (%ld) — defaulting to BME280", (long)e);
        return env_bme280_driver();
    }
    switch (cs.env) {
    case CFG_ENV_BME280:  return env_bme280_driver();
    case CFG_ENV_AHT21:   return env_aht21_driver();
    case CFG_ENV_DEFAULT:
    default:              return env_bme280_driver();
    }
}
