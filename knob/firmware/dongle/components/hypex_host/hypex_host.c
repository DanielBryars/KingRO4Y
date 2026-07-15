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
bool hypex_host_is_connected(void) { return false; }
void hypex_host_set_diag_cb(hypex_host_diag_cb_t cb) { (void)cb; }
esp_err_t hypex_host_read_status(hypex_status_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t hypex_host_write_state(const hypex_state_t *st)
{
    (void)st;
    return ESP_ERR_NOT_SUPPORTED;
}

#else /* SOC_USB_OTG_SUPPORTED */

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
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

/* How long we wait for a transfer to complete (see xfer_sync). */
#define XFER_WAIT_MS 1500
/* The amp may need a moment after the interface is claimed before it will
 * answer; retry the opening read rather than declaring the link dead. */
#define OPEN_READ_TRIES 3

typedef enum {
    LED_WAITING,     /* yellow solid — VBUS on, no device yet */
    LED_OK,          /* green solid — amp read OK */
    LED_WRONG_DEV,   /* yellow fast blink — a device, but not the FA503 */
    LED_ERROR,       /* both alternate blink */
} led_state_t;

static volatile led_state_t s_led_state = LED_WAITING;
static hypex_host_status_cb_t s_status_cb;
static hypex_host_diag_cb_t s_diag_cb;

void hypex_host_set_diag_cb(hypex_host_diag_cb_t cb) { s_diag_cb = cb; }

static void diag(const char *msg, const uint8_t *data, int len)
{
    ESP_LOGE(TAG, "%s", msg);
    if (s_diag_cb) {
        s_diag_cb(msg, data, len);
    }
}

static usb_host_client_handle_t s_client;

/* Per-transfer completion tracking (in usb_transfer_t.context). Two transfers
 * can now be in flight at once — the IN is armed BEFORE the OUT request goes
 * out, like Windows' always-pending HID read; see read_status_locked. */
typedef struct {
    SemaphoreHandle_t done;
    volatile int status; /* usb_transfer_status_t of the last completion */
} xfer_track_t;
static xfer_track_t s_out_track, s_in_track, s_ctrl_track;

/* Connection events from the USB client callback to conn_task. Teardown must
 * not happen in the callback itself (it blocks), so DEV_GONE is queued too. */
typedef enum { EVT_NEW_DEV, EVT_DEV_GONE } host_evt_type_t;
typedef struct {
    host_evt_type_t type;
    uint8_t addr; /* EVT_NEW_DEV only */
} host_evt_t;
static QueueHandle_t s_evt_q;

/* --- The open link. Held from connect until DEV_GONE, so transactions can
 * reuse the claimed interface and the two allocated transfers. All of this
 * is owned by conn_task; readers/writers go through s_link_mutex. ----------*/
static SemaphoreHandle_t s_link_mutex;
static volatile bool s_connected;
static usb_device_handle_t s_dev;
static uint8_t s_intf_num;
static uint8_t s_ep_in, s_ep_out;
static usb_transfer_t *s_out_xfer, *s_in_xfer;

bool hypex_host_is_connected(void) { return s_connected; }

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
    xfer_track_t *trk = (xfer_track_t *)t->context;
    trk->status = t->status;
    xSemaphoreGive(trk->done);
}

/* NOTE: usb_transfer_t.timeout_ms is NOT implemented by ESP-IDF's USB Host
 * library — it is accepted and ignored. A transfer the device never answers
 * therefore stays queued forever, so the wait in xfer_wait is OUR timeout,
 * and when it expires the transfer is STILL IN FLIGHT. Reusing it then is a
 * bug (submit returns INVALID_STATE, or a late completion fires against a
 * buffer we have moved on from). Callers must ep_recover the endpoint (or
 * treat the link as dead) after a timeout. */
