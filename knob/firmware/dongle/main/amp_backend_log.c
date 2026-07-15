/*
 * Phase A amp backend: a simulated FA503 held in RAM and logged to the
 * console, so the BLE half can be exercised end-to-end with no amp attached.
 * Also builds the real Set State packet each change and logs it at DEBUG —
 * exercising hypex_proto on target with the exact bytes phase B will send.
 */
#include "amp_backend.h"

#include "sdkconfig.h"

/* amp_backend_usb.c provides these same symbols in USB_LIVE mode. */
#if !CONFIG_DONGLE_APP_MODE_USB_LIVE

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "amp_sim";

static SemaphoreHandle_t s_lock;
static hypex_state_t s_state = {
    .preset = 1,
    .volume_db_x100 = -4000, /* match the amp's usual resting state */
    .mute = false,
    .input_source = HYPEX_INPUT_OPT,
};

static void log_state(void)
{
    ESP_LOGI(TAG, "preset %u | %+.2f dB | %s | input 0x%02x",
             s_state.preset, s_state.volume_db_x100 / 100.0,
             s_state.mute ? "MUTED" : "unmuted", s_state.input_source);

    /* What phase B would put on the wire (input round-tripped as
     * NO_CHANGE, exactly like hypex_probe.py does). The builder needs a raw
     * status frame to round-trip the persistent tail from; the simulated amp
     * fabricates a minimal one with the usual -40 dB startup volume. */
    uint8_t fake_status[HYPEX_PACKET_LEN] = {0};
    fake_status[0] = 0x05;
    fake_status[21] = 0x60; /* startup volume -40.00 dB, int16 LE */
    fake_status[22] = 0xf0;
    hypex_state_t wire = s_state;
    wire.input_source = HYPEX_INPUT_NO_CHANGE;
    uint8_t pkt[HYPEX_PACKET_LEN];
    if (hypex_build_set_state(pkt, &wire, fake_status)) {
        ESP_LOG_BUFFER_HEX_LEVEL(TAG, pkt, 8, ESP_LOG_DEBUG);
    }
}

void amp_backend_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    configASSERT(s_lock != NULL);
    ESP_LOGI(TAG, "simulated amp backend (phase A — no USB)");
    log_state();
}

hypex_state_t amp_backend_get_state(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    hypex_state_t copy = s_state;
    xSemaphoreGive(s_lock);
    return copy;
}

void amp_backend_volume_delta(int16_t delta_db_x100)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.volume_db_x100 =
        hypex_volume_clamp((int32_t)s_state.volume_db_x100 + delta_db_x100);
    log_state();
    xSemaphoreGive(s_lock);
}

void amp_backend_mute_toggle(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state.mute = !s_state.mute;
    log_state();
    xSemaphoreGive(s_lock);
}

void amp_backend_preset_step(int dir)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int p = (int)s_state.preset - 1 + dir;
    s_state.preset = (uint8_t)((p % 3 + 3) % 3 + 1);
    log_state();
    xSemaphoreGive(s_lock);
}

#endif /* !CONFIG_DONGLE_APP_MODE_USB_LIVE */
