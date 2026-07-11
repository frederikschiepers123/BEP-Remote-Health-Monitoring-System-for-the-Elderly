/*
 * Bring-up — ENS160 decisive diagnostic (clone vs power vs driver), RAW I²C,
 * no driver, no network. Settles "ENS160 ACKs + PART_ID ok + OPMODE=STANDARD
 * but STATAS never asserts and AQI/eCO2/TVOC stay 0".
 *
 * Two checks, with NO OPMODE re-kicking (unlike ens160_read_sample's recovery):
 *   A) APP FIRMWARE VERSION via the COMMAND(0x12)/GPR_READ path — warm-up
 *      independent CLONE detector. Genuine ScioSense parts return a sane
 *      version (field reference: 5.4.6); a clone/dead die returns 0.0.0 /
 *      garbage or never sets NEWGPR.
 *   B) RESET → STANDARD *once*, then poll DEVICE_STATUS every second WITHOUT
 *      ever rewriting OPMODE, so a genuine engine is free to bring STATAS up
 *      (must happen within ~1–2 s — independent of the 3-min validity warm-up).
 *
 * Decision rule:
 *   version sane + STATAS sets in ~1–2 s → genuine & fine; the production
 *                                          driver's re-kick recovery was masking.
 *   version sane + STATAS stuck 0        → genuine but starved: scope the
 *                                          ENS160 1.8 V core VDD for droop.
 *   version 0.0.0 / garbage / no NEWGPR  → clone or dead die: swap the board.
 *
 * Wiring: VCC=3V3(pin36) GND SDA=GP8(pin11) SCL=GP9(pin12); ENS160 @ 0x53.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "board_pico2wh.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#define DIAG_I2C_FREQ_HZ   100000u
#define I2C_TO_US          5000

/* ENS160 register map (datasheet v1.3). */
#define REG_PART_ID    0x00U   /* 2 bytes LE, expect 0x0160                 */
#define REG_OPMODE     0x10U
#define REG_COMMAND    0x12U
#define REG_STATUS     0x20U   /* DEVICE_STATUS                             */
#define REG_DATA_AQI   0x21U
#define REG_DATA_TVOC  0x22U   /* 2 bytes LE                                */
#define REG_DATA_ECO2  0x24U   /* 2 bytes LE                                */
#define REG_GPR_READ4  0x4CU   /* GET_APPVER → major.minor.build at 0x4C..E */

#define OPMODE_DEEP_SLEEP  0x00U
#define OPMODE_IDLE        0x01U
#define OPMODE_STANDARD    0x02U
#define OPMODE_RESET       0xF0U

#define CMD_NOP        0x00U
#define CMD_CLRGPR     0xCCU
#define CMD_GET_APPVER 0x0EU

#define ST_STATAS  0x80U
#define ST_STATER  0x40U
#define ST_VALID   0x0CU   /* bits 3:2 */
#define ST_NEWDAT  0x02U
#define ST_NEWGPR  0x01U

static const uint8_t ENS = BOARD_ENS160_ADDR;   /* 0x53 */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[ens-diag] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[ens-diag] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

static void i2c_bus_init(void)
{
    i2c_init(BOARD_I2C_INST, DIAG_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);
    printf("[ens-diag] I²C0 init SDA=GP%d(pin11) SCL=GP%d(pin12) @ %u Hz, ENS160@0x%02x\n",
           BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)DIAG_I2C_FREQ_HZ, ENS);
}

/* Timeout-bounded register write: [reg][data...]. Returns true on full ACK. */
static bool reg_write(uint8_t reg, const uint8_t *data, size_t n)
{
    uint8_t buf[8];
    if (n + 1 > sizeof buf) return false;
    buf[0] = reg;
    if (n) memcpy(&buf[1], data, n);
    int rc = i2c_write_timeout_us(BOARD_I2C_INST, ENS, buf, n + 1, false, I2C_TO_US);
    return rc == (int)(n + 1);
}
static bool reg_write1(uint8_t reg, uint8_t v) { return reg_write(reg, &v, 1); }

/* Timeout-bounded register read: write reg ptr (repeated start), then read n. */
static bool reg_read(uint8_t reg, uint8_t *dst, size_t n)
{
    int rc = i2c_write_timeout_us(BOARD_I2C_INST, ENS, &reg, 1, true, I2C_TO_US);
    if (rc != 1) return false;
    rc = i2c_read_timeout_us(BOARD_I2C_INST, ENS, dst, n, false, I2C_TO_US);
    return rc == (int)n;
}

