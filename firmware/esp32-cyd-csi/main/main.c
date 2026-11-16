/*
 * RuView original-ESP32 CSI receiver for ESP32-2432S022 (CYD).
 * Independent of RuView's ESP32-S3/C6 firmware. S022C parallel LCD by default.
 * ESP-IDF v5.4.x, target esp32, 4 MB flash, no PSRAM.
 * Outputs ADR-018-compatible raw CSI datagrams (magic 0xC5110001).
 * Not a medical device: no heartbeat / breathing / falls inference.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"
#include "cyd_display.h"

#if !CONFIG_IDF_TARGET_ESP32
#error "This lightweight CYD firmware is for the original ESP32, not S3/C3/C6."
#endif
#ifndef CONFIG_ESP_WIFI_CSI_ENABLED
#error "Wi-Fi CSI must be enabled in sdkconfig."
#endif

#define CSI_MAGIC       0xC5110001u
#define CSI_HEADER_SIZE 20u
#define CSI_MAX_BYTES   640u
#define CSI_QUEUE_DEPTH 12
#define WIFI_READY_BIT  BIT0

typedef struct {
    uint16_t len;
    uint8_t bytes[CSI_HEADER_SIZE + CSI_MAX_BYTES];
} csi_packet_t;

static const char *TAG = "cyd_csi";
static QueueHandle_t s_queue;
static EventGroupHandle_t s_wifi_events;
static esp_timer_handle_t s_reconnect_timer;
static uint8_t s_ap_bssid[6];
static volatile bool s_have_bssid = false;
static int64_t s_last_callback_us = 0;
static uint32_t s_sequence = 0;
static volatile uint32_t s_callbacks = 0;
static volatile uint32_t s_filtered = 0;
static volatile uint32_t s_accepted = 0;
static volatile uint32_t s_dropped = 0;
static volatile uint32_t s_sent = 0;
static volatile uint32_t s_probes = 0;

static void csi_callback(void *ctx, wifi_csi_info_t *info)
{
    (void)ctx;
    ++s_callbacks;
    if (info == NULL || info->buf == NULL || info->len < 2 ||
        info->len > CSI_MAX_BYTES || (info->len & 1u) != 0u) {
        ++s_filtered;
        return;
    }

    /* Stick to the currently associated AP rather than mixing radio paths.
     * The AP may forward traffic from any existing wired LAN computer. */
    if (!s_have_bssid || memcmp(info->mac, s_ap_bssid, 6) != 0) {
        ++s_filtered;
        return;
    }
    if ((xEventGroupGetBits(s_wifi_events) & WIFI_READY_BIT) == 0) {
        ++s_filtered;
        return;
    }

    const int64_t now = esp_timer_get_time();
    const int64_t interval = 1000000LL / CONFIG_CYD_CSI_MAX_RATE;
    if (now - s_last_callback_us < interval) {
        ++s_filtered;
        return;
    }
    s_last_callback_us = now;

    csi_packet_t frame = {0};
    const uint16_t iq_len = info->len;
    const uint16_t carriers = iq_len / 2u;
    const uint8_t channel = info->rx_ctrl.channel;
    const uint32_t mhz =
        (channel >= 1 && channel <= 13) ? 2412u + 5u * (channel - 1u) :
        (channel == 14) ? 2484u : 0u;
    if (mhz == 0) {
        ++s_filtered;
        return;
    }

    frame.len = CSI_HEADER_SIZE + iq_len;
    const uint32_t magic = CSI_MAGIC;
    const uint32_t seq = s_sequence++;
    memcpy(frame.bytes + 0, &magic, sizeof(magic));
    frame.bytes[4] = (uint8_t)CONFIG_CYD_CSI_NODE_ID;
    frame.bytes[5] = 1; /* single receiving antenna */
    memcpy(frame.bytes + 6, &carriers, sizeof(carriers));
    memcpy(frame.bytes + 8, &mhz, sizeof(mhz));
    memcpy(frame.bytes + 12, &seq, sizeof(seq));
    frame.bytes[16] = (uint8_t)info->rx_ctrl.rssi;
    frame.bytes[17] = (uint8_t)info->rx_ctrl.noise_floor;
    frame.bytes[18] = 0; /* legacy/HT packet class (ADR-110) */
    frame.bytes[19] =
        (info->rx_ctrl.cwb ? 0x01 : 0x00) |
        (info->rx_ctrl.stbc ? 0x04 : 0x00);
    memcpy(frame.bytes + CSI_HEADER_SIZE, info->buf, iq_len);
    if (info->first_word_invalid) {
        memset(frame.bytes + CSI_HEADER_SIZE, 0, iq_len < 4 ? iq_len : 4);
        frame.bytes[19] |= 0x20; /* ADR-018 first-word sanitized */
    }

    /* Wi-Fi task callback: do not send UDP or log here. */
    if (xQueueSend(s_queue, &frame, 0) == pdTRUE) {
        ++s_accepted;
    } else {
        ++s_dropped;
    }
}

