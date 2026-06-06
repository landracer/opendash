/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file espnow_master.c
 * @brief OpenDash Center Display — Channel-Based ESP-NOW Master Controller
 *
 * Event-driven master that replaces the legacy 20ms PING polling loop.
 *
 * Task architecture:
 *
 *   espnow_dispatcher_task (core 0, pri 4)
 *       Drains the raw ESP-NOW rx queue, deserializes each message,
 *       identifies the sender (by MAC → node registry lookup),
 *       stamps last_seen, and routes to the correct channel queue.
 *
 *   channel_critical_task (core 0, pri 5)
 *       Processes GPS, BMS, engine data.  ≤10ms latency.
 *       Pushes to UI + forwards to gauge pods.
 *
 *   channel_medium_task (core 0, pri 4)
 *       Processes pod display data, MultiDisplay relay, relay feedback.
 *       ≤50ms latency.
 *
 *   channel_low_task (core 1, pri 3)
 *       Processes diagnostics, config exchanges, logging.
 *       ≤200ms latency.
 *
 *   channel_control_task (core 0, pri 6)
 *       Processes outbound commands (relay ON/OFF, OTA, reboot).
 *       Immediate — ≤5ms latency.
 *
 *   timeout_checker (timer callback, 1 Hz)
 *       Marks stale nodes offline.  No polling involved — just checks
 *       last_seen timestamps vs channel timeout thresholds.
 *
 * ARCHITECTURE RULE: NO POLLING / NO PINGING.
 *
 * @see espnow_master.h        public API
 * @see channel_management.h    routing engine
 * @see channel_config.h        tuning knobs
 * @see node_definitions.h      node→channel map
 */

#include "espnow_master.h"
#include "channel_management.h"
#include "node_definitions.h"

#include "opendash_espnow.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"
#include "opendash_common.h"
#include "opendash_uart.h"
#include "sd_logger.h"
#include "ui_manager.h"
#include "display_init.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_now.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include <string.h>
#include <math.h>

static const char *TAG = "espnow_master";

/* ────────────────────────────────────────────────────────────────────────────
 * Module State
 * ──────────────────────────────────────────────────────────────────────────── */

static espnow_master_node_status_t s_node_status = {0};

/** Relay command queue — decoupled from radio (LVGL touch → queue → send) */
static QueueHandle_t s_relay_cmd_queue = NULL;

/** Timer handle for periodic timeout checks */
static TimerHandle_t s_timeout_timer = NULL;

/* ────────────────────────────────────────────────────────────────────────────
 * Helper: identify sender node from MAC
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Look up which node type sent this message based on MAC address.
 *
 * If the MAC isn't in the registry, we check the STATUS_REPORT payload
 * for the node_type field and auto-register (one-time discovery).
 *
 * @return node type, or OPENDASH_NODE_COUNT if not identifiable.
 */
static opendash_node_t identify_sender(const uint8_t *mac)
{
    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        const channel_node_t *n = channel_mgr_get_node((opendash_node_t)i);
        if (n && n->mac_known && memcmp(n->mac, mac, 6) == 0) {
            return (opendash_node_t)i;
        }
    }
    return OPENDASH_NODE_COUNT; /* Unknown */
}

/**
 * @brief Auto-register a node from its STATUS_REPORT / ANNOUNCE payload.
 *
 * The payload contains the node_type byte at offset 0 (after CMD/LEN).
 * This replaces the old PING discovery — nodes self-identify on first contact.
 */
