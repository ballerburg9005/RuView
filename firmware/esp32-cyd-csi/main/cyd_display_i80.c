/* Standard ESP32-2432S022C ST7789 8-bit parallel i80 display.
 * GPIOs follow the user's uploaded pin drawing, NOT the TF-card SPI bus.
 * This alternate profile is selected explicitly in idf.py menuconfig.
 */
#include "cyd_display.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lcd_io_i80.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#define TFT_WIDTH 320
#define TFT_HEIGHT 240
#define STRIP_HEIGHT 16
#define PIN_BL GPIO_NUM_0
#define PIN_RD GPIO_NUM_2
#define BG_COLOR   0x101Au
#define TEXT_COLOR 0xE7B7u
#define DIM_COLOR  0x9CD3u
#define GOOD_COLOR 0x4F54u
#define WARN_COLOR 0xFDE0u
#define BAD_COLOR  0xF1C8u
#define TITLE_COLOR 0x5DFFu

typedef struct {
    char ch;
    uint8_t rows[7]; /* leftmost pixel = bit 4 */
} glyph_t;

/* Small independent block font for diagnostic text; no graphics dependencies. */
static const glyph_t s_glyphs[] = {
    {'A',{14,17,17,31,17,17,17}},
    {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}},
    {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}},
    {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,14}},
    {'H',{17,17,17,31,17,17,17}},
    {'I',{31,4,4,4,4,4,31}},
    {'J',{7,2,2,2,18,18,12}},
    {'K',{17,18,20,24,20,18,17}},
    {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}},
    {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}},
    {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}},
    {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}},
    {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}},
    {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,21,21,21,10}},
    {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}},
    {'Z',{31,1,2,4,8,16,31}},
    {'0',{14,17,19,21,25,17,14}},
    {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}},
    {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}},
    {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}},
    {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}},
    {'9',{14,17,17,15,1,1,14}},
    {':',{0,4,4,0,4,4,0}},
    {'.',{0,0,0,0,0,12,12}},
    {'-',{0,0,0,31,0,0,0}},
    {'/',{1,1,2,4,8,16,16}},
    {' ',{0,0,0,0,0,0,0}},
    {'?',{14,17,1,2,4,0,4}},
};


static const char *TAG = "cyd_i80";
static uint8_t *s_strip;
static bool s_ready;
static esp_lcd_i80_bus_handle_t s_bus;
static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_transfer_done;

