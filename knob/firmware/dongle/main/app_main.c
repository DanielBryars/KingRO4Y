/*
 * KingRO4Y dongle firmware.
 *
 * Build-time application mode (menuconfig -> "KingRO4Y dongle"):
 *   BLE_SIM   (default) phase A — BLE HID host + simulated amp
 *             (+ optional on-board LCD via DONGLE_ENABLE_DISPLAY)
 *   USB_PROBE           phase B1 — read-only USB host probe of the FA503
 */
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_DONGLE_APP_MODE_USB_PROBE
#include "hypex_host.h"
#if CONFIG_DONGLE_ENABLE_DISPLAY
#include "ui_display.h"
#endif
#else
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "amp_backend.h"
#include "ble_input.h"
#include "input_map.h"
#if CONFIG_DONGLE_ENABLE_DISPLAY
#include "ui_display.h"
#endif
#endif

static const char *TAG = "dongle";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

#if !CONFIG_DONGLE_APP_MODE_USB_PROBE
/* Liveness heartbeat on the green LED (GPIO15 on the ESP32-S3-USB-OTG).
 * Independent of console and LCD: if this blinks, the app is running. */
#define HEARTBEAT_LED GPIO_NUM_15
static void heartbeat_task(void *arg)
{
    gpio_config_t io = {.mode = GPIO_MODE_OUTPUT,
                        .pin_bit_mask = 1ULL << HEARTBEAT_LED};
    gpio_config(&io);
    bool on = false;
    while (true) {
        on = !on;
        gpio_set_level(HEARTBEAT_LED, on);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
#endif

#if !CONFIG_DONGLE_APP_MODE_USB_PROBE && CONFIG_DONGLE_ENABLE_DISPLAY
static bool ui_differs(const dongle_ui_state_t *a, const dongle_ui_state_t *b)
{
    return a->volume_db_x100 != b->volume_db_x100 || a->preset != b->preset ||
           a->mute != b->mute || a->input_source != b->input_source ||
           a->ble_connected != b->ble_connected ||
           a->battery_pct != b->battery_pct;
}

static void display_task(void *arg)
{
    dongle_ui_state_t last = {.volume_db_x100 = 0x7fff}; /* force first draw */
    while (true) {
        hypex_state_t st = amp_backend_get_state();
        dongle_ui_state_t ui = {
            .volume_db_x100 = st.volume_db_x100,
            .preset = st.preset,
            .mute = st.mute,
            .input_source = st.input_source,
            .ble_connected = ble_input_is_connected(),
            .battery_pct = ble_input_battery_pct(),
        };
        if (ui_differs(&ui, &last)) {
            ui_display_render(&ui);
            last = ui;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif

#if CONFIG_DONGLE_APP_MODE_USB_PROBE && CONFIG_DONGLE_ENABLE_DISPLAY
/* The probe's status read decodes here; mirror it onto the LCD, since
 * enabling USB host drops the USB-Serial-JTAG console (COM7). */
static void probe_status_cb(const hypex_status_t *st)
{
    dongle_ui_state_t ui = {
        .volume_db_x100 = st->volume_db_x100,
        .preset = st->preset,
        .mute = st->mute,
        .input_source = st->active_input,
        .ble_connected = true, /* green dot = amp reported OK */
        .battery_pct = -1,
    };
    ui_display_render(&ui);
}
#endif

void app_main(void)
{
    init_nvs();

#if CONFIG_DONGLE_APP_MODE_USB_PROBE
    ESP_LOGI(TAG, "KingRO4Y dongle, phase B1: read-only USB host probe");
#if CONFIG_DONGLE_ENABLE_DISPLAY
    esp_err_t derr = ui_display_init();
    if (derr != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s — continuing without LCD",
                 esp_err_to_name(derr));
    }
    ESP_ERROR_CHECK(hypex_host_start(derr == ESP_OK ? probe_status_cb : NULL));
#else
    ESP_ERROR_CHECK(hypex_host_start(NULL));
#endif
#else
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 5, NULL);
    ESP_LOGW(TAG, "app_main reached — green LED should be blinking");

    amp_backend_init();

#if CONFIG_DONGLE_ENABLE_DISPLAY
    esp_err_t derr = ui_display_init();
    if (derr != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s — continuing without LCD",
                 esp_err_to_name(derr));
    } else {
        xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    }
#endif

    ESP_LOGI(TAG, "KingRO4Y dongle, phase A: turn the %s and watch the "
                  "simulated amp state change",
             BLE_INPUT_TARGET_NAME);
    ESP_ERROR_CHECK(ble_input_start(input_map_handle));
#endif
}
