#include "ui_display.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"

#include "font8x8_basic.h"

static const char *TAG = "ui_display";

/* --- ESP32-S3-USB-OTG LCD pin map (esp-bsp esp32_s3_usb_otg) --------------*/
#define LCD_SPI_HOST SPI3_HOST
#define PIN_MOSI 7
#define PIN_CLK 6
#define PIN_CS 5
#define PIN_DC 4
#define PIN_RST 8
#define PIN_BL 9
#define LCD_W 240
#define LCD_H 240
#define LCD_PCLK_HZ (40 * 1000 * 1000)

/* The whole screen is drawn into one full-frame buffer, then flushed to the
 * panel in FLUSH_ROWS-high slices. Drawing to a single contiguous buffer
 * (rather than per-band) means tall glyphs never fall through a seam; the
 * slicing is only to keep each SPI transfer at the vendor's ~14 KB size,
 * which avoids the oversized-transfer duplication bug. */
#define FLUSH_ROWS 30
#define FB_PX (LCD_W * LCD_H)

#define VOL_MIN_X100 (-9900)
#define VOL_MAX_X100 (0)

static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb; /* full frame, LCD_W * LCD_H */

/* 16-bit colour, byte-swapped so the ST7789 sees the correct order over SPI. */
static inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (uint16_t)((v >> 8) | (v << 8));
}

#define COL_BG rgb(8, 10, 14)
#define COL_FG rgb(235, 238, 245)
#define COL_DIM rgb(120, 128, 140)
#define COL_ACCENT rgb(60, 190, 255)
#define COL_GREEN rgb(60, 210, 120)
#define COL_RED rgb(255, 70, 70)
#define COL_BARBG rgb(30, 36, 44)

/* --- drawing primitives (write straight into the full frame) --------------*/
static inline void px(int x, int y, uint16_t c)
{
    if (x < 0 || x >= LCD_W || y < 0 || y >= LCD_H) return;
    s_fb[y * LCD_W + x] = c;
}

static void fill_rect(int x, int y, int w, int h, uint16_t c)
{
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++) px(xx, yy, c);
}

static void clear(uint16_t c)
{
    for (int i = 0; i < FB_PX; i++) s_fb[i] = c;
}

/* One 8x8 glyph scaled by `s`. */
static void draw_char(int x, int y, char ch, int s, uint16_t c)
{
    if ((unsigned char)ch >= 128) ch = '?';
    const unsigned char *g = (const unsigned char *)font8x8_basic[(int)ch];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (g[row] & (1 << col)) {
                fill_rect(x + col * s, y + row * s, s, s, c);
            }
        }
    }
}

static void draw_text(int x, int y, const char *str, int s, uint16_t c)
{
    for (; *str; str++, x += 8 * s) draw_char(x, y, *str, s, c);
}

static int text_w(const char *str, int s) { return (int)strlen(str) * 8 * s; }

static void draw_text_centered(int cx, int y, const char *str, int s,
                               uint16_t c)
{
    draw_text(cx - text_w(str, s) / 2, y, str, s, c);
}

/* --- flush ----------------------------------------------------------------*/
static void flush(void)
{
    for (int y = 0; y < LCD_H; y += FLUSH_ROWS) {
        int h = (y + FLUSH_ROWS <= LCD_H) ? FLUSH_ROWS : (LCD_H - y);
        esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + h,
                                  &s_fb[y * LCD_W]);
    }
}

