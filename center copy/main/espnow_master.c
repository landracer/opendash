/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file espnow_master.c
 * @brief OpenDash Center Display — ESP-NOW Wireless Master Controller
 *
 * Implements the ESP-NOW master that discovers and communicates with all
 * peripheral nodes (Left, Right, GPS) wirelessly.
 *
 * Polling loop (runs at ~2 Hz):
 *   1. Broadcast PING to discover/verify all nodes
 *   2. Process any incoming responses (STATUS_REPORT, DATA_RESPONSE)
 *   3. Push demo data point updates to online gauge pods
 *   4. Request GPS/IMU data from GPS node (if online)
 *   5. Periodic status logging
 *
 * Node discovery:
 *   - Center broadcasts PING via broadcast MAC (all nodes hear it)
 *   - Each node responds with STATUS_REPORT containing its node ID
 *   - Center extracts the sender's MAC from the response event
 *   - Center registers the MAC as a peer for future unicast
 *
 * @see opendash_espnow.h for the transport layer.
 * @see opendash_i2c_protocol.h for message format (reused as ESP-NOW payload).
 */

#include "espnow_master.h"
#include "opendash_espnow.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"
#include "opendash_common.h"
#include "opendash_logger.h"
#include "ui_manager.h"
#include "display_init.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <math.h>

static const char *TAG = "espnow_master";

/* ────────────────────────────────────────────────────────────────────────────
 * Configuration Constants
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Time between full polling cycles (ms). */
#define POLL_INTERVAL_MS        20

/** @brief Time to drain receive queue per cycle (ms). */
#define RECV_DRAIN_TIMEOUT_MS   10

/** @brief Seconds without response before marking node offline. */
#define OFFLINE_TIMEOUT_S       15

/** @brief How often to log node status (in poll cycles). */
#define STATUS_LOG_INTERVAL     500     /* Every ~10 seconds at ~20ms cycle */

/** @brief Maximum tracked slave nodes. */
#define MAX_NODES               3

/* ────────────────────────────────────────────────────────────────────────────
 * Human-readable node name
 * ──────────────────────────────────────────────────────────────────────────── */

static const char *node_name(opendash_node_t node)
{
    switch (node) {
        case OPENDASH_NODE_LEFT:   return "LEFT";
        case OPENDASH_NODE_RIGHT:  return "RIGHT";
        case OPENDASH_NODE_GPS:    return "GPS";
        case OPENDASH_NODE_CENTER: return "CENTER";
        case OPENDASH_NODE_BMS:    return "BMS";
        default:                   return "UNKNOWN";
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Node Tracking
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    opendash_node_t  node;              /**< Node type (LEFT, RIGHT, GPS) */
    bool             online;            /**< Currently responding? */
    bool             mac_known;         /**< Have we learned this node's MAC? */
    uint8_t          mac[6];            /**< WiFi MAC address (learned via discovery) */
    uint32_t         last_seen_ms;      /**< Timestamp of last response */
    int              last_rssi;         /**< Last signal strength (dBm) */
} node_info_t;

static node_info_t s_nodes[MAX_NODES] = {
    { .node = OPENDASH_NODE_LEFT,  .online = false, .mac_known = false },
    { .node = OPENDASH_NODE_RIGHT, .online = false, .mac_known = false },
    { .node = OPENDASH_NODE_GPS,   .online = false, .mac_known = false },
};

/* ────────────────────────────────────────────────────────────────────────────
 * Task State
 * ──────────────────────────────────────────────────────────────────────────── */

static TaskHandle_t                 s_task_handle = NULL;
static espnow_master_node_status_t  s_node_status = {0};

/* Scratch buffer for protocol serialization (single-threaded use) */
static uint8_t s_tx_buf[OPENDASH_ESPNOW_MAX_DATA];

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers — Send Protocol Messages
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Send a serialized protocol message to a specific node (unicast)
 *        or broadcast if the node's MAC is not yet known.
 */
static esp_err_t send_msg_to_node(node_info_t *ni, const opendash_i2c_msg_t *msg)
{
    uint16_t tx_len = 0;
    opendash_err_t od_ret = opendash_i2c_serialize(msg, s_tx_buf, &tx_len);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGE(TAG, "Serialize failed for cmd 0x%02X", msg->cmd);
        return ESP_FAIL;
    }

    if (ni->mac_known) {
        return opendash_espnow_send(ni->mac, s_tx_buf, tx_len);
    } else {
        return opendash_espnow_broadcast(s_tx_buf, tx_len);
    }
}

