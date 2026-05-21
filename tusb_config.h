#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

/* ── Controller ───────────────────────────────────────────────────────────── */
#define CFG_TUSB_MCU            OPT_MCU_RP2350

/* ── RTOS ────────────────────────────────────────────────────────────────── */
#define CFG_TUSB_OS             OPT_OS_FREERTOS

/* ── Device-only build ────────────────────────────────────────────────────── */
#define CFG_TUD_ENABLED         1

/* ── Debug (off in release) ───────────────────────────────────────────────── */
#ifndef NDEBUG
#  define CFG_TUSB_DEBUG        0
#endif

/* ── CDC interfaces ───────────────────────────────────────────────────────── */
/* CDC0: mTLS-encrypted MQTT byte stream (binary, no line discipline).
 * CDC1: stdio / log output.
 * See CLAUDE.md §8.1. */
#define CFG_TUD_CDC             2
#define CFG_TUD_CDC_RX_BUFSIZE  512
#define CFG_TUD_CDC_TX_BUFSIZE  1024

/* ── Unused class drivers (keep zeros to reduce code size) ───────────────── */
#define CFG_TUD_MSC             0
#define CFG_TUD_HID             0
#define CFG_TUD_MIDI            0
#define CFG_TUD_VENDOR          0

#endif /* TUSB_CONFIG_H */
