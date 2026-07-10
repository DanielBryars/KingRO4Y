/*
 * ble_input — BLE HID (HOGP) host half of the dongle.
 *
 * Scans for a BLE HID controller (the Fosi Audio VOL20 for bring-up),
 * connects, subscribes to its input reports, and delivers decoded events
 * to a callback. Reconnects automatically when the device drops.
 *
 * Built on ESP-IDF's esp_hid host (esp_hidh) plus the esp_hid_gap helper
 * vendored from the official esp_hid_host example (knob/vendor/...).
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Name the controller advertises. The VOL20 advertises as "VOL20". */
#define BLE_INPUT_TARGET_NAME "VOL20"

typedef enum {
    INPUT_EVT_VOL_UP,     /* Consumer Control 0xE9 Volume Increment */
    INPUT_EVT_VOL_DOWN,   /* 0xEA Volume Decrement */
    INPUT_EVT_MUTE,       /* 0xE2 Mute */
    INPUT_EVT_PLAY_PAUSE, /* 0xCD Play/Pause */
    INPUT_EVT_NEXT,       /* 0xB5 Scan Next Track */
    INPUT_EVT_PREV,       /* 0xB6 Scan Previous Track */
    INPUT_EVT_UNKNOWN,    /* unrecognised report — raw bytes are logged */
} input_event_t;

typedef void (*ble_input_event_cb_t)(input_event_t evt);

/* Initialises BT controller + Bluedroid + esp_hidh and starts the
 * scan/connect task. Call once, after nvs_flash_init(). */
esp_err_t ble_input_start(ble_input_event_cb_t cb);

/* True while a controller is connected. */
bool ble_input_is_connected(void);

/* Last reported controller battery level (0-100), or -1 if unknown. */
int ble_input_battery_pct(void);

#ifdef __cplusplus
}
#endif