/**
 * @brief Broadcast a serialized protocol message to all nodes.
 */
static esp_err_t broadcast_msg(const opendash_i2c_msg_t *msg)
{
    uint16_t tx_len = 0;
    opendash_err_t od_ret = opendash_i2c_serialize(msg, s_tx_buf, &tx_len);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGE(TAG, "Serialize failed for cmd 0x%02X", msg->cmd);
        return ESP_FAIL;
    }

    return opendash_espnow_broadcast(s_tx_buf, tx_len);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Node Discovery & Status
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Find a node_info slot by node type.
 */
static node_info_t *find_node(opendash_node_t node)
{
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].node == node) return &s_nodes[i];
    }
    return NULL;
}

/**
 * @brief Process a received STATUS_REPORT — register the sender's MAC
 *        and mark the node as online.
 */
static void handle_status_report(const opendash_espnow_event_t *evt,
                                  const opendash_i2c_msg_t *msg)
{
    if (msg->length < 1) return;

    opendash_node_t sender_node = (opendash_node_t)msg->payload[0];
    node_info_t *ni = find_node(sender_node);
    if (!ni) {
        ESP_LOGD(TAG, "STATUS_REPORT from unknown node type %d", sender_node);
        return;
    }

    /* Learn or update the node's MAC address */
    if (!ni->mac_known || memcmp(ni->mac, evt->src_mac, 6) != 0) {
        memcpy(ni->mac, evt->src_mac, 6);
        ni->mac_known = true;

        /* Register as ESP-NOW peer for future unicast */
        opendash_espnow_add_peer(ni->mac);

        ESP_LOGI(TAG, "Discovered %s @ " MACSTR " (RSSI=%d dBm)",
                 node_name(ni->node), MAC2STR(ni->mac), evt->rssi);
    }

    /* Mark online */
    if (!ni->online) {
        ESP_LOGI(TAG, "Node %s is ONLINE (RSSI=%d dBm)",
                 node_name(ni->node), evt->rssi);
    }
    ni->online = true;
    ni->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
    ni->last_rssi = evt->rssi;
}

/**
 * @brief Pending UI updates — cached from DATA_RESPONSE messages.
 * Updated in handle_data_response, flushed to LVGL by the main task loop.
 */
#define MAX_PENDING_UI  16
static struct {
    uint16_t dp_id;
    float    value;
} s_pending_ui[MAX_PENDING_UI];
static int s_pending_ui_count = 0;

/**
 * @brief Process a received DATA_RESPONSE from a node.
 *
 * Logs data, queues center display update, forwards to gauge pods.
 * Does NOT call LVGL directly (avoids watchdog in tight loops).
 */
static void handle_data_response(const opendash_espnow_event_t *evt,
                                  const opendash_i2c_msg_t *msg)
{
    if (msg->length < 6) return;

    uint16_t dp_id = (msg->payload[0] << 8) | msg->payload[1];
    float value;
    memcpy(&value, &msg->payload[2], sizeof(float));

    ESP_LOGD(TAG, "DATA_RESPONSE: dp=0x%04X value=%.2f from " MACSTR,
             dp_id, value, MAC2STR(evt->src_mac));

    /* Log to flash */
    opendash_logger_log(dp_id, value);

    /* Queue for center display update (batch applied later) */
    if (s_pending_ui_count < MAX_PENDING_UI) {
        s_pending_ui[s_pending_ui_count].dp_id = dp_id;
        s_pending_ui[s_pending_ui_count].value = value;
        s_pending_ui_count++;
    }

    /* Forward to all online gauge pods (LEFT, RIGHT) */
    for (int i = 0; i < 2; i++) {
        if (s_nodes[i].online && s_nodes[i].mac_known) {
            uint8_t payload[6];
            payload[0] = (dp_id >> 8) & 0xFF;
            payload[1] = dp_id & 0xFF;
            memcpy(&payload[2], &value, sizeof(float));

            opendash_i2c_msg_t fwd_msg;
            opendash_i2c_build_msg(&fwd_msg, OPENDASH_CMD_SET_DATA_POINT,
                                    payload, sizeof(payload));
            send_msg_to_node(&s_nodes[i], &fwd_msg);
        }
    }
}