static int xfer_submit(usb_transfer_t *t)
{
    xfer_track_t *trk = (xfer_track_t *)t->context;
    /* Drop any completion left over from a previously timed-out transfer, so
     * we can't mistake it for this one's. */
    xSemaphoreTake(trk->done, 0);
    esp_err_t err = usb_host_transfer_submit(t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "submit ep=0x%02x failed: %s", t->bEndpointAddress,
                 esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int xfer_wait(usb_transfer_t *t)
{
    xfer_track_t *trk = (xfer_track_t *)t->context;
    if (xSemaphoreTake(trk->done, pdMS_TO_TICKS(XFER_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "ep=0x%02x NO COMPLETION after %d ms — transfer is still "
                      "in flight; link is now unusable",
                 t->bEndpointAddress, XFER_WAIT_MS);
        return -1;
    }
    if (trk->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "ep=0x%02x transfer status %d", t->bEndpointAddress,
                 trk->status);
        return -1;
    }
    return t->actual_num_bytes;
}

/* Submit one transfer and block until its completion callback fires.
 * Returns the number of bytes actually transferred, or -1 on error. */
static int xfer_sync(usb_transfer_t *t)
{
    if (xfer_submit(t) < 0) {
        return -1;
    }
    return xfer_wait(t);
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

static void ep_recover(usb_device_handle_t dev, uint8_t ep);

/* Minimum spacing between ANY two transactions to the amp, not just writes.
 * HFD's own traffic never goes below 53 ms between requests (2026-05-03
 * pcap), and on 2026-07-15 running three back-to-back txns per volume flush
 * (pre-write read / Set State / verify read, ~1 ms apart) drove the amp's
 * protocol handler into a zero-response wedge that only a bus reset cleared.
 * The amp needs breathing room between requests, full stop. */
#define TXN_MIN_GAP_MS 60
static int64_t s_last_txn_us;

static void txn_throttle(void)
{
    int64_t since_ms = (esp_timer_get_time() - s_last_txn_us) / 1000;
    if (since_ms < TXN_MIN_GAP_MS) {
        vTaskDelay(pdMS_TO_TICKS(TXN_MIN_GAP_MS - (int)since_ms));
    }
}

/* One request/response round trip on the claimed interface. Caller holds
 * s_link_mutex and has checked s_connected. Returns bytes read, or -1.
 *
 * ORDER MATTERS: the IN is armed BEFORE the OUT request goes out. A real HID
 * host (Windows) keeps an IN transfer permanently pending, so IN tokens are
 * already flowing when the device wants to reply. Observed on 2026-07-15:
 * with the IN submitted only after the OUT completed, the FA503 ACKed every
 * request and never delivered a response (fresh amp, fresh dongle, any
 * order of attach) — yet answered Windows normally. Arming the IN first
 * matches the host behaviour the amp is evidently written against. */
static int txn_locked(int *out_len)
{
    txn_throttle();

    s_in_xfer->num_bytes = HYPEX_PACKET_LEN;
    int n = -1;
    if (xfer_submit(s_in_xfer) < 0) {
        goto done;
    }
    if (xfer_submit(s_out_xfer) < 0) {
        /* The armed IN is still in flight — reap it before anyone reuses it. */
        ep_recover(s_dev, s_ep_in);
        xfer_wait(s_in_xfer); /* consumes the CANCELED completion */
        goto done;
    }
    if (xfer_wait(s_out_xfer) < 0) {
        ESP_LOGE(TAG, "request (OUT) failed");
        ep_recover(s_dev, s_ep_in);
        xfer_wait(s_in_xfer);
        goto done;
    }
    n = xfer_wait(s_in_xfer);
    if (n < 0) {
        ESP_LOGE(TAG, "response (IN) failed");
    } else if (out_len) {
        *out_len = n;
    }
done:
    s_last_txn_us = esp_timer_get_time();
    return n;
}

/*
 * Send the SAFE get-status request and return the reply.
 *
 * Pairing is maintained by ARMING THE IN BEFORE THE OUT (txn_locked): with an
 * IN always pending when the request lands, the amp's response goes into it,
 * one-for-one — exactly how hidapi/Windows drive this device. An earlier
 * design instead validated the reply by its packet_id byte, expecting the
 * 0x06 historically seen there for get-status replies; on 2026-07-15 the amp
 * answered every get-status (hidapi AND this path) with packet_id 0x00, so
 * that byte is treated as informational, not as a validity check.
 */
static esp_err_t read_status_locked(hypex_status_t *out)
{
    /* Guard: this path only ever emits a SAFE read opcode. */
    hypex_build_get_status(s_out_xfer->data_buffer);
    configASSERT(hypex_is_safe_read_opcode(s_out_xfer->data_buffer[0]));
    s_out_xfer->num_bytes = HYPEX_PACKET_LEN;

    /* One retry: a bad frame can be a one-off (a leftover notification, or
     * the amp's zeroed-packet complaint) — the retry goes out paced by
     * txn_throttle, which is usually all the amp wanted. */
    for (int attempt = 0; attempt < 2; attempt++) {
        int n = txn_locked(NULL);
        if (n < 0) {
            ESP_LOGE(TAG, "get-status txn failed");
            return ESP_FAIL;
        }
        if (hypex_parse_status(s_in_xfer->data_buffer, n, out)) {
            ESP_LOGD(TAG, "status reply packet_id 0x%02x", out->packet_id);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "bad status frame (attempt %d/2, %d bytes)", attempt + 1,
                 n);
    }
    diag("bad status frame", s_in_xfer->data_buffer, 8);
    return ESP_ERR_INVALID_RESPONSE;
}

esp_err_t hypex_host_read_status(hypex_status_t *out)
{
    if (out == NULL) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(s_link_mutex, portMAX_DELAY);
    esp_err_t err =
        s_connected ? read_status_locked(out) : ESP_ERR_INVALID_STATE;
    xSemaphoreGive(s_link_mutex);
    return err;
}

esp_err_t hypex_host_write_state(const hypex_state_t *st)
{
    if (st == NULL) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_link_mutex, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_connected) {
        /* READ-MODIFY-WRITE, atomically under the link mutex. A Set State's
         * bytes 7..36 carry persistent config (startup volume at 21-22) that
         * the amp re-applies wholesale, so the packet must be built from a
         * FRESH status frame — zero-filling that tail is what silently
         * reprogrammed the amp's power-on volume to 0.0 dB (2026-07-15). */
        hypex_status_t cur;
        err = read_status_locked(&cur);
        if (err == ESP_OK) {
            /* The frame we just validated is still in the IN buffer. */
            uint8_t pkt[HYPEX_PACKET_LEN];
            if (!hypex_build_set_state(pkt, st, s_in_xfer->data_buffer)) {
                ESP_LOGE(TAG,
                         "refusing malformed Set State (preset %u vol %d in %u)",
                         st->preset, st->volume_db_x100, st->input_source);
                err = ESP_ERR_INVALID_ARG;
            } else if (pkt[0] != 0x05) {
                /* SAFETY BACKSTOP: 0x05 is the only opcode this transport may
                 * ever write. Anything else (0x07/0x09/0x0a+) is documented as
                 * able to hang or brick the amp — see the Safe-Opcode Policy
                 * in knob/docs/experiments.md. */
                ESP_LOGE(TAG, "BUG: non-Set-State write opcode 0x%02x blocked",
                         pkt[0]);
                err = ESP_ERR_INVALID_ARG;
            } else {
                memcpy(s_out_xfer->data_buffer, pkt, HYPEX_PACKET_LEN);
                s_out_xfer->num_bytes = HYPEX_PACKET_LEN;
                /* Set State returns a response frame (packet_id 0x00), and it
                 * MUST be read. The amp's IN pipe is strictly one response per
                 * request, so an undrained response desyncs every later read
                 * by one packet — the stale-buffer trap that once reported a
                 * preset change which had not happened, and the reason
                 * hypex_drain.py exists. The reference implementation
                 * (hypex_probe.py set_state) reads it too; we discard the
                 * contents and verify with a fresh read instead. */
                err = (txn_locked(NULL) < 0) ? ESP_FAIL : ESP_OK;
            }
        } else {
            ESP_LOGE(TAG, "pre-write status read failed — Set State not sent");
        }
    }
    xSemaphoreGive(s_link_mutex);
    return err;
}

/* --- HID class initialization ---------------------------------------------
 * Windows performs these at enumeration (visible in the 2026-05-03 HFD pcap):
 * it reads the HID report descriptor and sends SET_IDLE(0). Our raw-endpoint
 * host skipped both, and the FA503 was observed on 2026-07-15 to enumerate
 * but never answer the vendor protocol for such a host (it answered Windows
 * minutes apart) — evidently the amp gates its vendor pipe on looking like a
 * properly initialised HID host. So: mimic Windows. */
#define REPORT_DESC_MAX 256
#define HID_DESC_TYPE_REPORT 0x22 /* HID 1.11 §7.1.1 (not in IDF's ch9 set) */

static int ctrl_sync(usb_transfer_t *t)
{
    xfer_track_t *trk = (xfer_track_t *)t->context;
    xSemaphoreTake(trk->done, 0);
    esp_err_t err = usb_host_transfer_submit_control(s_client, t);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "control submit failed: %s", esp_err_to_name(err));
        return -1;
    }
    if (xSemaphoreTake(trk->done, pdMS_TO_TICKS(XFER_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "control transfer: no completion after %d ms",
                 XFER_WAIT_MS);
        return -1;
    }
    if (trk->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGW(TAG, "control transfer status %d", trk->status);
        return -1;
    }
    return t->actual_num_bytes;
}

static void hid_class_init(usb_device_handle_t dev, uint8_t intf_num)
{
    usb_transfer_t *ctrl = NULL;
    if (usb_host_transfer_alloc(sizeof(usb_setup_packet_t) + REPORT_DESC_MAX,
                                0, &ctrl) != ESP_OK) {
        ESP_LOGW(TAG, "no memory for HID init transfer — skipping");
        return;
    }
    ctrl->device_handle = dev;
    ctrl->bEndpointAddress = 0;
    ctrl->callback = xfer_cb;
    ctrl->context = &s_ctrl_track;

    /* GET_DESCRIPTOR(HID report descriptor) on the interface. */
    usb_setup_packet_t *setup = (usb_setup_packet_t *)ctrl->data_buffer;
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_IN |
                           USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
    setup->wValue = (HID_DESC_TYPE_REPORT << 8);
    setup->wIndex = intf_num;
    setup->wLength = REPORT_DESC_MAX;
    ctrl->num_bytes = sizeof(usb_setup_packet_t) + REPORT_DESC_MAX;
    int n = ctrl_sync(ctrl);
    if (n < 0) {
        ESP_LOGW(TAG, "HID report descriptor read failed (continuing)");
    } else {
        ESP_LOGI(TAG, "HID report descriptor: %d bytes",
                 n - (int)sizeof(usb_setup_packet_t));
    }

    /* SET_IDLE(duration 0, all reports) — the classic HID class request
     * Windows always sends. A STALL here would be non-fatal. */
    setup->bmRequestType = USB_BM_REQUEST_TYPE_DIR_OUT |
                           USB_BM_REQUEST_TYPE_TYPE_CLASS |
                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
    setup->bRequest = 0x0a; /* SET_IDLE */
    setup->wValue = 0;
    setup->wIndex = intf_num;
    setup->wLength = 0;
    ctrl->num_bytes = sizeof(usb_setup_packet_t);
    if (ctrl_sync(ctrl) < 0) {
        ESP_LOGW(TAG, "SET_IDLE failed (continuing)");
    } else {
        ESP_LOGI(TAG, "SET_IDLE(0) sent");
    }

    usb_host_transfer_free(ctrl);
}

/* Cancel anything stuck on an endpoint and make it usable again. Needed
 * because a transfer we gave up waiting for is still queued in the host
 * library (see xfer_sync); without flushing it, the next submit on that
 * endpoint fails. flush() completes the stranded transfers with a CANCELED
 * status, whose callbacks give the transfer's done semaphore — xfer_submit
 * drains that stale
 * count before its next submit. */
static void ep_recover(usb_device_handle_t dev, uint8_t ep)
{
    usb_host_endpoint_halt(dev, ep);
    usb_host_endpoint_flush(dev, ep);
    usb_host_endpoint_clear(dev, ep);
}

/* Enumerate, validate, claim, and hold the link open. */
static void link_open(uint8_t addr)
{
    usb_device_handle_t dev;
    if (usb_host_device_open(s_client, addr, &dev) != ESP_OK) {
        ESP_LOGE(TAG, "device open failed (addr %u)", addr);
        s_led_state = LED_ERROR;
        return;
    }

    const usb_device_desc_t *dd;
    if (usb_host_get_device_descriptor(dev, &dd) != ESP_OK) {
        s_led_state = LED_ERROR;
        goto close;
    }
    ESP_LOGI(TAG, "device VID=0x%04x PID=0x%04x", dd->idVendor, dd->idProduct);

    if (dd->idVendor != HYPEX_USB_VID || dd->idProduct != HYPEX_USB_PID) {
        ESP_LOGW(TAG, "not the FA503 (expected %04x:%04x) — ignoring",
                 HYPEX_USB_VID, HYPEX_USB_PID);
        s_led_state = LED_WRONG_DEV;
        goto close;
    }

    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(dev, &cfg) != ESP_OK) {
        s_led_state = LED_ERROR;
        goto close;
    }

    uint8_t intf_num, alt, ep_in, ep_out;
    if (!find_hid_endpoints(cfg, &intf_num, &alt, &ep_in, &ep_out)) {
        diag("no HID interrupt IN+OUT endpoints", NULL, 0);
        s_led_state = LED_ERROR;
        goto close;
    }
    ESP_LOGI(TAG, "HID interface %u alt %u  EP IN 0x%02x  EP OUT 0x%02x",
             intf_num, alt, ep_in, ep_out);
    if (ep_in != EXPECT_EP_IN || ep_out != EXPECT_EP_OUT) {
        ESP_LOGW(TAG, "endpoints differ from the expected 0x81/0x01 — "
                      "using the discovered ones");
    }

    if (usb_host_interface_claim(s_client, dev, intf_num, alt) != ESP_OK) {
        diag("interface claim failed", NULL, 0);
        s_led_state = LED_ERROR;
        goto close;
    }

    /* Mimic Windows' HID enumeration before touching the vendor pipe —
     * without this the amp has been seen to enumerate but never answer. */
    hid_class_init(dev, intf_num);

    /* NULL first: a failing alloc leaves its out-param untouched, and the
     * free_xfers path must not free a stale pointer. */
    s_out_xfer = s_in_xfer = NULL;
    if (usb_host_transfer_alloc(HYPEX_PACKET_LEN, 0, &s_out_xfer) != ESP_OK ||
        usb_host_transfer_alloc(HYPEX_PACKET_LEN, 0, &s_in_xfer) != ESP_OK) {
        ESP_LOGE(TAG, "transfer alloc failed");
        s_led_state = LED_ERROR;
        goto free_xfers; /* the first alloc may have succeeded */
    }

    s_out_xfer->device_handle = dev;
    s_out_xfer->bEndpointAddress = ep_out;
    s_out_xfer->callback = xfer_cb;
    s_out_xfer->context = &s_out_track;
    s_out_xfer->timeout_ms = 1000;

    s_in_xfer->num_bytes = HYPEX_PACKET_LEN;
    s_in_xfer->device_handle = dev;
    s_in_xfer->bEndpointAddress = ep_in;
    s_in_xfer->callback = xfer_cb;
    s_in_xfer->context = &s_in_track;
    s_in_xfer->timeout_ms = 1000;

    s_dev = dev;
    s_intf_num = intf_num;
    s_ep_in = ep_in;
    s_ep_out = ep_out;

    /* Publish the link only once the opening read has proved it works. While
     * s_connected is still false no other task can enter a transaction, so we
     * may use the _locked helper directly — and on failure we can tear the
     * transfers down knowing nobody else ever saw them. */
    hypex_status_t st;
    esp_err_t err = ESP_FAIL;
    for (int i = 0; i < OPEN_READ_TRIES; i++) {
        xSemaphoreTake(s_link_mutex, portMAX_DELAY);
        err = read_status_locked(&st);
        if (err == ESP_OK) {
            s_connected = true;
        }
        xSemaphoreGive(s_link_mutex);
        if (err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "opening read %d/%d failed — flushing endpoints, retrying",
                 i + 1, OPEN_READ_TRIES);
        ep_recover(dev, ep_in);
        ep_recover(dev, ep_out);
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    if (err != ESP_OK) {
        diag("opening read failed", NULL, 0);
        s_led_state = LED_ERROR;
        goto free_xfers;
    }

    /* Integer dB formatting: ESP-IDF's nano newlib drops %f. */
    int v100 = st.volume_db_x100, av = v100 < 0 ? -v100 : v100;
    ESP_LOGI(TAG, "AMP OK: preset %u | %s%d.%02d dB | %s | input 0x%02x",
             st.preset, v100 < 0 ? "-" : "", av / 100, av % 100,
             st.mute ? "MUTED" : "unmuted", st.active_input);
    s_led_state = LED_OK;
    if (s_status_cb) {
        s_status_cb(&st);
    }
    return; /* link stays open */

free_xfers: /* falls through: free transfers, release interface, close device */
    if (s_out_xfer) usb_host_transfer_free(s_out_xfer);
    if (s_in_xfer) usb_host_transfer_free(s_in_xfer);
    s_out_xfer = s_in_xfer = NULL;
    usb_host_interface_release(s_client, dev, intf_num);
close:
    usb_host_device_close(s_client, dev);
}

/* Teardown runs entirely under s_link_mutex. If a transaction is mid-flight
 * the take blocks until it finishes, so we can never free a transfer that is
 * still submitted, and any task that acquires the mutex afterwards sees
 * s_connected == false and bails before touching the freed handles. */
static void link_close(void)
{
    xSemaphoreTake(s_link_mutex, portMAX_DELAY);
    if (!s_connected) {
        xSemaphoreGive(s_link_mutex);
        return;
    }
    s_connected = false;
    if (s_out_xfer) usb_host_transfer_free(s_out_xfer);
    if (s_in_xfer) usb_host_transfer_free(s_in_xfer);
    s_out_xfer = s_in_xfer = NULL;
    usb_host_interface_release(s_client, s_dev, s_intf_num);
    usb_host_device_close(s_client, s_dev);
    s_dev = NULL;
    xSemaphoreGive(s_link_mutex);

    s_led_state = LED_WAITING;
    ESP_LOGW(TAG, "link closed");
}

/* --- USB host plumbing ----------------------------------------------------*/
static void client_event_cb(const usb_host_client_event_msg_t *msg, void *arg)
{
    host_evt_t e;
    switch (msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        e.type = EVT_NEW_DEV;
        e.addr = msg->new_dev.address;
        xQueueSend(s_evt_q, &e, 0);
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        ESP_LOGW(TAG, "device disconnected");
        /* Teardown blocks, so it cannot run here — hand it to conn_task. */
        e.type = EVT_DEV_GONE;
        e.addr = 0;
        xQueueSend(s_evt_q, &e, 0);
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

static void conn_task(void *arg)
{
    host_evt_t e;
    while (true) {
        if (xQueueReceive(s_evt_q, &e, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (e.type) {
        case EVT_NEW_DEV:
            if (s_connected) {
                ESP_LOGW(TAG, "already linked — ignoring device %u", e.addr);
                break;
            }
            link_open(e.addr);
            break;
        case EVT_DEV_GONE:
            link_close();
            break;
        }
    }
}

esp_err_t hypex_host_start(hypex_host_status_cb_t cb)
{
    s_status_cb = cb;
    s_evt_q = xQueueCreate(4, sizeof(host_evt_t));
    s_out_track.done = xSemaphoreCreateBinary();
    s_in_track.done = xSemaphoreCreateBinary();
    s_ctrl_track.done = xSemaphoreCreateBinary();
    s_link_mutex = xSemaphoreCreateMutex();
    if (!s_evt_q || !s_out_track.done || !s_in_track.done ||
        !s_ctrl_track.done || !s_link_mutex) {
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
    xTaskCreate(conn_task, "usb_conn", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "USB host running — connect the FA503 to the USB-A host port");
    return ESP_OK;
}

#endif /* SOC_USB_OTG_SUPPORTED */
