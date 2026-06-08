/*
 * Bring-up — SH1122 OLED (256×64 4-bit grayscale) status pages, no network.
 *
 * Renders four status pages, auto-cycled every 3 s and also advanced on each
 * press of the display-control button (GP16, active low). If the BME280 +
 * ENS160 are wired on the shared I²C0 bus they're sampled live and shown on
 * their respective pages; if not, the pages display "—" placeholders.
 *
 * Pages:
 *   1. RMMS header + uptime + sensor probe status
 *   2. Environment (BME280)
 *   3. Air quality (ENS160 — warmup-aware)
 *   4. Build info
 *
 * Text uses the 5×7 font at 2× scale (10×14 px per glyph) for legibility on
 * the 2.08" panel. The font is ASCII 32–95 only; lowercase folds to upper.
 */
#include "FreeRTOS.h"
#include "task.h"

#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"

#include "board_pico2wh.h"
#include "sh1122.h"
#include "bme280.h"
#include "ens160.h"
#include "err.h"

#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>
#include <string.h>

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName);
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    printf("[oled] STACK OVERFLOW in '%s'\n", pcTaskName ? pcTaskName : "?");
    for (;;) { tight_loop_contents(); }
}
void vApplicationMallocFailedHook(void);
void vApplicationMallocFailedHook(void)
{
    printf("[oled] malloc failed\n");
    for (;;) { tight_loop_contents(); }
}

static Sh1122  s_oled;
static Bme280  s_bme;
static Ens160  s_ens;
static bool    s_bme_ok = false;
static bool    s_ens_ok = false;

static Bme280Sample s_env_last;
static Ens160Sample s_air_last;
static bool s_env_valid = false;
static bool s_air_valid = false;