/* --- panel bring-up -------------------------------------------------------*/
#define CK(x)                                                          \
    do {                                                               \
        esp_err_t e_ = (x);                                            \
        if (e_ != ESP_OK) {                                            \
            ESP_LOGE(TAG, "%s -> %s", #x, esp_err_to_name(e_));        \
            return e_;                                                 \
        }                                                              \
    } while (0)

esp_err_t ui_display_init(void)
{
    s_fb = heap_caps_malloc(FB_PX * sizeof(uint16_t),
                            MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if (!s_fb) {
        s_fb = heap_caps_malloc(FB_PX * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    }
    if (!s_fb) {
        ESP_LOGE(TAG, "no memory for %d-byte frame buffer",
                 (int)(FB_PX * sizeof(uint16_t)));
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = PIN_CLK,
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FLUSH_ROWS * LCD_W * sizeof(uint16_t) + 8,
    };
    CK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = PIN_DC,
        .cs_gpio_num = PIN_CS,
        .pclk_hz = LCD_PCLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    CK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                &io_config, &io));

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    CK(esp_lcd_new_panel_st7789(io, &panel_config, &s_panel));
    CK(esp_lcd_panel_reset(s_panel));
    CK(esp_lcd_panel_init(s_panel));
    CK(esp_lcd_panel_invert_color(s_panel, true));
    CK(esp_lcd_panel_disp_on_off(s_panel, true));

    gpio_config_t bl = {.mode = GPIO_MODE_OUTPUT,
                        .pin_bit_mask = 1ULL << PIN_BL};
    gpio_config(&bl);
    gpio_set_level(PIN_BL, 1);

    clear(COL_BG);
    draw_text_centered(LCD_W / 2, 96, "KingRO4Y", 3, COL_ACCENT);
    draw_text_centered(LCD_W / 2, 128, "dongle", 2, COL_DIM);
    flush();
    ESP_LOGI(TAG, "LCD up (ST7789 240x240, full-frame)");
    return ESP_OK;
}

/* --- UI layout ------------------------------------------------------------*/
static const char *input_name(uint8_t v)
{
    switch (v) {
    case 0x00: return "SCAN";
    case 0x01: return "XLR";
    case 0x02: return "RCA";
    case 0x04: return "SPDIF";
    case 0x05: return "AES";
    case 0x06: return "OPT";
    default: return "?";
    }
}

static void draw_ui(const dongle_ui_state_t *s)
{
    /* Top: knob name + connection dot. */
    draw_text(12, 14, "VOL20", 2, s->ble_connected ? COL_FG : COL_DIM);
    fill_rect(210, 14, 16, 16, s->ble_connected ? COL_GREEN : COL_DIM);

    /* Centre: big volume (or MUTE). Integer formatting — ESP-IDF's default
     * nano newlib drops %f, which was blanking the number. */
    if (s->mute) {
        draw_text_centered(LCD_W / 2, 78, "MUTE", 5, COL_RED);
    } else {
        int v100 = s->volume_db_x100;
        int neg = v100 < 0;
        int a = neg ? -v100 : v100;
        char v[12];
        snprintf(v, sizeof(v), "%s%d.%d", neg ? "-" : "", a / 100,
                 (a % 100) / 10);
        draw_text_centered(LCD_W / 2, 74, v, 4, COL_FG);
        draw_text_centered(LCD_W / 2, 116, "dB", 2, COL_DIM);
    }

    /* Volume bar. */
    int bx = 18, by = 152, bw = LCD_W - 36, bh = 22;
    fill_rect(bx, by, bw, bh, COL_BARBG);
    int span = VOL_MAX_X100 - VOL_MIN_X100; /* 9900 */
    int filled = (int)((long)bw * (s->volume_db_x100 - VOL_MIN_X100) / span);
    if (filled < 0) filled = 0;
    if (filled > bw) filled = bw;
    fill_rect(bx, by, filled, bh, s->mute ? COL_RED : COL_ACCENT);

    /* Bottom: preset + input. */
    char line[24];
    snprintf(line, sizeof(line), "P%u    %s", s->preset,
             input_name(s->input_source));
    draw_text_centered(LCD_W / 2, 200, line, 2, COL_DIM);
}

void ui_display_render(const dongle_ui_state_t *s)
{
    if (!s_panel) return;
    clear(COL_BG);
    draw_ui(s);
    flush();
}