static const char *validity_str(uint8_t status)
{
    switch ((status & ST_VALID) >> 2) {
        case 0:  return "normal";
        case 1:  return "warmup";
        case 2:  return "startup";
        default: return "invalid";
    }
}

static void dump_status(uint32_t t_ms)
{
    uint8_t st = 0xFF, aqi = 0; uint8_t tvoc[2] = {0}, eco2[2] = {0};
    bool ok = reg_read(REG_STATUS, &st, 1);
    reg_read(REG_DATA_AQI, &aqi, 1);
    reg_read(REG_DATA_TVOC, tvoc, 2);
    reg_read(REG_DATA_ECO2, eco2, 2);
    if (!ok) { printf("[ens-diag] t=%2lus  STATUS read FAILED (bus?)\n", (unsigned long)(t_ms / 1000)); return; }
    printf("[ens-diag] t=%2lus  status=0x%02x  STATAS=%d STATER=%d validity=%s NEWDAT=%d NEWGPR=%d  "
           "AQI=%u TVOC=%u eCO2=%u\n",
           (unsigned long)(t_ms / 1000), st,
           (st & ST_STATAS) ? 1 : 0, (st & ST_STATER) ? 1 : 0,
           validity_str(st), (st & ST_NEWDAT) ? 1 : 0, (st & ST_NEWGPR) ? 1 : 0,
           aqi, (unsigned)(tvoc[0] | (tvoc[1] << 8)), (unsigned)(eco2[0] | (eco2[1] << 8)));
}

static void ens_diag_task(void *arg)
{
    (void)arg;

    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));

    printf("\n[ens-diag] ENS160 clone/power/driver discriminator — raw I²C, no re-kicking.\n");
    i2c_bus_init();

    /* RESET → settle → PART_ID. */
    if (!reg_write1(REG_OPMODE, OPMODE_RESET))
        printf("[ens-diag] RESET write NACK (0x53 not answering?)\n");
    vTaskDelay(pdMS_TO_TICKS(250));

    uint8_t pid[2] = {0};
    if (reg_read(REG_PART_ID, pid, 2)) {
        uint16_t id = (uint16_t)(pid[0] | (pid[1] << 8));
        printf("[ens-diag] PART_ID = 0x%04x (%s)\n", id,
               id == 0x0160 ? "matches ENS160 — but clones echo this too" : "UNEXPECTED");
    } else {
        printf("[ens-diag] PART_ID read FAILED — check wiring/power\n");
    }

    /* ── A) APP FIRMWARE VERSION (clone detector, before STANDARD) ───────── */
    reg_write1(REG_OPMODE, OPMODE_IDLE);   vTaskDelay(pdMS_TO_TICKS(30));
    reg_write1(REG_COMMAND, CMD_NOP);      vTaskDelay(pdMS_TO_TICKS(10));
    reg_write1(REG_COMMAND, CMD_CLRGPR);   vTaskDelay(pdMS_TO_TICKS(10));
    reg_write1(REG_COMMAND, CMD_GET_APPVER); vTaskDelay(pdMS_TO_TICKS(15));

    bool gpr_ready = false;
    for (int i = 0; i < 10; i++) {            /* poll NEWGPR up to ~100 ms */
        uint8_t st = 0;
        if (reg_read(REG_STATUS, &st, 1) && (st & ST_NEWGPR)) { gpr_ready = true; break; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (gpr_ready) {
        uint8_t ver[3] = {0};
        if (reg_read(REG_GPR_READ4, ver, 3)) {
            printf("[ens-diag] APP FW version = %u.%u.%u   →  %s\n",
                   ver[0], ver[1], ver[2],
                   (ver[0] || ver[1] || ver[2]) ? "non-zero (looks GENUINE)"
                                                : "0.0.0 (CLONE/dead die)");
        } else printf("[ens-diag] GPR version read FAILED\n");
    } else {
        printf("[ens-diag] NEWGPR never set after GET_APPVER → CLONE/dead die (genuine returns a version)\n");
    }

    /* ── B) STANDARD once, then poll STATUS forever — NO OPMODE rewrites ──── */
    printf("[ens-diag] writing OPMODE=STANDARD once; polling STATUS (no re-kick). "
           "Genuine engine → STATAS=1 within ~1-2 s.\n");
    reg_write1(REG_OPMODE, OPMODE_STANDARD);

    uint32_t t = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        t += 1000;
        dump_status(t);
    }
}

int main(void)
{
    stdio_init_all();
    (void)cyw43_arch_init();
    xTaskCreate(ens_diag_task, "ensdiag", 2048, NULL, 2, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
