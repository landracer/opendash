/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_espnow.c
 * @brief OpenDash ESP-NOW Wireless Transport Layer — Implementation
 *
 * Provides WiFi (STA, no connection) + ESP-NOW initialization, send/receive
 * wrappers, and peer management for inter-node wireless communication.
 *
 * The receive callback runs in WiFi task context and must be fast. It copies
 * incoming data into a FreeRTOS queue for deferred processing by the
 * application task.
 *
 * @see opendash_espnow.h for the public API.
 */

#include "opendash_espnow.h"

#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include <string.h>

static const char *TAG = "espnow";

/* ────────────────────────────────────────────────────────────────────────────
 * Module State
 * ──────────────────────────────────────────────────────────────────────────── */

const uint8_t OPENDASH_ESPNOW_BROADCAST[6] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static QueueHandle_t s_recv_queue = NULL;
static uint8_t       s_self_mac[6] = {0};
static bool          s_initialized = false;

/* ────────────────────────────────────────────────────────────────────────────
 * ESP-NOW Callbacks (run in WiFi task context — keep fast!)
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Receive callback — copies message into the application queue.
 */
static void espnow_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len)
{
    if (!s_recv_queue || !info || !data || len <= 0) return;

    opendash_espnow_event_t evt;
    memcpy(evt.src_mac, info->src_addr, 6);

    int copy = (len <= OPENDASH_ESPNOW_MAX_DATA) ? len : OPENDASH_ESPNOW_MAX_DATA;
    memcpy(evt.data, data, copy);
    evt.len = copy;
    evt.rssi = (info->rx_ctrl) ? info->rx_ctrl->rssi : 0;

    /* Non-blocking enqueue from WiFi context.
     * If the queue is full, this message is silently dropped.
     * The application should drain the queue fast enough.          */
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    xQueueSendFromISR(s_recv_queue, &evt, &xHigherPriorityTaskWoken);
    if (xHigherPriorityTaskWoken) {
        portYIELD_FROM_ISR();
    }
}

/**
 * @brief Send callback — logs delivery failures at debug level.
 *
 * ESP-IDF v6.x changed the send callback signature to use
 * esp_now_send_info_t instead of raw MAC address.
 */
