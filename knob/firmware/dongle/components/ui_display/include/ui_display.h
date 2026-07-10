/*
 * ui_display — on-board 1.3" ST7789 LCD (240x240) on the ESP32-S3-USB-OTG.
 *
 * Generic: knows nothing about BLE or the amp. The caller builds a
 * dongle_ui_state_t and calls ui_display_render(). Rendering is done in
 * horizontal bands with a small DMA buffer, so it needs no PSRAM and
 * coexists with Bluedroid.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t volume_db_x100; /* dB * 100 */
    uint8_t preset;         /* 1..3 */
    bool mute;
    uint8_t input_source;   /* hypex_input_t value */
    bool ble_connected;
    int battery_pct;        /* 0..100, or <0 if unknown */
} dongle_ui_state_t;

/* Brings up SPI3 + the ST7789 panel + backlight and shows a splash. */
esp_err_t ui_display_init(void);

/* Redraws the whole screen from the given state. Cheap to call at a few Hz. */
void ui_display_render(const dongle_ui_state_t *s);

#ifdef __cplusplus
}
#endif
