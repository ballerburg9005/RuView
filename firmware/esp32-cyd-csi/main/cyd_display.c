/*
 * Lightweight status screen for the 2.8-inch SPI CYD ESP32-2432S028.
 * Pins verified against the board's SPI pinout:
 * MOSI 13, SCLK 14, CS 15, DC 2, backlight 21 (active high).
 * ILI9341 default. No touchscreen pins used, no framebuffer/PSRAM required.
 * 320x240 landscape; only ~10 KiB DMA memory and small built-in 5x7 glyphs.
 */
#include "cyd_display.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define TFT_WIDTH 320
#define TFT_HEIGHT 240
#define STRIP_HEIGHT 16
#define PIN_MOSI GPIO_NUM_13
#define PIN_SCLK GPIO_NUM_14
#define PIN_CS GPIO_NUM_15
#define PIN_DC GPIO_NUM_2
#define PIN_BL GPIO_NUM_21
#define TFT_HOST SPI2_HOST

/* RGB565 colors */
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

static const char *TAG = "cyd_display";
static spi_device_handle_t s_spi;
static uint8_t *s_strip;
static bool s_ready;

static esp_err_t tx_bytes(const uint8_t *bytes, size_t length)
{
    if (!length) return ESP_OK;
    if (length <= 4) {
        spi_transaction_t trans = {
            .length = length * 8u,
            .flags = SPI_TRANS_USE_TXDATA,
        };
        memcpy(trans.tx_data, bytes, length);
        return spi_device_polling_transmit(s_spi, &trans);
    }
    spi_transaction_t trans = {
        .length = length * 8u,
        .tx_buffer = bytes,
    };
    return spi_device_polling_transmit(s_spi, &trans);
}

static esp_err_t write_cmd(uint8_t cmd, const uint8_t *params, size_t count)
{
    gpio_set_level(PIN_DC, 0);
    esp_err_t err = tx_bytes(&cmd, 1);
    if (err != ESP_OK) return err;
    if (count) {
        gpio_set_level(PIN_DC, 1);
        err = tx_bytes(params, count);
    }
    return err;
}

/* Send one DMA-backed region, with byte-packed RGB565. */
static esp_err_t tx_pixels(const uint8_t *pixels, size_t bytes)
{
    gpio_set_level(PIN_DC, 1);
    return tx_bytes(pixels, bytes);
}

static esp_err_t set_window(unsigned x0, unsigned y0,
                            unsigned x1, unsigned y1)
{
    const uint8_t x[] = { (uint8_t)(x0 >> 8), (uint8_t)x0,
                          (uint8_t)(x1 >> 8), (uint8_t)x1 };
    const uint8_t y[] = { (uint8_t)(y0 >> 8), (uint8_t)y0,
                          (uint8_t)(y1 >> 8), (uint8_t)y1 };
    esp_err_t err = write_cmd(0x2A, x, sizeof(x));
    if (err == ESP_OK) err = write_cmd(0x2B, y, sizeof(y));
    if (err == ESP_OK) err = write_cmd(0x2C, NULL, 0);
    return err;
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
    esp_err_t err = set_window(0, top, TFT_WIDTH - 1, top + STRIP_HEIGHT - 1);
    if (err != ESP_OK) return err;
    const size_t chunk = 2048u; /* aligned to whole RGB565 pixels */
    const size_t bytes = TFT_WIDTH * STRIP_HEIGHT * 2u;
    for (size_t off = 0; off < bytes; off += chunk) {
        size_t n = bytes - off;
        if (n > chunk) n = chunk;
        err = tx_pixels(s_strip + off, n);
        if (err != ESP_OK) return err;
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
        ESP_LOGW(TAG, "SPI paint failed: %s", esp_err_to_name(err));
        s_ready = false;
    }
}

esp_err_t cyd_display_init(void)
{
    if (s_ready) return ESP_OK;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PIN_DC) | (1ULL << PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    gpio_set_level(PIN_BL, 0);
    gpio_set_level(PIN_DC, 0);

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = -1,   /* writing only; GPIO12 is a boot strap pin */
        .sclk_io_num = PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_WIDTH * STRIP_HEIGHT * 2 + 8,
    };
    err = spi_bus_initialize(TFT_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) return err;
    const spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000, /* conservative for CYD clones */
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 1,
    };
    err = spi_bus_add_device(TFT_HOST, &dev, &s_spi);
    if (err != ESP_OK) return err;

    s_strip = heap_caps_malloc(TFT_WIDTH * STRIP_HEIGHT * 2,
                              MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_strip == NULL) return ESP_ERR_NO_MEM;

    err = write_cmd(0x01, NULL, 0);   /* SWRESET */
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));
    err = write_cmd(0x11, NULL, 0);   /* SLPOUT */
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(150));
    const uint8_t pixel_fmt = 0x55;  /* RGB565 */
    const uint8_t orientation = 0x28; /* landscape + BGR */
    err = write_cmd(0x3A, &pixel_fmt, 1);
    if (err == ESP_OK) err = write_cmd(0x36, &orientation, 1);
    if (err == ESP_OK) err = write_cmd(0x29, NULL, 0);  /* DISPON */
    if (err != ESP_OK) return err;

    /* Initialize entire screen to a solid dark background. */
    draw_stripe_text("", TEXT_COLOR, BG_COLOR);
    for (unsigned top = 0; top < TFT_HEIGHT; top += STRIP_HEIGHT) {
        err = flush_stripe(top);
        if (err != ESP_OK) return err;
    }
    gpio_set_level(PIN_BL, 1);
    s_ready = true;
    ESP_LOGI(TAG, "CYD display ready: 320x240 ILI9341 SPI2, 20MHz");
    return ESP_OK;
}

void cyd_display_render(const cyd_display_status_t *s)
{
    if (!s_ready || s == NULL) return;

    char text[40];
    paint_row(0,  "RUVIEW   CSI NODE", TITLE_COLOR);
    paint_row(1,  "ESP32-2432S028", DIM_COLOR);
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