static void i2c_bus_init(void)
{
    i2c_init(BOARD_I2C_INST, BOARD_I2C_FREQ_HZ);
    gpio_set_function(BOARD_I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(BOARD_I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(BOARD_I2C_SDA_PIN);
    gpio_pull_up(BOARD_I2C_SCL_PIN);
    printf("[oled] I²C0 init on SDA=GP%d SCL=GP%d @ %u Hz\n",
           BOARD_I2C_SDA_PIN, BOARD_I2C_SCL_PIN, (unsigned)BOARD_I2C_FREQ_HZ);
}

static void i2c_scan(void)
{
    printf("[oled] I²C scan:");
    int count = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        uint8_t dummy = 0;
        int rc = i2c_read_blocking(BOARD_I2C_INST, addr, &dummy, 1, false);
        if (rc >= 0) { printf(" 0x%02x", addr); count++; }
    }
    printf("  (%d device%s found)\n", count, count == 1 ? "" : "s");
}

static void sample_sensors(void)
{
    if (s_bme_ok && bme280_read_sample(&s_bme, &s_env_last) == ERR_OK) s_env_valid = true;
    if (s_ens_ok && ens160_read_sample(&s_ens, &s_air_last) == ERR_OK) s_air_valid = true;
}

static const char *aqi_label(uint8_t aqi)
{
    switch (aqi) {
    case 1: return "EXCELLENT";
    case 2: return "GOOD";
    case 3: return "MODERATE";
    case 4: return "POOR";
    case 5: return "UNHEALTHY";
    default: return "-";
    }
}

/* ── Page renderers ─────────────────────────────────────────────────────── */
/* At scale=2 each glyph is 10x14, stride 12 px horizontal. 4 lines fit
 * vertically: y=0, y=16, y=32, y=48. */

static void draw_footer_page_indicator(uint8_t page_num)
{
    char line[16];
    snprintf(line, sizeof(line), "%u/4", (unsigned)page_num);
    /* small (scale 1) bottom-right */
    sh1122_draw_text(&s_oled, 240, 56, 1, line);
}

static void draw_page_header(uint8_t page_num, uint32_t uptime_s)
{
    sh1122_clear(&s_oled);
    sh1122_draw_text(&s_oled, 0, 0,  2, "RMMS SENSOR MODULE");
    char line[40];
    snprintf(line, sizeof(line), "UPTIME %lu S", (unsigned long)uptime_s);
    sh1122_draw_text(&s_oled, 0, 18, 2, line);
    snprintf(line, sizeof(line), "BME280:%s   ENS160:%s",
             s_bme_ok ? "OK" : "--",
             s_ens_ok ? "OK" : "--");
    sh1122_draw_text(&s_oled, 0, 36, 2, line);
    draw_footer_page_indicator(page_num);
}

static void draw_page_env(uint8_t page_num)
{
    sh1122_clear(&s_oled);
    sh1122_draw_text(&s_oled, 0, 0, 2, "ENVIRONMENT");
    char line[40];
    if (s_bme_ok && s_env_valid) {
        snprintf(line, sizeof(line), "T %5.1f C", (double)s_env_last.temp_c);
        sh1122_draw_text(&s_oled, 0, 18, 2, line);
        snprintf(line, sizeof(line), "H %5.1f %%", (double)s_env_last.humidity_pct);
        sh1122_draw_text(&s_oled, 0, 36, 2, line);
        snprintf(line, sizeof(line), "P %6.1f HPA", (double)s_env_last.pressure_hpa);
        sh1122_draw_text(&s_oled, 124, 18, 2, line);  /* second column */
    } else {
        sh1122_draw_text(&s_oled, 0, 18, 2, "NOT AVAILABLE");
    }
    draw_footer_page_indicator(page_num);
}

static void draw_page_air(uint8_t page_num)
{
    sh1122_clear(&s_oled);
    sh1122_draw_text(&s_oled, 0, 0, 2, "AIR QUALITY");
    char line[40];
    if (s_ens_ok && s_air_valid) {
        Ens160Validity v = ens160_validity(s_air_last.status);
        if (v == ENS160_VALIDITY_NORMAL) {
            snprintf(line, sizeof(line), "AQI %u (%s)", s_air_last.aqi, aqi_label(s_air_last.aqi));
            sh1122_draw_text(&s_oled, 0, 18, 2, line);
            snprintf(line, sizeof(line), "CO2 %u PPM", s_air_last.co2_ppm);
            sh1122_draw_text(&s_oled, 0, 36, 2, line);
            snprintf(line, sizeof(line), "TVOC %u PPB", s_air_last.tvoc_ppb);
            sh1122_draw_text(&s_oled, 140, 36, 2, line);
        } else if (v == ENS160_VALIDITY_WARMUP) {
            sh1122_draw_text(&s_oled, 0, 18, 2, "WARMUP IN PROGRESS");
            sh1122_draw_text(&s_oled, 0, 36, 2, "(~3 MIN)");
        } else if (v == ENS160_VALIDITY_INITIAL_STARTUP) {
            sh1122_draw_text(&s_oled, 0, 18, 2, "INITIAL STARTUP");
            sh1122_draw_text(&s_oled, 0, 36, 2, "(~1 H, NEW SENSOR)");
        } else {
            sh1122_draw_text(&s_oled, 0, 18, 2, "INVALID OUTPUT");
        }
    } else {
        sh1122_draw_text(&s_oled, 0, 18, 2, "NOT AVAILABLE");
    }
    draw_footer_page_indicator(page_num);
}

static void draw_page_build(uint8_t page_num)
{
    sh1122_clear(&s_oled);
    sh1122_draw_text(&s_oled, 0, 0,  2, "BUILD INFO");
    sh1122_draw_text(&s_oled, 0, 18, 2, "BOARD: PICO 2 WH");
    sh1122_draw_text(&s_oled, 0, 36, 2, "RP2350 M33 / FREERTOS");
    draw_footer_page_indicator(page_num);
}

static void render_page(uint8_t page_idx, uint32_t uptime_s)
{
    switch (page_idx) {
    case 0: draw_page_header(1, uptime_s); break;
    case 1: draw_page_env(2);              break;
    case 2: draw_page_air(3);              break;
    case 3: draw_page_build(4);            break;
    default: draw_page_header(1, uptime_s); break;
    }
    (void)sh1122_flush(&s_oled);
}

/* ── Tasks ──────────────────────────────────────────────────────────────── */

static volatile uint8_t s_page = 0;

static void button_task(void *arg)
{
    (void)arg;
    gpio_init(BOARD_BTN_DISPLAY_PIN);
    gpio_set_dir(BOARD_BTN_DISPLAY_PIN, GPIO_IN);
    gpio_pull_up(BOARD_BTN_DISPLAY_PIN);
    bool last = true;
    for (;;) {
        bool now = gpio_get(BOARD_BTN_DISPLAY_PIN);
        if (last && !now) {
            s_page = (s_page + 1) & 0x03U;
            printf("[oled] button → page %u\n", (unsigned)s_page);
        }
        last = now;
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

static void render_task(void *arg)
{
    (void)arg;
    while (!stdio_usb_connected()) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(500));
    printf("\n[oled] SH1122 bring-up — 256x64 4bpp grayscale, no network\n");

    i2c_bus_init();
    i2c_scan();

    err_t e = sh1122_init(&s_oled, BOARD_I2C_INST, BOARD_OLED_ADDR);
    if (e != ERR_OK) {
        for (;;) {
            printf("[oled] sh1122 init FAILED rc=%d (addr 0x%02x). "
                   "Check VCC=3V3, GND, SDA=GP8, SCL=GP9. "
                   "Some panels are 0x3D — check the breakout solder bridge.\n",
                   (int)e, BOARD_OLED_ADDR);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
    printf("[oled] sh1122 init OK at 0x%02x\n", BOARD_OLED_ADDR);

    s_bme_ok = (bme280_init(&s_bme, BOARD_I2C_INST, BOARD_BME280_ADDR) == ERR_OK);
    printf("[oled] BME280: %s\n", s_bme_ok ? "OK" : "absent/failed");
    s_ens_ok = (ens160_init(&s_ens, BOARD_I2C_INST, BOARD_ENS160_ADDR) == ERR_OK);
    printf("[oled] ENS160: %s\n", s_ens_ok ? "OK" : "absent/failed");

    uint8_t last_page = 255;
    TickType_t boot_tick = xTaskGetTickCount();
    TickType_t last_cycle = boot_tick;

    for (;;) {
        sample_sensors();
        TickType_t now = xTaskGetTickCount();
        if ((now - last_cycle) >= pdMS_TO_TICKS(3000)) {
            s_page = (s_page + 1) & 0x03U;
            last_cycle = now;
        }
        uint32_t uptime_s = (uint32_t)((now - boot_tick) / configTICK_RATE_HZ);
        render_page(s_page, uptime_s);
        if (s_page != last_page) {
            printf("[oled] page=%u uptime=%lu\n", s_page, (unsigned long)uptime_s);
            last_page = s_page;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

int main(void)
{
    stdio_init_all();
    (void)cyw43_arch_init();
    xTaskCreate(button_task, "btn",     1024, NULL, 1, NULL);
    xTaskCreate(render_task, "render",  4096, NULL, 2, NULL);
    vTaskStartScheduler();
    for (;;) { tight_loop_contents(); }
}
