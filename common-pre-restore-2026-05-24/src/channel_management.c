/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file channel_management.c
 * @brief Channel dispatcher and node registry — implementation
 *
 * Core routing engine for event-driven ESP-NOW architecture.
 * Replaces the old 20ms PING polling loop with:
 *   - Per-channel FreeRTOS queues
 *   - Data-absence offline detection
 *   - Delta-based data point forwarding
 *   - Channel-aware retry policies
 *
 * ARCHITECTURE RULE: NO POLLING / NO PINGING.
 */

#include "channel_management.h"
#include "node_definitions.h"
#include "opendash_espnow.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include <string.h>
#include <math.h>

static const char *TAG = "channel_mgr";

/* ────────────────────────────────────────────────────────────────────────────
 * Internal State
 * ──────────────────────────────────────────────────────────────────────────── */

/** Inbound queue per channel */
static QueueHandle_t s_queues[CHANNEL_COUNT];

/** Node registry — one slot per opendash_node_t */
static channel_node_t s_nodes[OPENDASH_NODE_COUNT];

/** Mutex protecting the node registry (shared between dispatcher + recv cb) */
static SemaphoreHandle_t s_registry_mutex;

/** Per-channel statistics */
static channel_stats_t s_stats[CHANNEL_COUNT];

/** Delta tracking: last-known data point values */
typedef struct {
    uint16_t dp_id;
    float    value;
    bool     valid;
} dp_cache_entry_t;

static dp_cache_entry_t s_dp_cache[CHANNEL_MAX_DATA_POINTS];

static bool s_initialized = false;

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────────── */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Lifecycle
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t channel_mgr_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    /* Create registry mutex */
    s_registry_mutex = xSemaphoreCreateMutex();
    if (!s_registry_mutex) {
        ESP_LOGE(TAG, "Failed to create registry mutex");
        return ESP_ERR_NO_MEM;
    }

    /* Create per-channel inbound queues */
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        s_queues[ch] = xQueueCreate(CHANNEL_QUEUE_DEPTH,
                                     sizeof(channel_inbound_msg_t));
        if (!s_queues[ch]) {
            ESP_LOGE(TAG, "Failed to create queue for channel %d", ch);
            return ESP_ERR_NO_MEM;
        }
        /* Init stats */
        memset(&s_stats[ch], 0, sizeof(channel_stats_t));
        s_stats[ch].channel_id = ch;
        s_stats[ch].active = false;
    }

    /* Set channel processing intervals */
    s_stats[CHANNEL_CRITICAL].interval_ms = CHANNEL_CRITICAL_INTERVAL_MS;
    s_stats[CHANNEL_MEDIUM].interval_ms   = CHANNEL_MEDIUM_INTERVAL_MS;
    s_stats[CHANNEL_LOW].interval_ms      = CHANNEL_LOW_INTERVAL_MS;
    s_stats[CHANNEL_CONTROL].interval_ms  = CHANNEL_CONTROL_INTERVAL_MS;

    /* Zero node registry */
    memset(s_nodes, 0, sizeof(s_nodes));
    for (int n = 0; n < OPENDASH_NODE_COUNT; n++) {
        s_nodes[n].node_type  = (opendash_node_t)n;
        s_nodes[n].channel_id = NODE_DEFAULT_CHANNEL[n];
        s_nodes[n].online     = false;
        s_nodes[n].mac_known  = false;
    }

    /* Zero delta cache */
    memset(s_dp_cache, 0, sizeof(s_dp_cache));

    s_initialized = true;
    ESP_LOGI(TAG, "Channel manager initialized — %d channels, %d node slots",
             CHANNEL_COUNT, OPENDASH_NODE_COUNT);
    return ESP_OK;
}