/**
 * @brief Flush pending UI updates to LVGL.
 *
 * Processes 1 update per LVGL lock/unlock cycle with WDT feed between
 * iterations. Each ui_manager_update_value() triggers ~10 lv_label_set_text()
 * via outlined shadow labels, which is expensive on 800×480 RGB display.
 * Deduplicates first: if multiple updates for the same dp_id are pending,
 * only the latest value is applied.
 */
static void flush_pending_ui_updates(void)
{
    if (s_pending_ui_count == 0) return;

    /* Deduplicate: for each dp_id keep only the last value */
    for (int i = 0; i < s_pending_ui_count; i++) {
        for (int j = i + 1; j < s_pending_ui_count; j++) {
            if (s_pending_ui[j].dp_id == s_pending_ui[i].dp_id) {
                /* Later entry supersedes — mark earlier one invalid */
                s_pending_ui[i].dp_id = 0xFFFF;
                break;
            }
        }
    }

    /* Process one update per lock/unlock cycle, feed WDT between */
    for (int i = 0; i < s_pending_ui_count; i++) {
        if (s_pending_ui[i].dp_id == 0xFFFF) continue;  /* Skip deduped */

        if (display_lvgl_lock(10)) {
            ui_manager_update_value(s_pending_ui[i].dp_id, s_pending_ui[i].value);
            display_lvgl_unlock();
        }
        esp_task_wdt_reset();
        vTaskDelay(1);  /* Yield between updates */
    }
    s_pending_ui_count = 0;
}

/**
 * @brief Drain the receive queue and process all pending responses.
 */
static void process_responses(void)
{
    opendash_espnow_event_t evt;
    int processed = 0;

    while (opendash_espnow_recv(&evt, RECV_DRAIN_TIMEOUT_MS)) {
        /* Try to deserialize as an OpenDash protocol message */
        opendash_i2c_msg_t msg;
        opendash_err_t ret = opendash_i2c_deserialize(evt.data, evt.len, &msg);
        if (ret != OPENDASH_OK) {
            ESP_LOGD(TAG, "Invalid message from " MACSTR " (len=%d, err=%d)",
                     MAC2STR(evt.src_mac), evt.len, ret);
            continue;
        }

        /* Yield every 4 messages to prevent watchdog timeout */
        if (++processed >= 4) {
            vTaskDelay(1);
            processed = 0;
        }

        switch (msg.cmd) {
            case OPENDASH_CMD_STATUS_REPORT:
                handle_status_report(&evt, &msg);
                break;

            case OPENDASH_CMD_DATA_RESPONSE:
                handle_data_response(&evt, &msg);
                break;

            case OPENDASH_CMD_NAK:
                ESP_LOGD(TAG, "NAK from " MACSTR, MAC2STR(evt.src_mac));
                break;

            default:
                ESP_LOGD(TAG, "Unexpected cmd 0x%02X from " MACSTR,
                         msg.cmd, MAC2STR(evt.src_mac));
                break;
        }
    }
}

/**
 * @brief Check for nodes that have gone silent and mark them offline.
 */
