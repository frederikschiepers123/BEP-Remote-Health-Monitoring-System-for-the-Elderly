#define LOG_TAG "I2C_BUS"
#include "log.h"

#include "i2c_bus.h"
#include "board_pico2wh.h"
#include "err.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t s_mutex = NULL;

err_t i2c_bus_init(void)
{
    if (s_mutex != NULL) {
        return ERR_OK;   /* already initialised */
    }

    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) {
        LOG_E("Failed to create I²C bus mutex");
        return ERR_NO_MEM;
    }

    /* One-time bus hardware init — the single owner of I²C0 configuration. */
    i2c_init(BOARD_I2C_INST, BOARD_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    /* Breakouts carry 10k pull-ups; enable the internal pull-ups too as
     * belt-and-braces (RP2350 internal pull-up ≈ 50–80 kΩ). */
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);

    LOG_I("I²C0 init on SDA=GP%d SCL=GP%d @ %u Hz",
          BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)BOARD_I2C_FREQ_HZ);
    return ERR_OK;
}

void i2c_bus_lock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

void i2c_bus_unlock(void)
{
    if (s_mutex != NULL) {
        (void)xSemaphoreGive(s_mutex);
    }
}