esp_err_t channel_mgr_deinit(void)
{
    if (!s_initialized) return ESP_OK;

    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        if (s_queues[ch]) {
            vQueueDelete(s_queues[ch]);
            s_queues[ch] = NULL;
        }
    }
    if (s_registry_mutex) {
        vSemaphoreDelete(s_registry_mutex);
        s_registry_mutex = NULL;
    }

    s_initialized = false;
    ESP_LOGI(TAG, "Channel manager deinitialized");
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Node Registry
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t channel_mgr_register_node(opendash_node_t node_type,
                                     const uint8_t mac[6],
                                     uint8_t channel_id,
                                     uint8_t caps)
{
    if (node_type >= OPENDASH_NODE_COUNT) return ESP_ERR_INVALID_ARG;
    if (channel_id >= CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!mac) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_registry_mutex, portMAX_DELAY);

    channel_node_t *n = &s_nodes[node_type];
    memcpy(n->mac, mac, 6);
    n->mac_known    = true;
    n->channel_id   = channel_id;
    n->capabilities = caps;
    n->online       = true;
    n->last_seen_ms = now_ms();
    n->last_rssi    = 0;

    /* Mark channel active */
    s_stats[channel_id].active = true;

    xSemaphoreGive(s_registry_mutex);

    /* Register as ESP-NOW peer for unicast */
    opendash_espnow_add_peer(mac);

    const char *name = (node_type < OPENDASH_NODE_COUNT) ? NODE_NAMES[node_type] : "?";
    ESP_LOGI(TAG, "Registered %s on CH%d (%s) MAC=" MACSTR,
             name, channel_id, CHANNEL_NAMES[channel_id], MAC2STR(mac));
    return ESP_OK;
}

esp_err_t channel_mgr_unregister_node(opendash_node_t node_type)
{
    if (node_type >= OPENDASH_NODE_COUNT) return ESP_ERR_INVALID_ARG;

    xSemaphoreTake(s_registry_mutex, portMAX_DELAY);

    channel_node_t *n = &s_nodes[node_type];
    n->online    = false;
    n->mac_known = false;
    memset(n->mac, 0, 6);

    xSemaphoreGive(s_registry_mutex);

    ESP_LOGI(TAG, "Unregistered node %d", node_type);
    return ESP_OK;
}

const channel_node_t *channel_mgr_get_node(opendash_node_t node_type)
{
    if (node_type >= OPENDASH_NODE_COUNT) return NULL;
    return &s_nodes[node_type];
}

