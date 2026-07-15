#include "blackbox.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "blackbox";

#define BB_NS "bbox"
#define BB_KEY_MSG "msg"
#define BB_KEY_HEX "hex"
#define BB_KEY_CNT "cnt"
#define BB_HEX_LEN 8

static bool s_recorded_this_boot;

void blackbox_report_previous(void)
{
    nvs_handle_t h;
    if (nvs_open(BB_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }

    char msg[64];
    size_t msg_len = sizeof(msg);
    if (nvs_get_str(h, BB_KEY_MSG, msg, &msg_len) != ESP_OK) {
        nvs_close(h);
        ESP_LOGI(TAG, "no fault recorded on the previous run");
        return;
    }

    uint8_t hex[BB_HEX_LEN] = {0};
    size_t hex_len = sizeof(hex);
    bool have_hex = nvs_get_blob(h, BB_KEY_HEX, hex, &hex_len) == ESP_OK;

    uint32_t cnt = 0;
    nvs_get_u32(h, BB_KEY_CNT, &cnt);

    ESP_LOGE(TAG, "=========== FAULT FROM PREVIOUS RUN ===========");
    ESP_LOGE(TAG, "  %s   (occurrence #%u overall)", msg, (unsigned)cnt);
    if (have_hex && hex_len > 0) {
        char line[3 * BB_HEX_LEN + 1] = "";
        for (size_t i = 0; i < hex_len; i++) {
            snprintf(line + i * 3, 4, "%02x ", hex[i]);
        }
        ESP_LOGE(TAG, "  first 8 bytes of the amp's reply: %s", line);
    }
    ESP_LOGE(TAG, "===============================================");

    /* Clear so the next boot's report reflects the next run, not this one. */
    nvs_erase_key(h, BB_KEY_MSG);
    nvs_erase_key(h, BB_KEY_HEX);
    nvs_commit(h);
    nvs_close(h);
}

void blackbox_record(const char *msg, const uint8_t *data, int len)
{
    if (s_recorded_this_boot || msg == NULL) {
        return;
    }
    s_recorded_this_boot = true;

    nvs_handle_t h;
    if (nvs_open(BB_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, BB_KEY_MSG, msg);
    if (data != NULL && len > 0) {
        size_t n = len > BB_HEX_LEN ? BB_HEX_LEN : (size_t)len;
        nvs_set_blob(h, BB_KEY_HEX, data, n);
    }
    uint32_t cnt = 0;
    nvs_get_u32(h, BB_KEY_CNT, &cnt);
    nvs_set_u32(h, BB_KEY_CNT, cnt + 1);
    nvs_commit(h);
    nvs_close(h);
}
