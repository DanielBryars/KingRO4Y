#include "ble_input.h"

#include <string.h>

#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_hid_gap.h"
#include "esp_hidh.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ble_input";

#define SCAN_DURATION_SECONDS 5
#define RECONNECT_DELAY_MS 2000

static ble_input_event_cb_t s_event_cb;
static volatile bool s_connected;
static volatile int s_battery_pct = -1;

bool ble_input_is_connected(void) { return s_connected; }
int ble_input_battery_pct(void) { return s_battery_pct; }

/* --- VOL20 report decoding -------------------------------------------------
 * Captured empirically 2026-07-06: the VOL20 sends an unnumbered 3-byte
 * Consumer Control BITMAP report — one report per detent or gesture,
 * all-zero on release. Fast rotation repeats the same non-zero report
 * back-to-back (one per detent), so every non-zero report is one event.
 *
 * Byte 0 bit assignments seen on the wire:
 *   0x01 / 0x02  the two rotation directions — which is clockwise is
 *                PROVISIONAL (capture was freeform); if volume moves the
 *                wrong way, swap the two cases below
 *   0x20  single click                             — seen once on click
 *   0x08  seen on multi-click gestures             — PROVISIONAL: next-track?
 *   0x04, 0x10  never observed yet                 — PROVISIONAL guesses
 * Bytes 1-2 were always zero.
 * Pin these down with a choreographed test: 3 detents CW, pause, 3 CCW,
 * pause, single click, pause, double, pause, triple, pause, 2 s hold.
 */
static input_event_t decode_vol20_bits(uint8_t bits)
{
    switch (bits) {
    /* 0x01 = counter-clockwise, 0x02 = clockwise (confirmed on hardware
     * 2026-07-09: clockwise must raise the volume). */
    case 0x01: return INPUT_EVT_VOL_DOWN;
    case 0x02: return INPUT_EVT_VOL_UP;
    case 0x20: return INPUT_EVT_PLAY_PAUSE;
    case 0x08: return INPUT_EVT_NEXT;  /* provisional */
    case 0x10: return INPUT_EVT_PREV;  /* provisional */
    case 0x04: return INPUT_EVT_MUTE;  /* provisional */
    default:   return INPUT_EVT_UNKNOWN;
    }
}

static void handle_input_report(uint16_t report_id, const uint8_t *data,
                                uint16_t length)
{
    if (length == 0 || data == NULL) {
        return;
    }
    ESP_LOGD(TAG, "input report id=%u len=%u first=0x%02x", report_id,
             length, data[0]);

    uint8_t bits = data[0];
    if (bits == 0) {
        return; /* release */
    }

    input_event_t evt = decode_vol20_bits(bits);
    if (evt == INPUT_EVT_UNKNOWN) {
        ESP_LOGW(TAG, "unknown report — capture this and extend "
                      "decode_vol20_bits():");
        ESP_LOG_BUFFER_HEX(TAG, data, length);
    } else {
        ESP_LOGI(TAG, "event bits=0x%02x -> %d", bits, evt);
    }
    if (s_event_cb) {
        s_event_cb(evt);
    }
}

static void hidh_callback(void *handler_args, esp_event_base_t base,
                          int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT:
        if (param->open.status == ESP_OK) {
            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            ESP_LOGI(TAG, "connected: " ESP_BD_ADDR_STR " '%s'",
                     ESP_BD_ADDR_HEX(bda),
                     esp_hidh_dev_name_get(param->open.dev));
            esp_hidh_dev_dump(param->open.dev, stdout);
            s_connected = true;
        } else {
            ESP_LOGW(TAG, "open failed (status=%d)", param->open.status);
            s_connected = false;
        }
        break;
    case ESP_HIDH_BATTERY_EVENT:
        s_battery_pct = param->battery.level;
        ESP_LOGI(TAG, "controller battery: %d%%", param->battery.level);
        break;
    case ESP_HIDH_INPUT_EVENT:
        handle_input_report(param->input.report_id, param->input.data,
                            param->input.length);
        break;
    case ESP_HIDH_CLOSE_EVENT:
        ESP_LOGI(TAG, "disconnected");
        /* Required by esp_hidh to release the device's memory. */
        esp_hidh_dev_free(param->close.dev);
        s_connected = false;
        break;
    default:
        break;
    }
}