void channel_mgr_check_timeouts(void)
{
    uint32_t now = now_ms();

    xSemaphoreTake(s_registry_mutex, portMAX_DELAY);

    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        channel_node_t *n = &s_nodes[i];
        if (!n->mac_known || !n->online) continue;

        uint32_t timeout = CHANNEL_OFFLINE_TIMEOUT_MS[n->channel_id];
        if (timeout == 0) continue; /* CONTROL channel: no timeout */

        uint32_t elapsed = now - n->last_seen_ms;
        if (elapsed > timeout) {
            n->online = false;
            ESP_LOGD(TAG, "%s OFFLINE (no data for %lums, threshold %lums)",
                     NODE_NAMES[i], (unsigned long)elapsed, (unsigned long)timeout);
        }
    }

    xSemaphoreGive(s_registry_mutex);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Inbound Message Routing
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t channel_mgr_route_inbound(const channel_inbound_msg_t *msg)
{
    if (!msg) return ESP_ERR_INVALID_ARG;
    if (msg->channel_id >= CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;

    QueueHandle_t q = s_queues[msg->channel_id];
    if (!q) return ESP_ERR_INVALID_STATE;

    /* Track high-water mark */
    UBaseType_t used = CHANNEL_QUEUE_DEPTH - uxQueueSpacesAvailable(q);
    if (used > s_stats[msg->channel_id].queue_high_water) {
        s_stats[msg->channel_id].queue_high_water = (uint32_t)used;
    }

    if (xQueueSend(q, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "CH%d queue full — dropping message from " MACSTR,
                 msg->channel_id, MAC2STR(msg->src_mac));
        return ESP_ERR_NO_MEM;
    }

    s_stats[msg->channel_id].msgs_received++;
    return ESP_OK;
}

bool channel_mgr_recv(uint8_t channel_id,
                       channel_inbound_msg_t *out_msg,
                       uint32_t timeout_ms)
{
    if (channel_id >= CHANNEL_COUNT || !out_msg) return false;

    QueueHandle_t q = s_queues[channel_id];
    if (!q) return false;

    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    return (xQueueReceive(q, out_msg, ticks) == pdTRUE);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Outbound Sending
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t channel_mgr_send_to_node(opendash_node_t node_type,
                                    const uint8_t *data, uint16_t len)
{
    if (node_type >= OPENDASH_NODE_COUNT) return ESP_ERR_INVALID_ARG;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    const channel_node_t *n = &s_nodes[node_type];
    if (!n->mac_known) {
        /* Node not yet registered — broadcast and hope it arrives */
        ESP_LOGD(TAG, "Node %d MAC unknown — broadcasting", node_type);
        esp_err_t ret = opendash_espnow_broadcast(data, len);
        s_stats[n->channel_id].msgs_sent++;
        return ret;
    }

    /* Send with channel-aware retry policy.
     * NOTE: For offline known-MAC nodes we still attempt ONE send (no retries)
     * so the radio gets a chance to deliver if the node has come back without
     * announcing yet, but we never burn time on exponential backoff for them. */
    uint8_t max_retries = CHANNEL_MAX_RETRIES[n->channel_id];
    if (!n->online) max_retries = 0;
    uint32_t backoff_ms = CHANNEL_RETRY_BASE_MS;

    for (uint8_t attempt = 0; attempt <= max_retries; attempt++) {
        esp_err_t ret = opendash_espnow_send(n->mac, data, len);
        if (ret == ESP_OK) {
            s_stats[n->channel_id].msgs_sent++;
            return ESP_OK;
        }
        if (attempt < max_retries) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms *= 2; /* Exponential backoff */
        }
    }

    s_stats[n->channel_id].errors++;
    ESP_LOGW(TAG, "Send to %s failed after %d retries",
             NODE_NAMES[node_type], max_retries + 1);
    return ESP_FAIL;
}

esp_err_t channel_mgr_broadcast_channel(uint8_t channel_id,
                                         const uint8_t *data, uint16_t len)
{
    if (channel_id >= CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    if (!data || len == 0) return ESP_ERR_INVALID_ARG;

    /* Send to every registered online node on this channel */
    esp_err_t last_err = ESP_OK;
    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        const channel_node_t *n = &s_nodes[i];
        if (n->channel_id != channel_id) continue;
        if (!n->mac_known || !n->online) continue;

        esp_err_t ret = opendash_espnow_send(n->mac, data, len);
        if (ret != ESP_OK) {
            last_err = ret;
            s_stats[channel_id].errors++;
        } else {
            s_stats[channel_id].msgs_sent++;
        }
    }
    return last_err;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Delta Tracking
 * ──────────────────────────────────────────────────────────────────────────── */

bool channel_mgr_dp_changed(uint16_t dp_id, float value)
{
    /* Find existing entry */
    int free_slot = -1;
    for (int i = 0; i < CHANNEL_MAX_DATA_POINTS; i++) {
        if (s_dp_cache[i].valid && s_dp_cache[i].dp_id == dp_id) {
            /* Compare with epsilon for float */
            if (fabsf(s_dp_cache[i].value - value) < 0.001f) {
                return false; /* No change */
            }
            s_dp_cache[i].value = value;
            return true; /* Changed */
        }
        if (!s_dp_cache[i].valid && free_slot < 0) {
            free_slot = i;
        }
    }

    /* New DP — record it, report as changed */
    if (free_slot >= 0) {
        s_dp_cache[free_slot].dp_id = dp_id;
        s_dp_cache[free_slot].value = value;
        s_dp_cache[free_slot].valid = true;
    }
    return true;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Statistics
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t channel_mgr_get_stats(uint8_t channel_id, channel_stats_t *out)
{
    if (channel_id >= CHANNEL_COUNT || !out) return ESP_ERR_INVALID_ARG;
    *out = s_stats[channel_id];
    return ESP_OK;
}

void channel_mgr_log_stats(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        ESP_LOGD(TAG, "CH%d(%s): rx=%lu tx=%lu err=%lu qHW=%lu",
                 ch, CHANNEL_NAMES[ch],
                 (unsigned long)s_stats[ch].msgs_received,
                 (unsigned long)s_stats[ch].msgs_sent,
                 (unsigned long)s_stats[ch].errors,
                 (unsigned long)s_stats[ch].queue_high_water);
    }

    /* Log node online/offline summary */
    int online = 0, total = 0;
    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        if (s_nodes[i].mac_known) {
            total++;
            if (s_nodes[i].online) online++;
        }
    }
    ESP_LOGD(TAG, "Nodes: %d/%d online", online, total);
}