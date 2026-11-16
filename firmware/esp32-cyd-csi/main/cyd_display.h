/* CYD 2.8" SPI TFT status UI. Hardware: ESP32-2432S028 (ILI9341). */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    bool wifi_connected;
    char ip[16];
    int rssi_dbm;
    unsigned channel;
    uint32_t callbacks_per_s;
    uint32_t accepted_per_s;
    uint32_t sent_per_s;
    uint32_t probes_per_s;
    uint32_t drops_total;
} cyd_display_status_t;

/* Initializes TFT, returning an error without stopping CSI if it fails. */
esp_err_t cyd_display_init(void);

/* Call periodically from normal FreeRTOS task context; never from Wi-Fi callback. */
void cyd_display_render(const cyd_display_status_t *status);