/* Scan until we find BLE_INPUT_TARGET_NAME (or, failing that, any BLE HID
 * device), open it, then idle until it disconnects and repeat. */
static void scan_task(void *arg)
{
    while (true) {
        if (s_connected) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        size_t num_results = 0;
        esp_hid_scan_result_t *results = NULL;
        ESP_LOGI(TAG, "scanning %ds for '%s' (put it in pairing mode: "
                      "LED flashing blue)...",
                 SCAN_DURATION_SECONDS, BLE_INPUT_TARGET_NAME);
        esp_hid_scan(SCAN_DURATION_SECONDS, &num_results, &results);

        esp_hid_scan_result_t *target = NULL;
        for (esp_hid_scan_result_t *r = results; r != NULL; r = r->next) {
            ESP_LOGI(TAG, "  found " ESP_BD_ADDR_STR " rssi=%d name='%s' "
                          "usage=%s",
                     ESP_BD_ADDR_HEX(r->bda), r->rssi,
                     r->name ? r->name : "(none)",
                     esp_hid_usage_str(r->usage));
            if (r->transport == ESP_HID_TRANSPORT_BLE &&
                r->name != NULL &&
                strcmp(r->name, BLE_INPUT_TARGET_NAME) == 0) {
                target = r;
            }
        }
        /* Fallback: exactly one BLE HID device around — take it. */
        if (target == NULL && num_results == 1 &&
            results->transport == ESP_HID_TRANSPORT_BLE) {
            ESP_LOGW(TAG, "no name match; opening the only device found");
            target = results;
        }

        if (target != NULL) {
            /* Copy what we need before freeing the scan results. */
            esp_bd_addr_t bda;
            memcpy(bda, target->bda, sizeof(bda));
            esp_ble_addr_type_t addr_type = target->ble.addr_type;
            esp_hid_scan_results_free(results);

            ESP_LOGI(TAG, "opening " ESP_BD_ADDR_STR "...",
                     ESP_BD_ADDR_HEX(bda));
            esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE, addr_type);
            /* Result arrives via ESP_HIDH_OPEN_EVENT. Give it a moment
             * before deciding to rescan. */
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
        } else {
            if (results != NULL) {
                esp_hid_scan_results_free(results);
            }
            vTaskDelay(pdMS_TO_TICKS(RECONNECT_DELAY_MS));
        }
    }
}

/* HOGP peripherals require an encrypted (bonded) link before they allow
 * report subscription. "Just Works" bonding: no MITM, no IO capability —
 * appropriate for a knob with no display. */
static void configure_ble_security(void)
{
    esp_ble_auth_req_t auth_req = ESP_LE_AUTH_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req,
                                   sizeof(auth_req));
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap,
                                   sizeof(iocap));
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size,
                                   sizeof(key_size));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key,
                                   sizeof(init_key));
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key,
                                   sizeof(rsp_key));
}

esp_err_t ble_input_start(ble_input_event_cb_t cb)
{
    s_event_cb = cb;

    esp_err_t err = esp_hid_gap_init(HID_HOST_MODE);
    if (err != ESP_OK) {
        return err;
    }
    configure_ble_security();

    err = esp_ble_gattc_register_callback(esp_hidh_gattc_event_handler);
    if (err != ESP_OK) {
        return err;
    }

    esp_hidh_config_t config = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    err = esp_hidh_init(&config);
    if (err != ESP_OK) {
        return err;
    }

    if (xTaskCreate(scan_task, "ble_input_scan", 6 * 1024, NULL, 2, NULL) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
