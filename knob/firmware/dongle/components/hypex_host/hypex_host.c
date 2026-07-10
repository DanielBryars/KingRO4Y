#include "hypex_host.h"

#include "soc/soc_caps.h"

#if !SOC_USB_OTG_SUPPORTED

#include "esp_log.h"
esp_err_t hypex_host_start(hypex_host_status_cb_t cb)
{
    (void)cb;
    ESP_LOGE("hypex_host", "USB host requires a USB-OTG-capable target (S2/S3/P4)");
    return ESP_ERR_NOT_SUPPORTED;
}

#else /* SOC_USB_OTG_SUPPORTED */

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

static const char *TAG = "hypex_host";

/* --- ESP32-S3-USB-OTG board pin map (from esp-bsp esp32_s3_usb_otg.h) ------*/
#define PIN_USB_SEL GPIO_NUM_18       /* 1 = route PHY to USB-A HOST port */
#define PIN_DEV_VBUS_EN GPIO_NUM_12    /* 1 = 5V from USB_DEV port -> host port */
#define PIN_BOOST_EN GPIO_NUM_13       /* 1 = 5V from battery boost (unused here) */
#define PIN_LIMIT_EN GPIO_NUM_17       /* 1 = enable 500 mA current-limit IC */
#define PIN_LED_GREEN GPIO_NUM_15      /* active high */
#define PIN_LED_YELLOW GPIO_NUM_16     /* active high */

/* The FA503's fixed HID endpoints (per HypexUsbProtocol.md / UsbAmpControl).
 * We discover them from the descriptors, but keep these as a sanity check. */
#define EXPECT_EP_OUT 0x01
#define EXPECT_EP_IN 0x81

typedef enum {
    LED_WAITING,     /* yellow solid — VBUS on, no device yet */
    LED_OK,          /* green solid — amp read OK */
    LED_WRONG_DEV,   /* yellow fast blink — a device, but not the FA503 */
    LED_ERROR,       /* both alternate blink */
} led_state_t;

static volatile led_state_t s_led_state = LED_WAITING;
static hypex_host_status_cb_t s_status_cb;

static usb_host_client_handle_t s_client;
static QueueHandle_t s_new_dev_q;         /* uint8_t device addresses */
static SemaphoreHandle_t s_xfer_done;     /* per-transfer completion */
static volatile int s_xfer_status;        /* usb_transfer_status_t of last xfer */

/* --- LEDs -----------------------------------------------------------------*/
static void led_set(int green, int yellow)
{
    gpio_set_level(PIN_LED_GREEN, green);
    gpio_set_level(PIN_LED_YELLOW, yellow);
}

static void led_task(void *arg)
{
    bool phase = false;
    while (true) {
        phase = !phase;
        switch (s_led_state) {
        case LED_WAITING:   led_set(0, 1); break;
        case LED_OK:        led_set(1, 0); break;
        case LED_WRONG_DEV: led_set(0, phase); break;
        case LED_ERROR:     led_set(phase, !phase); break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* --- Board USB host power path --------------------------------------------
 * The Hypex amp's USB port is a *device* port and sources no VBUS, so the
 * dongle (as host) must supply 5 V toward it. On this board that means
 * enabling the current-limited path from the USB_DEV port and routing the
 * PHY to the USB-A host connector. */
static void board_power_on(void)
{
    gpio_config_t io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << PIN_USB_SEL) | (1ULL << PIN_DEV_VBUS_EN) |
                        (1ULL << PIN_BOOST_EN) | (1ULL << PIN_LIMIT_EN) |
                        (1ULL << PIN_LED_GREEN) | (1ULL << PIN_LED_YELLOW),
    };
    gpio_config(&io);

    gpio_set_level(PIN_BOOST_EN, 0);    /* not using battery boost */
    gpio_set_level(PIN_LIMIT_EN, 1);    /* current limit on */
    gpio_set_level(PIN_DEV_VBUS_EN, 1); /* 5 V from USB_DEV port to host port */
    gpio_set_level(PIN_USB_SEL, 1);     /* PHY -> USB-A host connector */
    led_set(0, 1);
    ESP_LOGI(TAG, "USB host power on: VBUS enabled, host port selected");
}

/* --- USB transfers --------------------------------------------------------*/
static void xfer_cb(usb_transfer_t *t)
{
    s_xfer_status = t->status;
    xSemaphoreGive(s_xfer_done);
}

/* Submit one transfer and block until its completion callback fires.
 * Returns the number of bytes actually transferred, or -1 on error. */
static int xfer_sync(usb_transfer_t *t)
{
    if (usb_host_transfer_submit(t) != ESP_OK) {
        return -1;
    }
    if (xSemaphoreTake(s_xfer_done, pdMS_TO_TICKS(1500)) != pdTRUE) {
        return -1;
    }
    if (s_xfer_status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "transfer status %d", s_xfer_status);
        return -1;
    }
    return t->actual_num_bytes;
}