static void auto_register_node(const uint8_t *mac,
                                const opendash_i2c_msg_t *msg)
{
    if (msg->len < 1) return;

    uint8_t node_type_raw = msg->payload[0];
    if (node_type_raw >= OPENDASH_NODE_COUNT) return;

    opendash_node_t node_type = (opendash_node_t)node_type_raw;

    /* Use default channel and capabilities from the static tables */
    uint8_t ch  = NODE_DEFAULT_CHANNEL[node_type];
    uint8_t cap = NODE_CAPABILITIES[node_type];

    channel_mgr_register_node(node_type, mac, ch, cap);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Dispatcher Task
 *
 * This is the ONLY task that reads from the raw ESP-NOW receive queue.
 * It deserializes, identifies the sender, updates last_seen, and routes
 * the message to the correct channel queue.  NO polling, NO pinging.
 * ──────────────────────────────────────────────────────────────────────────── */

static void espnow_dispatcher_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Dispatcher task started — event-driven, zero polling");
    esp_task_wdt_add(NULL);

    opendash_espnow_event_t evt;

    while (1) {
        esp_task_wdt_reset();

        /*
         * Block until a message arrives on the raw ESP-NOW queue.
         * This is event-driven — we sleep until there's work to do.
         * The 100ms max wait ensures WDT stays happy even at idle.
         */
        if (!opendash_espnow_recv(&evt, 100)) {
            continue; /* No message, loop back (WDT fed above) */
        }

        /* Deserialize the protocol frame */
        opendash_i2c_msg_t msg;
        opendash_err_t ret = opendash_i2c_deserialize(evt.data, evt.len, &msg);
        if (ret != OPENDASH_OK) {
            ESP_LOGD(TAG, "Invalid frame from " MACSTR " (len=%d, err=%d)",
                     MAC2STR(evt.src_mac), evt.len, ret);
            continue;
        }

        /* Identify sender */
        opendash_node_t sender = identify_sender(evt.src_mac);

        /* Auto-register on STATUS_REPORT or ANNOUNCE from unknown node */
        if (sender == OPENDASH_NODE_COUNT) {
            if (msg.cmd == OPENDASH_CMD_STATUS_REPORT ||
                msg.cmd == CHANNEL_MSG_ANNOUNCE) {
                auto_register_node(evt.src_mac, &msg);
                sender = identify_sender(evt.src_mac);
            }
            if (sender == OPENDASH_NODE_COUNT) {
                ESP_LOGD(TAG, "Unknown node " MACSTR " cmd=0x%02X — ignored",
                         MAC2STR(evt.src_mac), msg.cmd);
                continue;
            }
        }

        /* Update last_seen + RSSI (marks node online implicitly) */
        /* Access the node registry through the channel manager */
        channel_node_t *n = (channel_node_t *)channel_mgr_get_node(sender);
        if (n) {
            n->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
            n->last_rssi = (int8_t)evt.rssi;
            if (!n->online) {
                n->online = true;
                ESP_LOGI(TAG, "%s ONLINE (RSSI=%d dBm)",
                         NODE_NAMES[sender], evt.rssi);
            }
        }

        /* Route to the correct channel queue */
        uint8_t ch = NODE_DEFAULT_CHANNEL[sender];
        /* Override: relay/MOS commands go to CONTROL */
        if (msg.cmd == OPENDASH_CMD_SET_RELAY) {
            ch = CHANNEL_CONTROL;
        }

        channel_inbound_msg_t inbound;
        memcpy(inbound.src_mac, evt.src_mac, 6);
        memcpy(inbound.data, evt.data, evt.len);
        inbound.len = (uint16_t)evt.len;
        inbound.rssi = (int8_t)evt.rssi;
        inbound.channel_id = ch;

        channel_mgr_route_inbound(&inbound);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Channel Worker: CRITICAL (CH0)
 *
 * Processes GPS, BMS, engine data.  Forwards to UI and gauge pods.
 * ──────────────────────────────────────────────────────────────────────────── */

static void channel_critical_task(void *pvParameters)
{
    ESP_LOGI(TAG, "CH0 CRITICAL worker started (≤%dms latency)",
             CHANNEL_CRITICAL_INTERVAL_MS);

    channel_inbound_msg_t inbound;

    while (1) {
        /* Drain all pending messages from critical channel */
        while (channel_mgr_recv(CHANNEL_CRITICAL, &inbound, 0)) {
            opendash_i2c_msg_t msg;
            if (opendash_i2c_deserialize(inbound.data, inbound.len, &msg) != OPENDASH_OK) {
                continue;
            }

            if (msg.cmd == OPENDASH_CMD_DATA_RESPONSE && msg.len >= 6) {
                /* Extract data point: [dp_id_hi, dp_id_lo, float32] */
                uint16_t dp_id = ((uint16_t)msg.payload[0] << 8) | msg.payload[1];
                float value;
                memcpy(&value, &msg.payload[2], sizeof(float));

                /* Delta check — skip UI update if value unchanged */
                if (channel_mgr_dp_changed(dp_id, value)) {
                    /* Push to UI */
                    if (display_lvgl_lock(5)) {
                        ui_manager_update_value(dp_id, value);
                        display_lvgl_unlock();
                    }

                    /* Forward to gauge pods (LEFT + RIGHT) as SET_DATA_POINT */
                    uint8_t payload[6];
                    payload[0] = (dp_id >> 8) & 0xFF;
                    payload[1] = dp_id & 0xFF;
                    memcpy(&payload[2], &value, sizeof(float));

                    opendash_i2c_msg_t fwd;
                    opendash_i2c_build_msg(&fwd, OPENDASH_CMD_SET_DATA_POINT,
                                            payload, sizeof(payload));
                    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
                    uint16_t tx_len = 0;
                    if (opendash_i2c_serialize(&fwd, tx_buf, &tx_len) == OPENDASH_OK) {
                        channel_mgr_send_to_node(OPENDASH_NODE_LEFT, tx_buf, tx_len);
                        channel_mgr_send_to_node(OPENDASH_NODE_RIGHT, tx_buf, tx_len);
                    }

                    /* SD logging */
                    /* sd_logger_log_dp(dp_id, value); */
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CHANNEL_CRITICAL_INTERVAL_MS));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Channel Worker: MEDIUM (CH1)
 *
 * Processes pod display data, MultiDisplay relay, sensor readings.
 * ──────────────────────────────────────────────────────────────────────────── */

static void channel_medium_task(void *pvParameters)
{
    ESP_LOGI(TAG, "CH1 MEDIUM worker started (≤%dms latency)",
             CHANNEL_MEDIUM_INTERVAL_MS);

    channel_inbound_msg_t inbound;

    while (1) {
        while (channel_mgr_recv(CHANNEL_MEDIUM, &inbound, 0)) {
            opendash_i2c_msg_t msg;
            if (opendash_i2c_deserialize(inbound.data, inbound.len, &msg) != OPENDASH_OK) {
                continue;
            }

            if (msg.cmd == OPENDASH_CMD_DATA_RESPONSE && msg.len >= 6) {
                uint16_t dp_id = ((uint16_t)msg.payload[0] << 8) | msg.payload[1];
                float value;
                memcpy(&value, &msg.payload[2], sizeof(float));

                if (channel_mgr_dp_changed(dp_id, value)) {
                    if (display_lvgl_lock(10)) {
                        ui_manager_update_value(dp_id, value);
                        display_lvgl_unlock();
                    }

                    /* Forward to RIGHT pod if data came from LEFT */
                    opendash_node_t sender = identify_sender(inbound.src_mac);
                    if (sender == OPENDASH_NODE_LEFT) {
                        uint8_t payload[6];
                        payload[0] = (dp_id >> 8) & 0xFF;
                        payload[1] = dp_id & 0xFF;
                        memcpy(&payload[2], &value, sizeof(float));

                        opendash_i2c_msg_t fwd;
                        opendash_i2c_build_msg(&fwd, OPENDASH_CMD_SET_DATA_POINT,
                                                payload, sizeof(payload));
                        uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
                        uint16_t tx_len = 0;
                        if (opendash_i2c_serialize(&fwd, tx_buf, &tx_len) == OPENDASH_OK) {
                            channel_mgr_send_to_node(OPENDASH_NODE_RIGHT, tx_buf, tx_len);
                        }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CHANNEL_MEDIUM_INTERVAL_MS));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Channel Worker: LOW (CH2)
 *
 * Processes relay/MOS state reports, diagnostics, system info.
 * ──────────────────────────────────────────────────────────────────────────── */

static void channel_low_task(void *pvParameters)
{
    ESP_LOGI(TAG, "CH2 LOW worker started (≤%dms latency)",
             CHANNEL_LOW_INTERVAL_MS);

    channel_inbound_msg_t inbound;

    while (1) {
        while (channel_mgr_recv(CHANNEL_LOW, &inbound, 0)) {
            opendash_i2c_msg_t msg;
            if (opendash_i2c_deserialize(inbound.data, inbound.len, &msg) != OPENDASH_OK) {
                continue;
            }

            if (msg.cmd == OPENDASH_CMD_DATA_RESPONSE && msg.len >= 6) {
                uint16_t dp_id = ((uint16_t)msg.payload[0] << 8) | msg.payload[1];
                float value;
                memcpy(&value, &msg.payload[2], sizeof(float));

                if (display_lvgl_lock(10)) {
                    ui_manager_update_value(dp_id, value);
                    display_lvgl_unlock();
                }
            }

            /* Handle relay state feedback (relay confirms ON/OFF) */
            if (msg.cmd == OPENDASH_CMD_STATUS_REPORT) {
                /* Relay/MOS nodes include relay state in their status */
                opendash_node_t sender = identify_sender(inbound.src_mac);
                if (OPENDASH_NODE_IS_RELAY(sender)) {
                    ESP_LOGD(TAG, "Relay state report from %s", NODE_NAMES[sender]);
                    /* Update UI relay indicators here */
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(CHANNEL_LOW_INTERVAL_MS));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Channel Worker: CONTROL (CH3)
 *
 * Processes outbound commands — relay toggle, OTA, reboot.
 * Also drains the s_relay_cmd_queue for touch-originated relay toggles.
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    opendash_node_t node;
    uint8_t channel;
    uint8_t state;
    uint8_t pwm_duty;
} relay_cmd_t;

static void channel_control_task(void *pvParameters)
{
    ESP_LOGI(TAG, "CH3 CONTROL worker started (≤%dms latency)",
             CHANNEL_CONTROL_INTERVAL_MS);

    relay_cmd_t cmd;
    channel_inbound_msg_t inbound;

    while (1) {
        /* Process relay command queue (from UI touch callbacks) */
        while (xQueueReceive(s_relay_cmd_queue, &cmd, 0) == pdTRUE) {
            espnow_master_send_relay_command(cmd.node, cmd.channel,
                                              cmd.state, cmd.pwm_duty);
        }

        /* Process any inbound control messages */
        while (channel_mgr_recv(CHANNEL_CONTROL, &inbound, 0)) {
            opendash_i2c_msg_t msg;
            if (opendash_i2c_deserialize(inbound.data, inbound.len, &msg) != OPENDASH_OK) {
                continue;
            }
            ESP_LOGD(TAG, "Control msg cmd=0x%02X from " MACSTR,
                     msg.cmd, MAC2STR(inbound.src_mac));
        }

        vTaskDelay(pdMS_TO_TICKS(CHANNEL_CONTROL_INTERVAL_MS));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Timeout Timer (1 Hz)
 *
 * Marks stale nodes offline.  This is NOT polling — it just scans
 * timestamps that were set by the dispatcher when data arrived.
 * ──────────────────────────────────────────────────────────────────────────── */

static void timeout_timer_callback(TimerHandle_t xTimer)
{
    (void)xTimer;
    channel_mgr_check_timeouts();

    /* Update global status snapshot (read by UI without locking) */
    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        const channel_node_t *n = channel_mgr_get_node((opendash_node_t)i);
        if (!n) continue;
        switch (i) {
            case OPENDASH_NODE_LEFT:        s_node_status.left_online       = n->online; break;
            case OPENDASH_NODE_RIGHT:       s_node_status.right_online      = n->online; break;
            case OPENDASH_NODE_GPS:         s_node_status.gps_online        = n->online; break;
            case OPENDASH_NODE_BMS:         s_node_status.bms_online        = n->online; break;
            case OPENDASH_NODE_POD1:        s_node_status.pod1_online       = n->online; break;
            case OPENDASH_NODE_POD2:        s_node_status.pod2_online       = n->online; break;
            case OPENDASH_NODE_RELAY_4CH:   s_node_status.relay_4ch_online  = n->online; break;
            case OPENDASH_NODE_RELAY_8CH_A: s_node_status.relay_8ch_a_online = n->online; break;
            case OPENDASH_NODE_RELAY_8CH_B: s_node_status.relay_8ch_b_online = n->online; break;
            case OPENDASH_NODE_MOS_4CH_A:   s_node_status.mos_4ch_a_online  = n->online; break;
            case OPENDASH_NODE_MOS_4CH_B:   s_node_status.mos_4ch_b_online  = n->online; break;
            default: break;
        }
    }

    /* Push to config screen if visible */
    if (display_lvgl_lock(5)) {
        ui_manager_update_config_node_status(&s_node_status);
        display_lvgl_unlock();
    }

    /* Periodic stats log (every timer tick = 1s) */
    channel_mgr_log_stats();
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t espnow_master_init(void)
{
    ESP_LOGI(TAG, "Initializing channel-based ESP-NOW master (NO polling)");

    /* Init ESP-NOW transport */
    esp_err_t ret = opendash_espnow_init(OPENDASH_NODE_CENTER);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ESP-NOW transport init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Init channel manager */
    ret = channel_mgr_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Channel manager init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create relay command queue (LVGL touch → queue → control task) */
    s_relay_cmd_queue = xQueueCreate(CHANNEL_CMD_QUEUE_DEPTH, sizeof(relay_cmd_t));
    if (!s_relay_cmd_queue) {
        ESP_LOGE(TAG, "Failed to create relay cmd queue");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Channel-based master initialized — awaiting node announcements");
    return ESP_OK;
}

esp_err_t espnow_master_start(void)
{
    BaseType_t ret;

    ESP_LOGI(TAG, "Starting channel-based tasks — NO PING, NO POLL");

    /* Dispatcher (core 0, priority 4): routes raw ESP-NOW → channel queues */
    ret = xTaskCreatePinnedToCore(
        espnow_dispatcher_task, "espnow_dispatch",
        8192, NULL, CHANNEL_TASK_PRIORITY_DISPATCH, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create dispatcher task");
        return ESP_FAIL;
    }

    /* Critical channel (core 0, priority 5) */
    ret = xTaskCreatePinnedToCore(
        channel_critical_task, "ch_critical",
        CHANNEL_TASK_STACK_SIZE, NULL, CHANNEL_TASK_PRIORITY_CRITICAL, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create critical channel task");
        return ESP_FAIL;
    }

    /* Medium channel (core 0, priority 4) */
    ret = xTaskCreatePinnedToCore(
        channel_medium_task, "ch_medium",
        CHANNEL_TASK_STACK_SIZE, NULL, CHANNEL_TASK_PRIORITY_MEDIUM, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create medium channel task");
        return ESP_FAIL;
    }

    /* Low channel (core 1, priority 3) — offloaded to keep core 0 responsive */
    ret = xTaskCreatePinnedToCore(
        channel_low_task, "ch_low",
        CHANNEL_TASK_STACK_SIZE, NULL, CHANNEL_TASK_PRIORITY_LOW, NULL, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create low channel task");
        return ESP_FAIL;
    }

    /* Control channel (core 0, priority 6 — highest) */
    ret = xTaskCreatePinnedToCore(
        channel_control_task, "ch_control",
        CHANNEL_TASK_STACK_SIZE, NULL, CHANNEL_TASK_PRIORITY_CONTROL, NULL, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create control channel task");
        return ESP_FAIL;
    }

    /* Timeout timer: 1 Hz check for stale nodes */
    s_timeout_timer = xTimerCreate("node_timeout", pdMS_TO_TICKS(1000),
                                    pdTRUE, NULL, timeout_timer_callback);
    if (s_timeout_timer) {
        xTimerStart(s_timeout_timer, 0);
    }

    ESP_LOGI(TAG, "Channel-based master running — 4 channels, zero polling");
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
    /* Delta check — suppress if unchanged */
    if (!channel_mgr_dp_changed(dp_id, value)) {
        return ESP_OK;
    }

    uint8_t payload[6];
    payload[0] = (dp_id >> 8) & 0xFF;
    payload[1] = dp_id & 0xFF;
    memcpy(&payload[2], &value, sizeof(float));

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_SET_DATA_POINT,
                            payload, sizeof(payload));

    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t tx_len = 0;
    opendash_err_t od_ret = opendash_i2c_serialize(&msg, tx_buf, &tx_len);
    if (od_ret != OPENDASH_OK) {
        return ESP_FAIL;
    }

    return channel_mgr_send_to_node(node, tx_buf, tx_len);
}

esp_err_t espnow_master_send_relay_command(opendash_node_t node,
                                            uint8_t channel, uint8_t state,
                                            uint8_t pwm_duty)
{
    if (!OPENDASH_NODE_IS_RELAY(node)) {
        ESP_LOGW(TAG, "Node %d is not a relay/MOS node", node);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t payload[3] = { channel, state, pwm_duty };

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_SET_RELAY,
                            payload, sizeof(payload));

    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t tx_len = 0;
    opendash_err_t od_ret = opendash_i2c_serialize(&msg, tx_buf, &tx_len);
    if (od_ret != OPENDASH_OK) {
        return ESP_FAIL;
    }

    /* Relay commands go through control channel (max retries) */
    return channel_mgr_send_to_node(node, tx_buf, tx_len);
}

esp_err_t espnow_master_send_system_subcmd(opendash_node_t node, uint8_t subcmd)
{
    uint8_t payload[1] = { subcmd };

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_SYSTEM,
                            payload, sizeof(payload));

    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t tx_len = 0;
    opendash_err_t od_ret = opendash_i2c_serialize(&msg, tx_buf, &tx_len);
    if (od_ret != OPENDASH_OK) {
        return ESP_FAIL;
    }

    return channel_mgr_send_to_node(node, tx_buf, tx_len);
}