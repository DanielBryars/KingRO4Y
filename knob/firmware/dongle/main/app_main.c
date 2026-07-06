/*
 * KingRO4Y dongle firmware — phase A.
 *
 * BLE HID host bring-up against the Fosi Audio VOL20, driving a *simulated*
 * amp (console logging). Runs on any ESP32 with BLE (bring-up board:
 * Olimex ESP32-DevKit-LiPo / WROOM-32E). Phase B adds the real USB host
 * backend on ESP32-S3 behind the same amp_backend interface.
 */
#include "amp_backend.h"
#include "ble_input.h"
#include "esp_log.h"
#include "input_map.h"
#include "nvs_flash.h"

static const char *TAG = "dongle";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    amp_backend_init();

    ESP_LOGI(TAG, "KingRO4Y dongle, phase A: turn the %s and watch the "
                  "simulated amp state change",
             BLE_INPUT_TARGET_NAME);
    ESP_ERROR_CHECK(ble_input_start(input_map_handle));
}
