/*
 * KingRO4Y dongle firmware.
 *
 * Build-time application mode (menuconfig -> "KingRO4Y dongle"):
 *   BLE_SIM   (default) phase A  — BLE HID host + simulated amp
 *   USB_PROBE           phase B1 — read-only USB host probe of the FA503
 *   USB_LIVE            phase B2 — BLE knob driving the real FA503 over USB
 * All three optionally drive the on-board LCD (DONGLE_ENABLE_DISPLAY).
 */
#include <stdio.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#if CONFIG_DONGLE_APP_MODE_USB_PROBE || CONFIG_DONGLE_APP_MODE_USB_LIVE
#include "blackbox.h"
#include "hypex_host.h"
#endif

/* Everything except the read-only probe has a knob driving an amp backend. */
#if !CONFIG_DONGLE_APP_MODE_USB_PROBE
#include "amp_backend.h"
#include "ble_input.h"
#include "input_map.h"
#endif

#if CONFIG_DONGLE_APP_MODE_BLE_SIM
#include "driver/gpio.h"
#endif

#if CONFIG_DONGLE_ENABLE_DISPLAY
#include "ui_display.h"
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

#if CONFIG_DONGLE_APP_MODE_BLE_SIM
/* Liveness heartbeat on the green LED (GPIO15 on the ESP32-S3-USB-OTG).
 * Independent of console and LCD: if this blinks, the app is running.
 * BLE_SIM only — in the USB modes hypex_host owns this LED for link status. */
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

#if CONFIG_DONGLE_ENABLE_DISPLAY && !CONFIG_DONGLE_APP_MODE_USB_PROBE
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

#if (CONFIG_DONGLE_APP_MODE_USB_PROBE || CONFIG_DONGLE_APP_MODE_USB_LIVE) && \
    CONFIG_DONGLE_ENABLE_DISPLAY
/* USB host mode kills the console, so put USB faults on the LCD — otherwise a
 * failure to link is nothing but a blinking LED. */
static void usb_diag_cb(const char *msg, const uint8_t *data, int len)
{
    /* Persist it: the console is dead by now, so the next boot reports this
     * during the flash window where a host can actually read it. */
    blackbox_record(msg, data, len);

    char hex[26] = "";
    if (data != NULL && len > 0) {
        int n = len > 8 ? 8 : len;
        for (int i = 0; i < n; i++) {
            snprintf(hex + i * 3, 4, "%02x ", data[i]);
        }
    }
    ui_display_banner("USB FAULT", msg, hex[0] ? hex : NULL);
}
#endif

#if CONFIG_DONGLE_APP_MODE_USB_PROBE || CONFIG_DONGLE_APP_MODE_USB_LIVE
/* Hold the USB PHY (and therefore the COM port) for a few seconds so esptool
 * can auto-reset into download mode. Without this window the firmware destroys
 * its own flashing interface ~1 s after boot. See DONGLE_USB_HOST_START_DELAY_S. */
static void usb_host_flash_window(void)
{
    /* Console is alive right now and about to die — say everything worth
     * saying, including whatever went wrong on the previous run. */
    blackbox_report_previous();
#if CONFIG_DONGLE_USB_HOST_START_DELAY_S > 0
    for (int s = CONFIG_DONGLE_USB_HOST_START_DELAY_S; s > 0; s--) {
        ESP_LOGW(TAG, "USB host starts in %d s — flash NOW if you want to "
                      "(console dies when it starts)", s);
#if CONFIG_DONGLE_ENABLE_DISPLAY
        char l2[32];
        snprintf(l2, sizeof(l2), "USB host in %d s", s);
        ui_display_banner("FLASH WINDOW", l2, "idf.py flash");
#endif
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
}
#endif

#if CONFIG_DONGLE_ENABLE_DISPLAY
/* Non-fatal: a dead LCD must never stop the dongle working. */
static bool start_display(void)
{
    esp_err_t err = ui_display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s — continuing without LCD",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}
#endif

void app_main(void)
{
    init_nvs();

#if CONFIG_DONGLE_APP_MODE_USB_PROBE
    ESP_LOGI(TAG, "KingRO4Y dongle, phase B1: read-only USB host probe");
#if CONFIG_DONGLE_ENABLE_DISPLAY
    bool lcd = start_display();
    if (lcd) hypex_host_set_diag_cb(usb_diag_cb);
    usb_host_flash_window();
    ESP_ERROR_CHECK(hypex_host_start(lcd ? probe_status_cb : NULL));
#else
    usb_host_flash_window();
    ESP_ERROR_CHECK(hypex_host_start(NULL));
#endif

#elif CONFIG_DONGLE_APP_MODE_USB_LIVE
    ESP_LOGI(TAG, "KingRO4Y dongle, phase B2: %s -> real FA503 over USB",
             BLE_INPUT_TARGET_NAME);
#if CONFIG_DONGLE_ENABLE_DISPLAY
    bool lcd = start_display();
#endif
    amp_backend_init();
#if CONFIG_DONGLE_ENABLE_DISPLAY
    if (lcd) {
        xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    }
#endif

    /* BLE BEFORE USB host, deliberately. Installing the USB host driver seizes
     * the shared PHY and kills the USB-Serial-JTAG console, so anything
     * started after it is undebuggable. Order is otherwise irrelevant: the
     * backend drops knob input until the amp has seeded it, so the knob cannot
     * command anything during the window before the link is up. */
    ESP_ERROR_CHECK(ble_input_start(input_map_handle));
    ESP_LOGW(TAG, "BLE up — starting USB host now; the console (COM7) will "
                  "drop here. Watch the LEDs and the LCD from this point.");
    vTaskDelay(pdMS_TO_TICKS(200)); /* let that line flush before the PHY goes */
#if CONFIG_DONGLE_ENABLE_DISPLAY
    if (lcd) hypex_host_set_diag_cb(usb_diag_cb);
#endif
    usb_host_flash_window();
    ESP_ERROR_CHECK(hypex_host_start(NULL));

#else /* CONFIG_DONGLE_APP_MODE_BLE_SIM */
    xTaskCreate(heartbeat_task, "heartbeat", 2048, NULL, 5, NULL);
    ESP_LOGW(TAG, "app_main reached — green LED should be blinking");

    amp_backend_init();
#if CONFIG_DONGLE_ENABLE_DISPLAY
    if (start_display()) {
        xTaskCreate(display_task, "display", 4096, NULL, 3, NULL);
    }
#endif
    ESP_LOGI(TAG, "KingRO4Y dongle, phase A: turn the %s and watch the "
                  "simulated amp state change",
             BLE_INPUT_TARGET_NAME);
    ESP_ERROR_CHECK(ble_input_start(input_map_handle));
#endif
}