/* Find the HID interface (bInterfaceClass 0x03) and its interrupt IN/OUT
 * endpoint addresses. Returns false if not found. */
static bool find_hid_endpoints(const usb_config_desc_t *cfg,
                               uint8_t *intf_num, uint8_t *alt,
                               uint8_t *ep_in, uint8_t *ep_out)
{
    for (int i = 0; i < cfg->bNumInterfaces; i++) {
        int off = 0;
        const usb_intf_desc_t *intf =
            usb_parse_interface_descriptor(cfg, i, 0, &off);
        if (intf == NULL || intf->bInterfaceClass != USB_CLASS_HID) {
            continue;
        }
        uint8_t in = 0, out = 0;
        for (int e = 0; e < intf->bNumEndpoints; e++) {
            int ep_off = off;
            const usb_ep_desc_t *ep = usb_parse_endpoint_descriptor_by_index(
                intf, e, cfg->wTotalLength, &ep_off);
            if (ep == NULL) {
                continue;
            }
            if ((ep->bmAttributes & USB_BM_ATTRIBUTES_XFERTYPE_MASK) !=
                USB_BM_ATTRIBUTES_XFER_INT) {
                continue;
            }
            if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                in = ep->bEndpointAddress;
            } else {
                out = ep->bEndpointAddress;
            }
        }
        if (in && out) {
            *intf_num = intf->bInterfaceNumber;
            *alt = intf->bAlternateSetting;
            *ep_in = in;
            *ep_out = out;
            return true;
        }
    }
    return false;
}