static void reconnect_callback(void *arg)
{
    (void)arg;
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect request: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        const wifi_event_sta_connected_t *e =
            (const wifi_event_sta_connected_t *)data;
        /* Reconnect path: mark unset before copying new BSSID. */
        s_have_bssid = false;
        memcpy(s_ap_bssid, e->bssid, sizeof(s_ap_bssid));
        s_have_bssid = true;
        ESP_LOGI(TAG, "Associated with AP on channel %u", e->channel);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *e =
            (const wifi_event_sta_disconnected_t *)data;
        xEventGroupClearBits(s_wifi_events, WIFI_READY_BIT);
        s_have_bssid = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected (reason %u); retrying in 3 s",
                 e->reason);
        (void)esp_timer_stop(s_reconnect_timer);
        ESP_ERROR_CHECK(esp_timer_start_once(s_reconnect_timer, 3000000));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Node IP: " IPSTR " (probe UDP %d; collector UDP %d)",
                 IP2STR(&e->ip_info.ip),
                 CONFIG_CYD_CSI_PROBE_PORT, CONFIG_CYD_CSI_SERVER_PORT);
        xEventGroupSetBits(s_wifi_events, WIFI_READY_BIT);
    }
}

static void wifi_start(void)
{
    if (strlen(CONFIG_CYD_CSI_WIFI_SSID) == 0 ||
        strlen(CONFIG_CYD_CSI_WIFI_SSID) > 32 ||
        strlen(CONFIG_CYD_CSI_WIFI_PASSWORD) > 63) {
        ESP_LOGE(TAG, "Set Wi-Fi credentials with idf.py menuconfig; "
                 "TFT status remains available without association");
        return;
    }

    /* The TCP/IP stack and default event loop are initialized in app_main()
     * before any FreeRTOS tasks can invoke lwIP sockets. */
    esp_netif_create_default_wifi_sta();
    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    esp_timer_create_args_t timer_args = {
        .callback = reconnect_callback, .name = "reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, CONFIG_CYD_CSI_WIFI_SSID,
           strlen(CONFIG_CYD_CSI_WIFI_SSID));
    memcpy(cfg.sta.password, CONFIG_CYD_CSI_WIFI_PASSWORD,
           strlen(CONFIG_CYD_CSI_WIFI_PASSWORD));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Both 11g and 11n OFDM/LTF data are useful. Only frames actually
     * received on the association channel appear in the CSI callback. */
    wifi_csi_config_t csi = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = false,
        .manu_scale = false,
        .shift = 0,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_callback, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
}

