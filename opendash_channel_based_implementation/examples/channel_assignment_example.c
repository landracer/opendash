/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file channel_assignment_example.c
 * @brief Working example: how to use the channel-based ESP-NOW API
 *
 * Copy-paste these snippets into your app_main() or integration tests.
 * They demonstrate:
 *   1. Startup sequence  (init → start → done)
 *   2. Sending data points
 *   3. Sending relay commands from LVGL touch callbacks
 *   4. Querying node status
 *   5. Dumping channel statistics
 *
 * ARCHITECTURE RULE:  NO POLLING / NO PINGING.
 */

#include "espnow_master.h"
#include "channel_management.h"
#include "channel_config.h"
#include "node_definitions.h"
#include "opendash_common.h"

#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "channel_example";

/* ────────────────────────────────────────────────────────────────────────────
 * Example 1: Startup Sequence
 *
 * Call from app_main().  Two calls — init then start.
 * Nodes self-register via ANNOUNCE on first contact.  No discovery loop.
 * ──────────────────────────────────────────────────────────────────────────── */

void example_startup(void)
{
    ESP_LOGI(TAG, "=== Channel-based startup ===");

    /* Step 1: initialise transport + channel manager + relay queue */
    ESP_ERROR_CHECK(espnow_master_init());

    /* Step 2: spawn dispatcher + 4 channel workers + timeout timer */
    ESP_ERROR_CHECK(espnow_master_start());

    ESP_LOGI(TAG, "Master running — awaiting node announcements");
}

/* ────────────────────────────────────────────────────────────────────────────
 * Example 2: Sending a Data Point to a Gauge Pod
 *
 * Center pushes DP 0x0101 (RPM) to LEFT pod.
 * Delta check is built in — if the value hasn't changed, the send is suppressed.
 * ──────────────────────────────────────────────────────────────────────────── */

void example_send_to_gauge(void)
{
    float rpm = 4200.0f;
    esp_err_t ret = espnow_master_send_data_point(OPENDASH_NODE_LEFT,
                                                    0x0101, rpm);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send RPM to LEFT: %s", esp_err_to_name(ret));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Example 3: Relay Toggle from Touch UI
 *
 * Called from an LVGL button callback.
 * Uses the public API — the control channel task handles send + retry.
 * ──────────────────────────────────────────────────────────────────────────── */

void example_relay_toggle(void)
{
    /* Turn ON channel 2 on the 4-channel relay board */
    esp_err_t ret = espnow_master_send_relay_command(
        OPENDASH_NODE_RELAY_4CH,
        2,      /* relay channel */
        1,      /* ON */
        0       /* pwm_duty (unused for relay, set 0) */
    );
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Relay cmd failed: %s", esp_err_to_name(ret));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Example 4: Query Node Online Status
 *
 * The master maintains a lock-free status snapshot updated at 1 Hz
 * by the timeout timer callback.  Safe to read from LVGL context.
 * ──────────────────────────────────────────────────────────────────────────── */

void example_query_status(void)
{
    espnow_master_node_status_t st;
    espnow_master_get_status(&st);

    ESP_LOGI(TAG, "GPS=%s  BMS=%s  LEFT=%s  RIGHT=%s",
             st.gps_online  ? "ON" : "off",
             st.bms_online  ? "ON" : "off",
             st.left_online ? "ON" : "off",
             st.right_online? "ON" : "off");

    ESP_LOGI(TAG, "RELAY_4CH=%s  MOS_4CH_A=%s",
             st.relay_4ch_online ? "ON" : "off",
             st.mos_4ch_a_online ? "ON" : "off");
}

/* ────────────────────────────────────────────────────────────────────────────
 * Example 5: Dump Channel Statistics
 *
 * Call periodically (or from a debug command) to see per-channel traffic.
 * ──────────────────────────────────────────────────────────────────────────── */

void example_dump_stats(void)
{
    for (int ch = 0; ch < CHANNEL_COUNT; ch++) {
        channel_stats_t st;
        channel_mgr_get_stats(ch, &st);

        ESP_LOGI(TAG, "CH%d %-8s | active=%d | interval=%dms | "
                 "rx=%lu tx=%lu drop=%lu retry=%lu | qHWM=%d",
                 ch, CHANNEL_NAMES[ch],
                 (int)st.active,
                 (int)st.interval_ms,
                 (unsigned long)st.rx_count,
                 (unsigned long)st.tx_count,
                 (unsigned long)st.dropped_count,
                 (unsigned long)st.retry_count,
                 (int)st.queue_hwm);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Example 6: Node-to-Channel Map (informational log)
 * ──────────────────────────────────────────────────────────────────────────── */

void example_log_channel_map(void)
{
    ESP_LOGI(TAG, "=== Node → Channel Map ===");
    ESP_LOGI(TAG, "%-14s  %-10s  Capabilities", "Node", "Channel");
    ESP_LOGI(TAG, "──────────────  ──────────  ────────────");

    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        if (i == OPENDASH_NODE_CENTER) continue; /* Center is the master */
        uint8_t ch  = NODE_DEFAULT_CHANNEL[i];
        uint8_t cap = NODE_CAPABILITIES[i];
        ESP_LOGI(TAG, "%-14s  %-10s  0x%02X",
                 NODE_NAMES[i],
                 CHANNEL_NAMES[ch],
                 cap);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Example 7: System Command (reboot a node)
 * ──────────────────────────────────────────────────────────────────────────── */

void example_reboot_node(opendash_node_t node)
{
    ESP_LOGW(TAG, "Sending REBOOT to %s", NODE_NAMES[node]);
    espnow_master_send_system_subcmd(node, 0x01 /* SUBCMD_REBOOT */);
}