static void check_offline_nodes(void)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].online) {
            uint32_t elapsed = now_ms - s_nodes[i].last_seen_ms;
            if (elapsed > (OFFLINE_TIMEOUT_S * 1000)) {
                ESP_LOGW(TAG, "Node %s went OFFLINE (no response for %d ms)",
                         node_name(s_nodes[i].node), (int)elapsed);
                s_nodes[i].online = false;
            }
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Data Push — Demo Sweep Generator
 * ──────────────────────────────────────────────────────────────────────────── */

/* ────────────────────────────────────────────────────────────────────────────
 * Realistic 7-Second 1/4 Mile Drag Race Simulation
 *
 * Keyframe-based with cubic interpolation for smooth, realistic data.
 * Simulates: launch → 1st gear → 2nd → 3rd → 4th → finish → cooldown → repeat
 *
 * Total cycle: ~12 seconds (7s run + 3s cooldown + 2s staging)
 * ──────────────────────────────────────────────────────────────────────────── */
#define DRAG_CYCLE_SECS     12.0f    /* Total cycle duration */
#define DRAG_RUN_SECS       7.0f     /* 1/4 mile duration */
#define DRAG_COOLDOWN_SECS  3.0f     /* Cooldown after finish */

/**
 * @brief Smooth interpolation between two values (cubic ease in/out).
 */
static float lerp_smooth(float a, float b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    /* Smoothstep: 3t² - 2t³ */
    t = t * t * (3.0f - 2.0f * t);
    return a + (b - a) * t;
}

/**
 * @brief Keyframe interpolation for multi-segment data.
 *
 * @param times   Array of keyframe times (sorted ascending).
 * @param values  Array of values at each keyframe.
 * @param n       Number of keyframes.
 * @param t       Current time.
 * @return Interpolated value.
 */
static float keyframe_interp(const float *times, const float *values, int n, float t)
{
    if (t <= times[0]) return values[0];
    if (t >= times[n - 1]) return values[n - 1];

    for (int i = 0; i < n - 1; i++) {
        if (t >= times[i] && t < times[i + 1]) {
            float seg_t = (t - times[i]) / (times[i + 1] - times[i]);
            return lerp_smooth(values[i], values[i + 1], seg_t);
        }
    }
    return values[n - 1];
}

/**
 * @brief Generate drag race demo data at current time.
 *
 * RPM profile simulates gear shifts with sawtooth pattern:
 *   Launch: 5500 RPM → 7800 shift → drop to 5200 → 7800 shift → etc.
 * Speed: 0 → 170 mph over 7 seconds
 * Boost: tracks RPM load
 */
static void drag_race_values(float *rpm, float *speed_mph, float *boost_kpa,
                              float *coolant_c, float *oil_c, float *batt_v,
                              float *afr)
{
    float t_us = (float)(esp_timer_get_time());
    float t_sec = t_us / 1000000.0f;
    float t = fmodf(t_sec, DRAG_CYCLE_SECS);

    /* ── Before launch (staging): idle at ~1200 RPM ───────── */
    if (t < 2.0f) {
        float stage_t = t / 2.0f;
        *rpm       = lerp_smooth(1200.0f, 5500.0f, stage_t);  /* Building revs */
        *speed_mph = 0.0f;
        *boost_kpa = lerp_smooth(0.0f, 50.0f, stage_t);
        *coolant_c = 85.0f;
        *oil_c     = 90.0f;
        *batt_v    = 13.8f;
        *afr       = 14.7f;  /* Stoichiometric at idle/staging */
        return;
    }

    /* ── Drag run (7 seconds) ─────────────────────────────── */
    float run_t = t - 2.0f;
    if (run_t < DRAG_RUN_SECS) {
        /* Speed: smooth acceleration curve 0→170 mph */
        const float spd_times[]  = {0.0f, 0.5f, 1.5f, 2.5f, 3.5f, 4.5f, 5.5f, 7.0f};
        const float spd_values[] = {0.0f, 15.0f, 45.0f, 72.0f, 100.0f, 130.0f, 155.0f, 170.0f};
        *speed_mph = keyframe_interp(spd_times, spd_values, 8, run_t);

        /* RPM: sawtooth pattern — 4 gear shifts
         * 1st: 5500→7800 (0-1.5s), shift at 1.5s
         * 2nd: 5200→7600 (1.5-3.0s), shift at 3.0s
         * 3rd: 5000→7400 (3.0-5.0s), shift at 5.0s
         * 4th: 4800→7800 (5.0-7.0s) */
        if (run_t < 1.5f) {
            float seg = run_t / 1.5f;
            *rpm = lerp_smooth(5500.0f, 7800.0f, seg);
        } else if (run_t < 1.55f) {
            *rpm = lerp_smooth(7800.0f, 5200.0f, (run_t - 1.5f) / 0.05f);
        } else if (run_t < 3.0f) {
            float seg = (run_t - 1.55f) / (3.0f - 1.55f);
            *rpm = lerp_smooth(5200.0f, 7600.0f, seg);
        } else if (run_t < 3.05f) {
            *rpm = lerp_smooth(7600.0f, 5000.0f, (run_t - 3.0f) / 0.05f);
        } else if (run_t < 5.0f) {
            float seg = (run_t - 3.05f) / (5.0f - 3.05f);
            *rpm = lerp_smooth(5000.0f, 7400.0f, seg);
        } else if (run_t < 5.05f) {
            *rpm = lerp_smooth(7400.0f, 4800.0f, (run_t - 5.0f) / 0.05f);
        } else {
            float seg = (run_t - 5.05f) / (7.0f - 5.05f);
            *rpm = lerp_smooth(4800.0f, 7800.0f, seg);
        }

        /* Boost follows RPM proportionally, 0→200 kPa */
        *boost_kpa = (*rpm - 4000.0f) / (7800.0f - 4000.0f) * 200.0f;
        if (*boost_kpa < 0.0f) *boost_kpa = 0.0f;

        /* Coolant rises under load: 85→98°C over the run */
        *coolant_c = lerp_smooth(85.0f, 98.0f, run_t / DRAG_RUN_SECS);
        /* Oil rises faster: 90→115°C */
        *oil_c = lerp_smooth(90.0f, 115.0f, run_t / DRAG_RUN_SECS);
        /* Battery voltage drops under load: 13.8→12.8V */
        *batt_v = lerp_smooth(13.8f, 12.8f, run_t / DRAG_RUN_SECS);
        /* AFR: rich under boost (10.9), stoichiometric off-boost */
        *afr = (*boost_kpa > 20.0f) ? 10.9f : lerp_smooth(14.7f, 10.9f, *boost_kpa / 20.0f);
        return;
    }

    /* ── Cooldown (3 seconds) ─────────────────────────────── */
    float cool_t = (run_t - DRAG_RUN_SECS) / DRAG_COOLDOWN_SECS;
    if (cool_t > 1.0f) cool_t = 1.0f;

    *rpm       = lerp_smooth(7800.0f, 1200.0f, cool_t);
    *speed_mph = lerp_smooth(170.0f, 0.0f, cool_t);
    *boost_kpa = lerp_smooth(200.0f, 0.0f, cool_t);
    *coolant_c = lerp_smooth(98.0f, 88.0f, cool_t);
    *oil_c     = lerp_smooth(115.0f, 95.0f, cool_t);
    *batt_v    = lerp_smooth(12.8f, 13.8f, cool_t);
    *afr       = lerp_smooth(10.9f, 14.7f, cool_t);  /* Returns to stoich */
}

/**
 * @brief Push a SET_DATA_POINT message to a node.
 */
static esp_err_t push_data_point(node_info_t *ni, uint16_t dp_id, float value)
{
    if (!ni->online || !ni->mac_known) return ESP_ERR_NOT_FOUND;

    uint8_t payload[6];
    payload[0] = (dp_id >> 8) & 0xFF;
    payload[1] = dp_id & 0xFF;
    memcpy(&payload[2], &value, sizeof(float));

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_SET_DATA_POINT,
                            payload, sizeof(payload));

    return send_msg_to_node(ni, &msg);
}