/* Do the read-only probe against one opened device. */
static void probe_device(usb_device_handle_t dev)
{
    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK) {
        s_led_state = LED_ERROR;
        return;
    }
    ESP_LOGI(TAG, "device VID=0x%04x PID=0x%04x", dd->idVendor, dd->idProduct);

    if (dd->idVendor != HYPEX_USB_VID || dd->idProduct != HYPEX_USB_PID) {
        ESP_LOGW(TAG, "not the FA503 (expected %04x:%04x) — ignoring",
                 HYPEX_USB_VID, HYPEX_USB_PID);
        s_led_state = LED_WRONG_DEV;
        return;
    }

    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        s_led_state = LED_ERROR;
        return;
    }

    uint8_t intf_num, alt, ep_in, ep_out;
    if (!find_hid_endpoints(cfg, &intf_num, &alt, &ep_in, &ep_out)) {
        ESP_LOGE(TAG, "no HID interface with interrupt IN+OUT endpoints");
        s_led_state = LED_ERROR;
        return;
    }
    ESP_LOGI(TAG, "HID interface %u alt %u  EP IN 0x%02x  EP OUT 0x%02x",
             intf_num, alt, ep_in, ep_out);
    if (ep_in != EXPECT_EP_IN || ep_out != EXPECT_EP_OUT) {
        ESP_LOGW(TAG, "endpoints differ from the expected 0x81/0x01 — "
                      "using the discovered ones");
    }

    if (usb_host_interface_claim(s_client, dev, intf_num, alt) != ESP_OK) {
        ESP_LOGE(TAG, "interface claim failed");
        s_led_state = LED_ERROR;
        return;
    }

    usb_transfer_t *out_xfer = NULL, *in_xfer = NULL;
    bool ok = false;
    if (usb_host_transfer_alloc(HYPEX_PACKET_LEN, 0, &out_xfer) != ESP_OK ||
        usb_host_transfer_alloc(HYPEX_PACKET_LEN, 0, &in_xfer) != ESP_OK) {
        goto cleanup;
    }

    /* Build the get-status request. Guard: only ever a SAFE read opcode. */
    hypex_build_get_status(out_xfer->data_buffer);
    configASSERT(hypex_is_safe_read_opcode(out_xfer->data_buffer[0]));
    out_xfer->num_bytes = HYPEX_PACKET_LEN;
    out_xfer->device_handle = dev;
    out_xfer->bEndpointAddress = ep_out;
    out_xfer->callback = xfer_cb;
    out_xfer->timeout_ms = 1000;

    in_xfer->num_bytes = HYPEX_PACKET_LEN;
    in_xfer->device_handle = dev;
    in_xfer->bEndpointAddress = ep_in;
    in_xfer->callback = xfer_cb;
    in_xfer->timeout_ms = 1000;

    if (xfer_sync(out_xfer) < 0) {
        ESP_LOGE(TAG, "status request (OUT) failed");
        goto cleanup;
    }
    int n = xfer_sync(in_xfer);
    if (n < 0) {
        ESP_LOGE(TAG, "status response (IN) failed");
        goto cleanup;
    }

    ESP_LOG_BUFFER_HEX(TAG, in_xfer->data_buffer, n);
    hypex_status_t st;
    if (!hypex_parse_status(in_xfer->data_buffer, n, &st)) {
        ESP_LOGE(TAG, "response did not decode as a status frame");
        goto cleanup;
    }

    ESP_LOGI(TAG, "AMP OK: preset %u | %+.2f dB | %s | input 0x%02x",
             st.preset, st.volume_db_x100 / 100.0,
             st.mute ? "MUTED" : "unmuted", st.active_input);
    if (s_status_cb) {
        s_status_cb(&st);
    }
    s_led_state = LED_OK;
    ok = true;

cleanup:
    if (out_xfer) usb_host_transfer_free(out_xfer);
    if (in_xfer) usb_host_transfer_free(in_xfer);
    usb_host_interface_release(s_client, dev, intf_num);
    if (!ok && s_led_state != LED_WRONG_DEV) {
        s_led_state = LED_ERROR;
    }
}

/* --- USB host plumbing ----------------------------------------------------*/
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV: {
        uint8_t addr = msg->new_dev.address;
        xQueueSend(s_new_dev_q, &addr, 0);
        break;
    }
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "device disconnected");
        s_led_state = LED_WAITING;
        break;
    default:
        break;
    }
}

static void host_lib_task(void *arg)
{
    while (true) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void client_task(void *arg)
{
    while (true) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

static void probe_task(void *arg)
{
    uint8_t addr;
    while (true) {
        if (xQueueReceive(s_new_dev_q, &addr, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        usb_device_handle_t dev;
        if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
            ESP_LOGE(TAG, "device open failed (addr %u)", addr);
            s_led_state = LED_ERROR;
            continue;
        }
        probe_device(dev);
        usb_host_device_close(s_client, dev);
    }
}

esp_err_t hypex_host_start(hypex_host_status_cb_t cb)
{
    s_status_cb = cb;
    s_new_dev_q = xQueueCreate(4, sizeof(uint8_t));
    s_xfer_done = xSemaphoreCreateBinary();
    if (!s_new_dev_q || !s_xfer_done) {
        return ESP_ERR_NO_MEM;
    }

    board_power_on();
    xTaskCreate(led_task, "hh_led", 2048, NULL, 3, NULL);

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_install: %s", esp_err_to_name(err));
        return err;
    }

    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    err = usb_host_client_register(&client_config, &s_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "usb_host_client_register: %s", esp_err_to_name(err));
        return err;
    }

    xTaskCreate(host_lib_task, "usb_lib", 4096, NULL, 5, NULL);
    xTaskCreate(client_task, "usb_client", 4096, NULL, 4, NULL);
    xTaskCreate(probe_task, "usb_probe", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "USB host probe running — connect the FA503 (Mini-B) to the "
                  "USB-A host port");
    return ESP_OK;
}

#endif /* SOC_USB_OTG_SUPPORTED */
