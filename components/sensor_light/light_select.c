#define LOG_TAG "LIGHT"
#include "log.h"

#include "light_driver.h"
#include "cfg.h"

light_driver_t *light_select_from_config(void) {
    CfgSensors cs;
    err_t e = cfg_load_sensors(&cs);
    if (e != ERR_OK) {
        LOG_W("cfg_load_sensors failed (%ld) — defaulting to BH1750", (long)e);
        return light_bh1750_driver();
    }
    switch (cs.light) {
    case CFG_LIGHT_BH1750: return light_bh1750_driver();
    case CFG_LIGHT_GL5516: return light_gl5516_driver();
    case CFG_LIGHT_DEFAULT:
    default:               return light_bh1750_driver();
    }
}