static void sender_task(void *arg)
{
    (void)arg;
    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(CONFIG_CYD_CSI_SERVER_PORT);
    if (inet_pton(AF_INET, CONFIG_CYD_CSI_SERVER_IP,
                  &dest.sin_addr) != 1) {
        ESP_LOGE(TAG, "Invalid collector IPv4: %s", CONFIG_CYD_CSI_SERVER_IP);
        vTaskDelete(NULL);
        return;
    }
    int sock = -1;
    csi_packet_t frame;
    while (true) {
        if ((xEventGroupGetBits(s_wifi_events) & WIFI_READY_BIT) == 0) {
            if (sock >= 0) { close(sock); sock = -1; }
            xEventGroupWaitBits(s_wifi_events, WIFI_READY_BIT,
                                pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (sock < 0) {
                ESP_LOGW(TAG, "CSI sender socket failed; retrying");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (xQueueReceive(s_queue, &frame, pdMS_TO_TICKS(200)) != pdTRUE)
            continue;
        if (sendto(sock, frame.bytes, frame.len, 0,
                   (struct sockaddr *)&dest, sizeof(dest)) == frame.len) {
            ++s_sent;
        } else {
            ++s_dropped;
            close(sock);
            sock = -1;
        }
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "Failed to create UDP probe sink");
        vTaskDelete(NULL);
        return;
    }
    const struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_CYD_CSI_PROBE_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (const struct sockaddr *)&listen_addr,
             sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind UDP probe port %d",
                 CONFIG_CYD_CSI_PROBE_PORT);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    char buf[128];
    while (true) {
        if (recvfrom(sock, buf, sizeof(buf), 0, NULL, NULL) > 0)
            ++s_probes;
    }
}

static void status_task(void *arg)
{
    (void)arg;
    uint32_t old_cb = 0, old_acc = 0, old_sent = 0, old_probe = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        const uint32_t cb = s_callbacks, acc = s_accepted, sent = s_sent,
                       probe = s_probes;
        const uint32_t cb_hz = (cb - old_cb) / 2,
                       acc_hz = (acc - old_acc) / 2,
                       sent_hz = (sent - old_sent) / 2,
                       probe_hz = (probe - old_probe) / 2;
        const bool connected =
            (xEventGroupGetBits(s_wifi_events) & WIFI_READY_BIT) != 0;
        cyd_display_status_t display = {
            .wifi_connected = connected,
            .rssi_dbm = 0,
            .channel = 0,
            .callbacks_per_s = cb_hz,
            .accepted_per_s = acc_hz,
            .sent_per_s = sent_hz,
            .probes_per_s = probe_hz,
            .drops_total = s_dropped,
        };
        if (connected) {
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                display.channel = ap.primary;
                display.rssi_dbm = ap.rssi;
            }
            esp_netif_t *iface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            esp_netif_ip_info_t ip_info;
            if (iface != NULL &&
                esp_netif_get_ip_info(iface, &ip_info) == ESP_OK) {
                snprintf(display.ip, sizeof(display.ip), IPSTR,
                         IP2STR(&ip_info.ip));
            }
        }
        ESP_LOGI(TAG, "2s: CSI callback=%lu queued=%lu UDP sent=%lu probes=%lu "
                 "filtered=%lu dropped=%lu queue=%u",
                 (unsigned long)(cb - old_cb),
                 (unsigned long)(acc - old_acc),
                 (unsigned long)(sent - old_sent),
                 (unsigned long)(probe - old_probe),
                 (unsigned long)s_filtered,
                 (unsigned long)s_dropped,
                 (unsigned)uxQueueMessagesWaiting(s_queue));
        cyd_display_render(&display);
        old_cb = cb;
        old_acc = acc;
        old_sent = sent;
        old_probe = probe;
    }
}

void app_main(void)
{
#if CONFIG_CYD_LCD_I80
    ESP_LOGI(TAG, "RuView CSI node %d: ESP32-2432S022C ST7789 i80 LCD",
             CONFIG_CYD_CSI_NODE_ID);
#else
    ESP_LOGI(TAG, "RuView CSI node %d: ESP32-2432S028 ILI9341 SPI LCD",
             CONFIG_CYD_CSI_NODE_ID);
#endif
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* Order is essential: probe_task starts with socket(AF_INET, ...).
     * Creating it before esp_netif_init() leaves lwIP's TCP/IP mailbox
     * uninitialized and triggers tcpip_send_msg_wait_sem: Invalid mbox.
     * Initialize the network core even for screen-only builds without SSID. */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_queue = xQueueCreate(CSI_QUEUE_DEPTH, sizeof(csi_packet_t));
    s_wifi_events = xEventGroupCreate();
    if (s_queue == NULL || s_wifi_events == NULL) {
        ESP_LOGE(TAG, "Not enough internal RAM for CSI queue");
        abort();
    }
    ESP_ERROR_CHECK(xTaskCreate(sender_task, "csi_udp", 4096, NULL, 5, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(probe_task, "probe", 3072, NULL, 3, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(xTaskCreate(status_task, "stats", 3072, NULL, 2, NULL)
                    == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    /* Bring the TFT up first, so even a blank Wi-Fi configuration shows
     * connection diagnostics rather than silently rebooting with a black
     * screen. No screen pin is used by the CSI radio path. */
    esp_err_t display_err = cyd_display_init();
    if (display_err == ESP_OK) {
        const cyd_display_status_t initial = {.wifi_connected = false};
        cyd_display_render(&initial);
    } else {
        ESP_LOGW(TAG, "CYD TFT unavailable: %s (CSI still running)",
                 esp_err_to_name(display_err));
    }
    wifi_start();
}