/* ────────────────────────────────────────────────────────────────────────────
 * ESP-NOW Master Polling Task
 * ──────────────────────────────────────────────────────────────────────────── */

static void espnow_master_task(void *pvParameters)
{
    ESP_LOGI(TAG, "ESP-NOW master task started");

    /* Subscribe this task to the task watchdog so deadlocks are caught,
     * even though we disabled the IDLE0 check (CPU0 is loaded by WiFi). */
    esp_task_wdt_add(NULL);

    uint32_t cycle = 0;

    while (1) {
        esp_task_wdt_reset();  /* Feed WDT at top of each loop iteration */
        /* ── Phase 1: Broadcast PING for discovery / keepalive ───── */
        opendash_i2c_msg_t ping_msg;
        uint8_t subcmd = OPENDASH_SUBCMD_PING;
        opendash_i2c_build_msg(&ping_msg, OPENDASH_CMD_SYSTEM, &subcmd, 1);
        broadcast_msg(&ping_msg);

        /* ── Phase 2: Process all incoming responses ─────────────── */
        vTaskDelay(pdMS_TO_TICKS(50));   /* Give nodes time to respond */
        process_responses();

        /* Flush any queued GPS data UI updates in a single LVGL lock */
        flush_pending_ui_updates();
        vTaskDelay(pdMS_TO_TICKS(10));  /* Yield for WDT after UI batch */

        /* ── Phase 3: Check for offline nodes ────────────────────── */
        check_offline_nodes();

        /* Update global status snapshot */
        s_node_status.left_online  = s_nodes[0].online;
        s_node_status.right_online = s_nodes[1].online;
        s_node_status.gps_online   = s_nodes[2].online;

        /* ── Phase 4: Drag race demo data + push to all displays ─── */
        {
            float rpm, speed_mph, boost, coolant, oil, batt, afr;
            drag_race_values(&rpm, &speed_mph, &boost, &coolant, &oil, &batt, &afr);

            /* ── Demo GPS data — derived from drag race speed ─── */
            float demo_t    = (float)(esp_timer_get_time()) / 1000000.0f;
            float gps_speed  = speed_mph;                              /* Same as drag speed */
            float altitude   = 150.0f + speed_mph * 0.3f;             /* Simulated altitude (m) */
            float sat_count  = 12.0f;                                  /* Fixed satellite count */
            float heading    = fmodf(demo_t * 5.0f, 360.0f);          /* Slowly rotating heading */
            float latitude   = 33.7490f + sinf(demo_t / 5.0f) * 0.001f;
            float hdop       = 0.8f;                                   /* Good HDOP */
            float longitude  = -84.3880f + cosf(demo_t / 5.0f) * 0.001f;

            /* Log all values */
            opendash_logger_log(OPENDASH_DP_RPM, rpm);
            opendash_logger_log(OPENDASH_DP_COOLANT_TEMP, coolant);
            opendash_logger_log(OPENDASH_DP_OIL_TEMP, oil);
            opendash_logger_log(OPENDASH_DP_BATTERY_VOLTAGE, batt);
            opendash_logger_log(OPENDASH_DP_BOOST_PRESSURE, boost);
            opendash_logger_log(OPENDASH_DP_AFR, afr);

            /* Batch all LVGL updates in a single lock (minimizes
             * display latency — all values update in the same frame). */
            if (display_lvgl_lock(10)) {
                ui_manager_update_value(OPENDASH_DP_RPM, rpm);
                ui_manager_update_value(OPENDASH_DP_COOLANT_TEMP, coolant);
                ui_manager_update_value(OPENDASH_DP_OIL_TEMP, oil);
                ui_manager_update_value(OPENDASH_DP_BATTERY_VOLTAGE, batt);
                ui_manager_update_value(OPENDASH_DP_BOOST_PRESSURE, boost);
                ui_manager_update_value(OPENDASH_DP_AFR, afr);
                /* GPS demo data — keeps GPS screen alive after swipe */
                ui_manager_update_value(OPENDASH_DP_GPS_SPEED, gps_speed);
                ui_manager_update_value(OPENDASH_DP_ALTITUDE, altitude);
                ui_manager_update_value(OPENDASH_DP_SAT_COUNT, sat_count);
                ui_manager_update_value(OPENDASH_DP_GPS_HEADING, heading);
                ui_manager_update_value(OPENDASH_DP_LATITUDE, latitude);
                ui_manager_update_value(OPENDASH_DP_HDOP, hdop);
                ui_manager_update_value(OPENDASH_DP_LONGITUDE, longitude);
                display_lvgl_unlock();
            }
            esp_task_wdt_reset();

            /* Push to online gauge pods (LEFT, RIGHT) — all at once,
             * no per-node delay so they receive simultaneously.      */
            for (int i = 0; i < 2; i++) {
                if (!s_nodes[i].online || !s_nodes[i].mac_known) continue;

                push_data_point(&s_nodes[i], OPENDASH_DP_RPM, rpm);
                push_data_point(&s_nodes[i], OPENDASH_DP_COOLANT_TEMP, coolant);
                push_data_point(&s_nodes[i], OPENDASH_DP_OIL_TEMP, oil);
                push_data_point(&s_nodes[i], OPENDASH_DP_BATTERY_VOLTAGE, batt);
                push_data_point(&s_nodes[i], OPENDASH_DP_BOOST_PRESSURE, boost);
                push_data_point(&s_nodes[i], OPENDASH_DP_AFR, afr);
                push_data_point(&s_nodes[i], OPENDASH_DP_GPS_SPEED, gps_speed);
            }
        }

        /* ── Phase 4b: Drain any responses that arrived during push ─ */
        process_responses();

        /* ── Phase 5: Request GPS data (if GPS node is online) ──── */
        if (s_nodes[2].online && s_nodes[2].mac_known) {
            uint8_t req_payload[2];
            req_payload[0] = (OPENDASH_DP_GPS_SPEED >> 8) & 0xFF;
            req_payload[1] = OPENDASH_DP_GPS_SPEED & 0xFF;

            opendash_i2c_msg_t req_msg;
            opendash_i2c_build_msg(&req_msg, OPENDASH_CMD_REQUEST_DATA,
                                    req_payload, sizeof(req_payload));
            send_msg_to_node(&s_nodes[2], &req_msg);
        }

        /* ── Phase 6: Periodic status log ────────────────────────── */
        if ((cycle % STATUS_LOG_INTERVAL) == 0) {
            ESP_LOGI(TAG, "Status: Left=%s%s  Right=%s%s  GPS=%s%s",
                     s_nodes[0].online ? "ONLINE" : "offline",
                     s_nodes[0].mac_known ? "" : "(undiscovered)",
                     s_nodes[1].online ? "ONLINE" : "offline",
                     s_nodes[1].mac_known ? "" : "(undiscovered)",
                     s_nodes[2].online ? "ONLINE" : "offline",
                     s_nodes[2].mac_known ? "" : "(undiscovered)");

            /* Log RSSI for online nodes */
            for (int i = 0; i < MAX_NODES; i++) {
                if (s_nodes[i].online) {
                    ESP_LOGI(TAG, "  %s RSSI: %d dBm",
                             node_name(s_nodes[i].node), s_nodes[i].last_rssi);
                }
            }
        }

        cycle++;
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t espnow_master_init(void)
{
    ESP_LOGI(TAG, "Initializing ESP-NOW master...");

    /* Initialize data logger (mounts SPIFFS, loads session counter) */
    esp_err_t log_ret = opendash_logger_init();
    if (log_ret != ESP_OK) {
        ESP_LOGW(TAG, "Logger init failed: %s (logging disabled)",
                 esp_err_to_name(log_ret));
    }

    return opendash_espnow_init(OPENDASH_NODE_CENTER);
}

esp_err_t espnow_master_start(void)
{
    /* Start data logging session */
    if (opendash_logger_init() == ESP_OK) {
        opendash_logger_start();
    }

    BaseType_t ret = xTaskCreatePinnedToCore(
        espnow_master_task,
        "espnow_master",
        6144,
        NULL,
        4,                  /* Medium priority */
        &s_task_handle,
        0                   /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ESP-NOW master task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ESP-NOW master task started on core 0");
    return ESP_OK;
}

void espnow_master_get_status(espnow_master_node_status_t *status)
{
    if (status) {
        *status = s_node_status;
    }
}

esp_err_t espnow_master_send_data_point(opendash_node_t node,
                                         uint16_t dp_id, float value)
{
    node_info_t *ni = find_node(node);
    if (!ni) return ESP_ERR_INVALID_ARG;
    return push_data_point(ni, dp_id, value);
}