static bool on_color_done(esp_lcd_panel_io_handle_t io,
                          esp_lcd_panel_io_event_data_t *event_data,
                          void *ctx)
{
    (void)io; (void)event_data;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

static void put_pixel(unsigned x, unsigned y, uint16_t rgb565)
{
    if (x >= TFT_WIDTH || y >= STRIP_HEIGHT) return;
    const size_t i = ((size_t)y * TFT_WIDTH + x) * 2u;
    s_strip[i] = (uint8_t)(rgb565 >> 8);     /* ILI9341 expects RGB565 MSB first */
    s_strip[i + 1] = (uint8_t)rgb565;
}

static const uint8_t *glyph(char c)
{
    for (size_t i = 0; i < sizeof(s_glyphs) / sizeof(s_glyphs[0]); i++) {
        if (s_glyphs[i].ch == c) return s_glyphs[i].rows;
    }
    return s_glyphs[sizeof(s_glyphs)/sizeof(s_glyphs[0])-1].rows;
}

static void draw_stripe_text(const char *message, uint16_t text_rgb565,
                             uint16_t background)
{
    for (size_t i = 0; i < TFT_WIDTH * STRIP_HEIGHT; i++) {
        s_strip[2 * i] = (uint8_t)(background >> 8);
        s_strip[2 * i + 1] = (uint8_t)background;
    }
    const unsigned x0 = 9;
    for (unsigned pos = 0; message[pos] && x0 + pos * 12 + 10 < TFT_WIDTH; pos++) {
        const uint8_t *rows = glyph(message[pos]);
        for (unsigned ry = 0; ry < 7; ry++) {
            for (unsigned rx = 0; rx < 5; rx++) {
                if ((rows[ry] >> (4u-rx)) & 1u) {
                    unsigned x = x0 + pos * 12 + rx * 2;
                    unsigned y = 1 + ry * 2;
                    put_pixel(x, y, text_rgb565);
                    put_pixel(x+1, y, text_rgb565);
                    put_pixel(x, y+1, text_rgb565);
                    put_pixel(x+1, y+1, text_rgb565);
                }
            }
        }
    }
}

/* A 16px stripe is sent in five DMA transactions; each <= 2048 bytes.
 * This avoids allocating a 150KiB full-screen buffer on the no-PSRAM ESP32. */

static esp_err_t flush_stripe(unsigned top)
{
    if (top + STRIP_HEIGHT > TFT_HEIGHT) return ESP_ERR_INVALID_ARG;
    esp_err_t e = esp_lcd_panel_draw_bitmap(s_panel, 0, top,
                                            TFT_WIDTH, top + STRIP_HEIGHT,
                                            s_strip);
    if (e != ESP_OK) return e;
    if (xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void paint_row(unsigned index, const char *text, uint16_t color)
{
    const unsigned y = 8 + index * 17u;
    if (y + STRIP_HEIGHT > TFT_HEIGHT) return;
    draw_stripe_text(text, color, BG_COLOR);
    const esp_err_t err = flush_stripe(y);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I80 paint failed: %s", esp_err_to_name(err));
        s_ready = false;
    }
}


esp_err_t cyd_display_init(void)
{
    if (s_ready) return ESP_OK;
    /* GPIO0 is a boot strap: configure backlight only AFTER boot. */
    gpio_config_t g = {
        .pin_bit_mask = (1ULL << PIN_BL) | (1ULL << PIN_RD),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t e = gpio_config(&g);
    if (e != ESP_OK) return e;
    gpio_set_level(PIN_BL, 0);
    gpio_set_level(PIN_RD, 1); /* LCD RD pin inactive */

    s_transfer_done = xSemaphoreCreateBinary();
    if (!s_transfer_done) return ESP_ERR_NO_MEM;

    const esp_lcd_i80_bus_config_t bus = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .dc_gpio_num = GPIO_NUM_16,
        .wr_gpio_num = GPIO_NUM_4,
        /* DB8..DB15 = i80 D0..D7, from annotated S022C pin diagram */
        .data_gpio_nums = {15, 13, 12, 14, 27, 25, 33, 32},
        .bus_width = 8,
        .max_transfer_bytes = TFT_WIDTH * STRIP_HEIGHT * 2,
    };
    e = esp_lcd_new_i80_bus(&bus, &s_bus);
    if (e != ESP_OK) return e;

    const esp_lcd_panel_io_i80_config_t io = {
        .cs_gpio_num = GPIO_NUM_17,
        .pclk_hz = 10000000,  /* conservative parallel write clock */
        .trans_queue_depth = 2,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_levels = {
            .dc_idle_level = 0,
            .dc_cmd_level = 0,
            .dc_dummy_level = 0,
            .dc_data_level = 1,
        },
    };
    e = esp_lcd_new_panel_io_i80(s_bus, &io, &s_io);
    if (e != ESP_OK) return e;
    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_done,
    };
    e = esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, s_transfer_done);
    if (e != ESP_OK) return e;

    const esp_lcd_panel_dev_config_t panel = {
        .reset_gpio_num = -1,  /* LCD reset wired to ESP32 EN */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    e = esp_lcd_new_panel_st7789(s_io, &panel, &s_panel);
    if (e != ESP_OK) return e;
    e = esp_lcd_panel_reset(s_panel);
    if (e != ESP_OK) return e;
    e = esp_lcd_panel_init(s_panel);
    if (e != ESP_OK) return e;
    /* 320x240 landscape geometry on native 240x320 GRAM */
    e = esp_lcd_panel_swap_xy(s_panel, true);
    if (e != ESP_OK) return e;
    e = esp_lcd_panel_mirror(s_panel, true, false);
    if (e != ESP_OK) return e;
    e = esp_lcd_panel_invert_color(s_panel, true);
    if (e != ESP_OK) return e;
    e = esp_lcd_panel_disp_on_off(s_panel, true);
    if (e != ESP_OK) return e;

    s_strip = heap_caps_malloc(TFT_WIDTH * STRIP_HEIGHT * 2u,
                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_strip) return ESP_ERR_NO_MEM;
    draw_stripe_text("", TEXT_COLOR, BG_COLOR);
    for (unsigned y=0; y<TFT_HEIGHT; y+=STRIP_HEIGHT) {
        e = flush_stripe(y);
        if (e != ESP_OK) return e;
    }
    gpio_set_level(PIN_BL, 1);
    s_ready = true;
    ESP_LOGI(TAG,"S022C parallel ST7789 status display initialized");
    return ESP_OK;
}
void cyd_display_render(const cyd_display_status_t *s)
{
    if (!s_ready || s == NULL) return;

    char text[40];
    paint_row(0,  "RUVIEW   CSI NODE", TITLE_COLOR);
    paint_row(1,  "ESP32 S022C I80", DIM_COLOR);
    paint_row(2,  s->wifi_connected ? "WIFI: CONNECTED" : "WIFI: DISCONNECTED",
              s->wifi_connected ? GOOD_COLOR : BAD_COLOR);

    snprintf(text, sizeof(text), "IP: %s", s->wifi_connected ? s->ip : "NONE");
    paint_row(3, text, TEXT_COLOR);
    snprintf(text, sizeof(text), "AP CH:%u  RSSI:%d", s->channel, s->rssi_dbm);
    paint_row(4, text, DIM_COLOR);

    snprintf(text, sizeof(text), "CSI CALLBACK: %lu/S",
             (unsigned long)s->callbacks_per_s);
    paint_row(5, text, s->callbacks_per_s ? GOOD_COLOR : WARN_COLOR);
    snprintf(text, sizeof(text), "CSI QUEUED: %lu/S",
             (unsigned long)s->accepted_per_s);
    paint_row(6, text, TEXT_COLOR);
    snprintf(text, sizeof(text), "UDP SENT: %lu/S",
             (unsigned long)s->sent_per_s);
    paint_row(7, text, s->sent_per_s ? GOOD_COLOR : WARN_COLOR);
    snprintf(text, sizeof(text), "PROBES: %lu/S",
             (unsigned long)s->probes_per_s);
    paint_row(8, text, DIM_COLOR);
    snprintf(text, sizeof(text), "DROPPED: %lu TOTAL",
             (unsigned long)s->drops_total);
    paint_row(9, text, s->drops_total ? WARN_COLOR : GOOD_COLOR);

    paint_row(10, "RAW WIFI CSI ONLY", DIM_COLOR);
    paint_row(11, "NO HEART RATE ESTIMATE", WARN_COLOR);
    paint_row(12, "NOT FOR MEDICAL USE", WARN_COLOR);
}