static void espnow_send_cb(const esp_now_send_info_t *info,
                             esp_now_send_status_t status)
{
    if (status != ESP_NOW_SEND_SUCCESS && info) {
        ESP_LOGD(TAG, "Send to " MACSTR " failed",
                 MAC2STR(info->des_addr));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Human-readable node name (local helper)
 * ──────────────────────────────────────────────────────────────────────────── */

static const char *node_label(opendash_node_t node)
{
    switch (node) {
        case OPENDASH_NODE_CENTER: return "CENTER";
        case OPENDASH_NODE_LEFT:   return "LEFT";
        case OPENDASH_NODE_RIGHT:  return "RIGHT";
        case OPENDASH_NODE_GPS:    return "GPS";
        case OPENDASH_NODE_BMS:    return "BMS";
        default:                   return "UNKNOWN";
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API — Initialization
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t opendash_espnow_init(opendash_node_t self_node)
{
    if (s_initialized) return ESP_OK;

    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");
    ESP_LOGI(TAG, "Initializing ESP-NOW transport for node %s",
             node_label(self_node));
    ESP_LOGI(TAG, "══════════════════════════════════════════════════════");

    /* ── Create receive queue ──────────────────────────────────────── */
    s_recv_queue = xQueueCreate(OPENDASH_ESPNOW_QUEUE_SIZE,
                                 sizeof(opendash_espnow_event_t));
    if (!s_recv_queue) {
        ESP_LOGE(TAG, "Failed to create receive queue");
        return ESP_ERR_NO_MEM;
    }

    /* ── Initialize WiFi in STA mode (no AP connection) ────────────── */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Lock all nodes to the same WiFi channel for ESP-NOW */
    ESP_ERROR_CHECK(esp_wifi_set_channel(OPENDASH_ESPNOW_CHANNEL,
                                          WIFI_SECOND_CHAN_NONE));

    /* Reduce TX power for short-range dashboard use (~2 dBm).
     * Saves power, reduces interference with other 2.4GHz devices.  */
    esp_wifi_set_max_tx_power(8);

    /* Get our MAC address */
    esp_wifi_get_mac(WIFI_IF_STA, s_self_mac);
    ESP_LOGI(TAG, "  Node: %s", node_label(self_node));
    ESP_LOGI(TAG, "  MAC:  " MACSTR, MAC2STR(s_self_mac));
    ESP_LOGI(TAG, "  Channel: %d", OPENDASH_ESPNOW_CHANNEL);

    /* ── Initialize ESP-NOW ────────────────────────────────────────── */
    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_recv_cb(espnow_recv_cb));
    ESP_ERROR_CHECK(esp_now_register_send_cb(espnow_send_cb));

    /* Add broadcast peer (always needed for discovery PINGs) */
    esp_now_peer_info_t bcast_peer = {};
    memcpy(bcast_peer.peer_addr, OPENDASH_ESPNOW_BROADCAST, 6);
    bcast_peer.channel = OPENDASH_ESPNOW_CHANNEL;
    bcast_peer.ifidx   = WIFI_IF_STA;
    bcast_peer.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&bcast_peer));

    s_initialized = true;
    ESP_LOGI(TAG, "ESP-NOW transport ready (zero wires, full speed)");
    return ESP_OK;
}

esp_err_t opendash_espnow_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    esp_wifi_stop();
    esp_wifi_deinit();

    if (s_recv_queue) {
        vQueueDelete(s_recv_queue);
        s_recv_queue = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "ESP-NOW transport shut down");
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API — Sending
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t opendash_espnow_send(const uint8_t *dst_mac,
                                const uint8_t *data, size_t len)
{
    if (!s_initialized || !dst_mac || !data || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > OPENDASH_ESPNOW_MAX_DATA) {
        ESP_LOGE(TAG, "Payload too large (%d > %d)",
                 (int)len, OPENDASH_ESPNOW_MAX_DATA);
        return ESP_ERR_INVALID_SIZE;
    }

    return esp_now_send(dst_mac, data, (size_t)len);
}

esp_err_t opendash_espnow_broadcast(const uint8_t *data, size_t len)
{
    return opendash_espnow_send(OPENDASH_ESPNOW_BROADCAST, data, len);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API — Receiving
 * ──────────────────────────────────────────────────────────────────────────── */

bool opendash_espnow_recv(opendash_espnow_event_t *evt, uint32_t wait_ms)
{
    if (!s_recv_queue || !evt) return false;

    TickType_t ticks = (wait_ms == 0) ? 0 : pdMS_TO_TICKS(wait_ms);
    return xQueueReceive(s_recv_queue, evt, ticks) == pdTRUE;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API — Peer Management
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t opendash_espnow_add_peer(const uint8_t *mac)
{
    if (!s_initialized || !mac) return ESP_ERR_INVALID_ARG;

    /* Idempotent: skip if already registered */
    if (esp_now_is_peer_exist(mac)) return ESP_OK;

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, mac, 6);
    peer.channel = OPENDASH_ESPNOW_CHANNEL;
    peer.ifidx   = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t ret = esp_now_add_peer(&peer);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Peer added: " MACSTR, MAC2STR(mac));
    } else {
        ESP_LOGE(TAG, "Failed to add peer " MACSTR ": %s",
                 MAC2STR(mac), esp_err_to_name(ret));
    }
    return ret;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API — Utility
 * ──────────────────────────────────────────────────────────────────────────── */

void opendash_espnow_get_mac(uint8_t *mac_out)
{
    if (mac_out) memcpy(mac_out, s_self_mac, 6);
}
