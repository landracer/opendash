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
#include "node_health.h"

#include "opendash_espnow.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"
#include "opendash_common.h"
#include "opendash_rollover.h"
#include "opendash_uart.h"
#include "opendash_layout.h"
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

/** Timer handle for periodic health evaluation */
static TimerHandle_t s_timeout_timer = NULL;

/* ────────────────────────────────────────────────────────────────────────────
 * Aux RX hook + engine DP cache (used by boost_client and similar consumers)
 *
 * The dispatcher latches engine-related DATA_RESPONSE values into s_engine_cache
 * as they fly through, so downstream consumers (e.g. the 10 Hz boost live-feed
 * pusher) can read a coherent snapshot without re-issuing requests.
 *
 * Out-of-band opcodes (currently the BOOST report range 0x90..0x94) bypass the
 * channel router entirely and are handed straight to s_aux_rx_cb when set.
 * ──────────────────────────────────────────────────────────────────────────── */

static espnow_master_rx_cb_t s_aux_rx_cb = NULL;

/* Last parachute STATUS echo per node (written by dispatcher, read by UI). */
static opendash_parachute_status_t s_para_status[OPENDASH_NODE_COUNT];
static bool                        s_para_status_valid[OPENDASH_NODE_COUNT];

/* ── Distributed rollover-detection fusion ─────────────────────────────────
 * Each detector node (RIGHT/POD1/POD2) broadcasts a VOTE (0x96). The center
 * fuses them: an autonomous deploy requires ALL detectors voting `rolling`
 * with a fresh (non-expired) vote, OR any single fresh manual-release vote.
 * Either path is still interlocked by the target MOS being armed + enabled +
 * channelled + online and by that MOS's per-config AUTO_DETECT opt-in flag. */
typedef struct {
    bool     valid;
    bool     rolling;
    bool     manual;
    float    roll_deg;
    float    roll_rate;
    uint32_t seq;
    int64_t  rx_us;
} rollover_vote_cache_t;
static rollover_vote_cache_t s_vote[OPENDASH_NODE_COUNT];
static bool                  s_auto_latched[OPENDASH_NODE_COUNT]; /* per-MOS fired latch */

static const opendash_node_t s_detectors[OPENDASH_ROLLOVER_DETECTOR_COUNT] =
    OPENDASH_ROLLOVER_DETECTORS;

/* Count detectors with a fresh `rolling` vote; set *manual if any detector is
 * holding a fresh manual-release vote. Stale/absent votes count as no vote. */
static int rollover_tally(bool *manual_out)
{
    const int64_t now       = esp_timer_get_time();
    const int64_t expiry_us = (int64_t)OPENDASH_ROLLOVER_VOTE_EXPIRY_MS * 1000;
    int  rolling = 0;
    bool manual  = false;
    for (int i = 0; i < OPENDASH_ROLLOVER_DETECTOR_COUNT; i++) {
        const rollover_vote_cache_t *v = &s_vote[s_detectors[i]];
        if (!v->valid) continue;
        if ((now - v->rx_us) > expiry_us) continue;   /* expired = no vote */
        if (v->rolling) rolling++;
        if (v->manual)  manual = true;
    }
    if (manual_out) *manual_out = manual;
    return rolling;
}

typedef struct {
    float    rpm;
    float    boost_cbar;     /* centi-bar gauge, mirrors opendash_boost_live_t */
    float    egt_c;
    float    afr;
    float    fuel_kpa;
    float    throttle_pct;
    float    gear;
    uint32_t last_update_ms;
} engine_cache_t;

static engine_cache_t s_engine_cache = {0};

static inline void engine_cache_update(uint16_t dp_id, float value)
{
    bool touched = true;
    switch (dp_id) {
        case OPENDASH_DP_RPM:            s_engine_cache.rpm          = value; break;
        case OPENDASH_DP_BOOST_PRESSURE: s_engine_cache.boost_cbar   = value; break;
        case OPENDASH_DP_EGT1:
        case OPENDASH_DP_EGT2:
        case OPENDASH_DP_EGT3:
        case OPENDASH_DP_EGT4:
            if (value > s_engine_cache.egt_c) s_engine_cache.egt_c = value;
            break;
        case OPENDASH_DP_AFR:            s_engine_cache.afr          = value; break;
        case OPENDASH_DP_FUEL_PRESSURE:  s_engine_cache.fuel_kpa     = value; break;
        case OPENDASH_DP_THROTTLE_POS:   s_engine_cache.throttle_pct = value; break;
        default: touched = false; break;
    }
    if (touched) {
        s_engine_cache.last_update_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Send Status Hook (Layer 2: MAC-layer ACK tracking)
 *
 * When center sends data TO a node, the ESP-NOW hardware ACKs the frame.
 * This callback feeds ACK/NACK results into the node_health system.
 * If we get an ACK, the node's radio is definitely alive — no timeout needed.
 * ──────────────────────────────────────────────────────────────────────────── */

static void master_send_status_cb(const uint8_t *mac, bool success)
{
    if (!mac) return;

    /* Look up which node this MAC belongs to */
    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        const channel_node_t *n = channel_mgr_get_node((opendash_node_t)i);
        if (n && n->mac_known && memcmp(n->mac, mac, 6) == 0) {
            if (success) {
                node_health_ack((opendash_node_t)i);
            } else {
                node_health_nack((opendash_node_t)i);
            }
            return;
        }
    }
}

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
    if (msg->length < 1) return;

    uint8_t node_type_raw = msg->payload[0];
    if (node_type_raw >= OPENDASH_NODE_COUNT) return;

    opendash_node_t node_type = (opendash_node_t)node_type_raw;

    /* Use default channel and capabilities from the static tables */
    uint8_t ch  = NODE_DEFAULT_CHANNEL[node_type];
    uint8_t cap = NODE_CAPABILITIES[node_type];

    channel_mgr_register_node(node_type, mac, ch, cap);

    /* Register with node health system (persists MAC to NVS) */
    node_health_register_mac(node_type, mac);

    ESP_LOGI(TAG, "Auto-registered %s @ " MACSTR " (ch=%d cap=0x%02X)",
             NODE_NAMES[node_type], MAC2STR(mac), ch, cap);
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

        /* DIAG: surface any inbound parachute echo (0x95) BEFORE sender
         * resolution, so a one-way link or unknown-MAC drop is visible. */
        if (msg.cmd == OPENDASH_CMD_PARACHUTE_STATUS) {
            ESP_LOGI(TAG, "RX 0x95 echo frame from " MACSTR " (len=%d) sender=%d",
                     MAC2STR(evt.src_mac), msg.length, sender);
        }

        /* Auto-register unknown nodes via multiple methods */
        if (sender == OPENDASH_NODE_COUNT) {
            /* Method 1: STATUS_REPORT / ANNOUNCE contains node_type in payload */
            if (msg.cmd == OPENDASH_CMD_STATUS_REPORT ||
                msg.cmd == CHANNEL_MSG_ANNOUNCE) {
                auto_register_node(evt.src_mac, &msg);
                sender = identify_sender(evt.src_mac);
            }

            /* Method 2: Check NVS registry for this MAC (known from prev boot) */
            if (sender == OPENDASH_NODE_COUNT) {
                sender = node_health_find_by_mac(evt.src_mac);
                if (sender != OPENDASH_NODE_COUNT) {
                    /* Known MAC from NVS — re-register in channel_mgr */
                    uint8_t ch  = NODE_DEFAULT_CHANNEL[sender];
                    uint8_t cap = NODE_CAPABILITIES[sender];
                    channel_mgr_register_node(sender, evt.src_mac, ch, cap);
                    ESP_LOGI(TAG, "NVS MAC match: %s re-registered from data frame",
                             NODE_NAMES[sender]);
                }
            }

            /* Method 3: Identify node by message content (GPS TIME_SYNC, GPS data points) */
            if (sender == OPENDASH_NODE_COUNT) {
                opendash_node_t inferred = OPENDASH_NODE_COUNT;

                /* TIME_SYNC (cmd=0x07, subcmd=0x05) is unique to GPS */
                if (msg.cmd == OPENDASH_CMD_SYSTEM && msg.length >= 1 &&
                    msg.payload[0] == OPENDASH_SUBCMD_TIME_SYNC) {
                    inferred = OPENDASH_NODE_GPS;
                }

                /* DATA_RESPONSE with GPS-unique data point IDs */
                if (msg.cmd == OPENDASH_CMD_DATA_RESPONSE && msg.length >= 2) {
                    uint16_t dp_id = ((uint16_t)msg.payload[0] << 8) | msg.payload[1];
                    if (dp_id == OPENDASH_DP_GPS_SPEED || dp_id == OPENDASH_DP_GPS_HEADING ||
                        dp_id == OPENDASH_DP_LATITUDE || dp_id == OPENDASH_DP_LONGITUDE ||
                        dp_id == OPENDASH_DP_ALTITUDE || dp_id == OPENDASH_DP_SAT_COUNT ||
                        dp_id == OPENDASH_DP_HDOP || dp_id == OPENDASH_DP_GPS_FIX ||
                        dp_id == OPENDASH_DP_GFORCE_LAT || dp_id == OPENDASH_DP_GFORCE_LONG ||
                        dp_id == OPENDASH_DP_GFORCE_VERT) {
                        inferred = OPENDASH_NODE_GPS;
                    }
                }

                if (inferred != OPENDASH_NODE_COUNT) {
                    uint8_t ch  = NODE_DEFAULT_CHANNEL[inferred];
                    uint8_t cap = NODE_CAPABILITIES[inferred];
                    channel_mgr_register_node(inferred, evt.src_mac, ch, cap);
                    node_health_register_mac(inferred, evt.src_mac);
                    ESP_LOGI(TAG, "Auto-identified %s from message content (cmd=0x%02X)",
                             NODE_NAMES[inferred], msg.cmd);
                    sender = inferred;
                }
            }

            if (sender == OPENDASH_NODE_COUNT) {
                ESP_LOGD(TAG, "UNKNOWN MAC " MACSTR " cmd=0x%02X len=%d — not registered",
                         MAC2STR(evt.src_mac), msg.cmd, evt.len);
                continue;
            }
        }

        /* Update last_seen + RSSI (marks node online implicitly) */
        /* Access the node registry through the channel manager */
        channel_node_t *n = (channel_node_t *)channel_mgr_get_node(sender);
        if (n) {
            n->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
            n->last_rssi = (int8_t)evt.rssi;
            n->online = true;
        }

        /* Feed the node health monitor (Layer 1: data flow) */
        node_health_rx(sender, (int8_t)evt.rssi);

        /* ── Capture STATUS_REPORT flag byte so the UI can render badges
         *    (BLE-OTA in particular). Payload layout from slaves is
         *    [node_id:1][flags_lo:1][flags_hi:1] — we only consume flags_lo
         *    today, the hi byte is reserved. */
        if (msg.cmd == OPENDASH_CMD_STATUS_REPORT && msg.length >= 2) {
            node_health_set_status_flags(sender, msg.payload[1]);
        }

        /* ── Parachute STATUS echo (S→M 0x95): cache per node for the UI. */
        if (msg.cmd == OPENDASH_CMD_PARACHUTE_STATUS) {
            if (msg.length >= sizeof(opendash_parachute_status_t) &&
                sender < OPENDASH_NODE_COUNT) {
                memcpy(&s_para_status[sender], msg.payload,
                       sizeof(opendash_parachute_status_t));
                s_para_status_valid[sender] = true;
                ESP_LOGI(TAG, "RX parachute echo from %s (len=%d)",
                         NODE_NAMES[sender], msg.length);
            } else {
                ESP_LOGW(TAG, "Parachute echo DROPPED: sender=%d len=%d need=%d",
                         sender, msg.length,
                         (int)sizeof(opendash_parachute_status_t));
            }
            continue;
        }

        /* ── Rollover detector VOTE (S→M 0x96, broadcast): cache per node for
         *    the fusion reconciler + the deploy-panel "X/3" indicator. */
        if (msg.cmd == OPENDASH_CMD_PARACHUTE_VOTE) {
            if (msg.length >= sizeof(opendash_parachute_vote_t) &&
                sender < OPENDASH_NODE_COUNT) {
                opendash_parachute_vote_t v;
                memcpy(&v, msg.payload, sizeof(v));
                s_vote[sender].valid     = true;
                s_vote[sender].rolling   = v.rolling != 0;
                s_vote[sender].manual    = v.manual != 0;
                s_vote[sender].roll_deg  = v.roll_deg;
                s_vote[sender].roll_rate = v.roll_rate;
                s_vote[sender].seq       = v.seq;
                s_vote[sender].rx_us     = esp_timer_get_time();
                ESP_LOGW(TAG, "VOTE %s roll=%d manual=%d |%.0f| rate=%.0f seq=%lu",
                         NODE_NAMES[sender], v.rolling, v.manual,
                         (double)v.roll_deg, (double)v.roll_rate,
                         (unsigned long)v.seq);
            }
            continue;
        }

        /* ── Aux RX intercept: opcodes outside the standard router (BOOST 0x90-0x94)
         *    are handed straight to the registered consumer and skip the channel
         *    queue entirely.  Keep this above the channel routing block. */
        if (msg.cmd >= OPENDASH_CMD_BOOST_TELEMETRY &&
            msg.cmd <= OPENDASH_CMD_BOOST_THROTTLE_REPORT) {
            if (s_aux_rx_cb) {
                s_aux_rx_cb(&evt, &msg);
            }
            continue;
        }

        /* ── Passive snoop: latch engine values into the shared cache so the
         *    boost live-pusher (and similar) can read a coherent snapshot
         *    without re-issuing requests. */
        if (msg.cmd == OPENDASH_CMD_DATA_RESPONSE && msg.length >= 6) {
            uint16_t dp_id = ((uint16_t)msg.payload[0] << 8) | msg.payload[1];
            float value;
            memcpy(&value, &msg.payload[2], sizeof(float));
            engine_cache_update(dp_id, value);
        }

        /* Route to the correct channel queue */
        uint8_t ch = NODE_DEFAULT_CHANNEL[sender];
        /* Override: relay/MOS commands go to CONTROL */
        if (msg.cmd == OPENDASH_CMD_SET_RELAY) {
            ch = CHANNEL_CONTROL;
        }

        channel_inbound_msg_t inbound;
        memcpy(inbound.src_mac, evt.src_mac, 6);
        /* Defensive bounds: never memcpy past the queue item buffer. */
        uint16_t copy_len = (evt.len > CHANNEL_QUEUE_ITEM_SIZE)
                                ? CHANNEL_QUEUE_ITEM_SIZE
                                : (uint16_t)evt.len;
        memcpy(inbound.data, evt.data, copy_len);
        inbound.len = copy_len;
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

            if (msg.cmd == OPENDASH_CMD_DATA_RESPONSE && msg.length >= 6) {
                /* Extract data point: [dp_id_hi, dp_id_lo, float32] */
                uint16_t dp_id = ((uint16_t)msg.payload[0] << 8) | msg.payload[1];
                float value;
                memcpy(&value, &msg.payload[2], sizeof(float));

                /* ALWAYS push to UI — this is real-time safety-critical data.
                 * Never filter/deduplicate on the display path. */
                if (display_lvgl_lock(5)) {
                    ui_manager_update_value(dp_id, value);
                    display_lvgl_unlock();
                }

                /* Forward ONLY data that gauge pods actually need.
                 * LEFT has its own MD UART — it only needs GPS data from center.
                 * RIGHT needs GPS + whatever its current gauge page shows.
                 * This prevents wireless bandwidth flooding. */
                if (dp_id == OPENDASH_DP_GPS_SPEED ||
                    dp_id == OPENDASH_DP_GPS_FIX ||
                    dp_id == OPENDASH_DP_GPS_HEADING) {
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
    ESP_LOGI(TAG, "CH1 MEDIUM worker started (≤%dms latency, no dedup)",
             CHANNEL_CRITICAL_INTERVAL_MS);

    channel_inbound_msg_t inbound;
    uint32_t ch1_dp_processed = 0;
    uint64_t ch1_last_log_us = esp_timer_get_time();

    while (1) {
        while (channel_mgr_recv(CHANNEL_MEDIUM, &inbound, 0)) {
            opendash_i2c_msg_t msg;
            if (opendash_i2c_deserialize(inbound.data, inbound.len, &msg) != OPENDASH_OK) {
                continue;
            }

            if (msg.cmd == OPENDASH_CMD_DATA_RESPONSE && msg.length >= 6) {
                uint16_t dp_id = ((uint16_t)msg.payload[0] << 8) | msg.payload[1];
                float value;
                memcpy(&value, &msg.payload[2], sizeof(float));

                /* ALWAYS push to UI immediately — real-time, no filtering */
                if (display_lvgl_lock(5)) {
                    ui_manager_update_value(dp_id, value);
                    display_lvgl_unlock();
                }
                ch1_dp_processed++;

                /* Forward single datapoint to RIGHT (fail-fast if offline) */
                uint8_t fwd_payload[6];
                fwd_payload[0] = (dp_id >> 8) & 0xFF;
                fwd_payload[1] = dp_id & 0xFF;
                memcpy(&fwd_payload[2], &value, sizeof(float));
                opendash_i2c_msg_t fwd;
                opendash_i2c_build_msg(&fwd, OPENDASH_CMD_SET_DATA_POINT,
                                        fwd_payload, sizeof(fwd_payload));
                uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
                uint16_t tx_len = 0;
                if (opendash_i2c_serialize(&fwd, tx_buf, &tx_len) == OPENDASH_OK) {
                    channel_mgr_send_to_node(OPENDASH_NODE_RIGHT, tx_buf, tx_len);
                }
            }
            else if (msg.cmd == OPENDASH_CMD_DATA_BATCH && msg.length >= 1) {
                /* Batched telemetry from a slave: [count:1][dp_id:2][value:4]×N
                 * One UART frame from LEFT arrives as a single packet here. */
                uint8_t count = msg.payload[0];
                uint16_t expected = 1 + (uint16_t)count * 6;
                if (msg.length < expected || count == 0) {
                    continue;
                }

                /* Fan out every entry to local UI */
                if (display_lvgl_lock(5)) {
                    const uint8_t *e = &msg.payload[1];
                    for (uint8_t i = 0; i < count; i++) {
                        uint16_t dp_id = ((uint16_t)e[0] << 8) | e[1];
                        float value;
                        memcpy(&value, &e[2], sizeof(float));
                        ui_manager_update_value(dp_id, value);
                        e += 6;
                    }
                    display_lvgl_unlock();
                }
                ch1_dp_processed += count;

                /* Re-batch and forward to RIGHT in ONE packet (fail-fast if offline) */
                opendash_i2c_msg_t fwd;
                opendash_i2c_build_msg(&fwd, OPENDASH_CMD_SET_DATA_BATCH,
                                        msg.payload, msg.length);
                uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
                uint16_t tx_len = 0;
                if (opendash_i2c_serialize(&fwd, tx_buf, &tx_len) == OPENDASH_OK) {
                    channel_mgr_send_to_node(OPENDASH_NODE_RIGHT, tx_buf, tx_len);
                }
            }
        }  /* end inner drain loop */

        /* Heavy debug telemetry: prove CH1 worker is alive even when idle.
         * Logs every 1 s regardless of whether messages are arriving. */
        uint64_t now_us = esp_timer_get_time();
        if ((now_us - ch1_last_log_us) >= 1000000ULL) {
            channel_stats_t st = {0};
            if (channel_mgr_get_stats(CHANNEL_MEDIUM, &st) == ESP_OK) {
                ESP_LOGI(TAG,
                         "CH1 flow: dp/s=%lu rx=%lu tx=%lu err=%lu qHW=%lu",
                         (unsigned long)ch1_dp_processed,
                         (unsigned long)st.msgs_received,
                         (unsigned long)st.msgs_sent,
                         (unsigned long)st.errors,
                         (unsigned long)st.queue_high_water);
            } else {
                ESP_LOGI(TAG, "CH1 flow: dp/s=%lu", (unsigned long)ch1_dp_processed);
            }
            ch1_dp_processed = 0;
            ch1_last_log_us = now_us;
        }

        vTaskDelay(pdMS_TO_TICKS(CHANNEL_CRITICAL_INTERVAL_MS)); /* 10ms — real-time */
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

            if (msg.cmd == OPENDASH_CMD_DATA_RESPONSE && msg.length >= 6) {
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

/* ── Rollover fusion reconciler ────────────────────────────────────────────
 * Runs at OPENDASH_ROLLOVER_FUSE_PERIOD_MS cadence (driven from the control
 * task). When the detectors reach quorum (ALL voting `rolling`) OR any
 * manual-release vote is fresh, command DEPLOY on every MOS that is
 * INDEPENDENTLY eligible:
 *   online && status echoed && cfg.enabled && (flags & AUTO_DETECT) &&
 *   channel_mask != 0 && armed.
 * A per-MOS latch prevents command spam; it clears once the trigger relaxes
 * (no rolling, no manual) so the system re-arms for a subsequent event. This
 * path is independent of which UI screen is showing. The center touchscreen
 * manual DEPLOY button (deploy_fire_cb) remains the always-available override
 * and is unaffected by the AUTO_DETECT gate. */
static void rollover_fusion_eval(void)
{
    static int64_t last_us = 0;
    const int64_t now = esp_timer_get_time();
    if ((now - last_us) < (int64_t)OPENDASH_ROLLOVER_FUSE_PERIOD_MS * 1000) return;
    last_us = now;

    bool manual  = false;
    int  rolling = rollover_tally(&manual);
    bool trigger = manual || (rolling >= OPENDASH_ROLLOVER_DETECTOR_COUNT);

    static const opendash_node_t mos_nodes[] = {
        OPENDASH_NODE_MOS_4CH_A, OPENDASH_NODE_MOS_4CH_B,
    };
    for (size_t i = 0; i < sizeof(mos_nodes) / sizeof(mos_nodes[0]); i++) {
        opendash_node_t mos = mos_nodes[i];

        if (!trigger) { s_auto_latched[mos] = false; continue; }
        if (s_auto_latched[mos]) continue;   /* already fired this event */

        if (!espnow_master_node_online(mos)) continue;
        if (!s_para_status_valid[mos])       continue;
        const opendash_parachute_status_t *st = &s_para_status[mos];
        if (!st->armed)                      continue;
        if (!st->cfg.enabled)                continue;
        if (!(st->cfg.flags & OPENDASH_PARACHUTE_FLAG_AUTO_DETECT)) continue;
        if (st->cfg.channel_mask == 0)       continue;

        espnow_master_send_parachute_deploy(mos);
        espnow_master_send_parachute_pull(mos);
        s_auto_latched[mos] = true;
        ESP_LOGW(TAG, "AUTO-DEPLOY %s: %s (rolling=%d/%d) ch=0x%X",
                 NODE_NAMES[mos],
                 manual ? "MANUAL RELEASE" : "ROLLOVER QUORUM",
                 rolling, OPENDASH_ROLLOVER_DETECTOR_COUNT,
                 st->cfg.channel_mask);
    }
}

/* Deploy-panel indicator: fresh `rolling` detector count (0..total); sets
 * *manual if any detector holds a fresh manual-release vote. */
int espnow_master_rollover_status(bool *manual, int *detectors_total)
{
    if (detectors_total) *detectors_total = OPENDASH_ROLLOVER_DETECTOR_COUNT;
    return rollover_tally(manual);
}

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

        /* Distributed rollover fusion — autonomous parachute deploy decision.
         * Internally rate-limited to OPENDASH_ROLLOVER_FUSE_PERIOD_MS. */
        rollover_fusion_eval();

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

    /* Legacy timeout check (safety net — node_health catches faster) */
    channel_mgr_check_timeouts();

    /* ── Node Health Evaluation (primary detection system) ──────────
     * This runs the frequency-based state machine for every node.
     * It compares actual receive rate vs expected, checks ACK status,
     * and transitions nodes through ONLINE → DEGRADED → OFFLINE.
     * Much faster and more reliable than the old data-absence timeout. */
    node_health_evaluate();

    /* Build status snapshot from node_health (replaces old channel_mgr scan) */
    s_node_status.left_online       = node_health_is_alive(OPENDASH_NODE_LEFT);
    s_node_status.right_online      = node_health_is_alive(OPENDASH_NODE_RIGHT);
    s_node_status.gps_online        = node_health_is_alive(OPENDASH_NODE_GPS);
    s_node_status.bms_online        = node_health_is_alive(OPENDASH_NODE_BMS);
    s_node_status.pod1_online       = node_health_is_alive(OPENDASH_NODE_POD1);
    s_node_status.pod2_online       = node_health_is_alive(OPENDASH_NODE_POD2);
    s_node_status.relay_4ch_online  = node_health_is_alive(OPENDASH_NODE_RELAY_4CH);
    s_node_status.relay_8ch_a_online = node_health_is_alive(OPENDASH_NODE_RELAY_8CH_A);
    s_node_status.relay_8ch_b_online = node_health_is_alive(OPENDASH_NODE_RELAY_8CH_B);
    s_node_status.mos_4ch_a_online  = node_health_is_alive(OPENDASH_NODE_MOS_4CH_A);
    s_node_status.mos_4ch_b_online  = node_health_is_alive(OPENDASH_NODE_MOS_4CH_B);

    /* Push to config screen if visible */
    if (display_lvgl_lock(5)) {
        ui_manager_update_config_node_status(&s_node_status);
        ui_manager_refresh_ota_banner();
        display_lvgl_unlock();
    }

    /* Periodic stats log (every timer tick = 200ms) */
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

    /* Init node health monitor (NVS-backed, frequency-aware) */
    ret = node_health_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Node health init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Pre-register all NVS-known MACs in channel_mgr for instant data routing.
     * Without this, first packets from known nodes need Method 2 NVS lookup.
     * With this, identify_sender() succeeds immediately on first packet. */
    for (int i = 1; i < OPENDASH_NODE_COUNT; i++) {  /* skip CENTER (0) */
        const node_health_record_t *rec = node_health_get_record((opendash_node_t)i);
        if (rec && rec->mac_known) {
            uint8_t ch  = NODE_DEFAULT_CHANNEL[i];
            uint8_t cap = NODE_CAPABILITIES[i];
            channel_mgr_register_node((opendash_node_t)i, rec->mac, ch, cap);
        }
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

    /* Health evaluation timer: 2 Hz (500ms) — evaluate() checks if window elapsed internally */
    s_timeout_timer = xTimerCreate("node_health", pdMS_TO_TICKS(500),
                                    pdTRUE, NULL, timeout_timer_callback);
    if (s_timeout_timer) {
        xTimerStart(s_timeout_timer, 0);
    }

    /* Register MAC-layer ACK hook for Layer 2 health detection */
    opendash_espnow_set_send_status_cb(master_send_status_cb);

    /* Firm handshake: send SET_DATA_POINT to ALL display pods so they discover
     * center's MAC instantly on power-on. Pods gate data forwarding until they
     * know our MAC — this kickstarts them. 3× for wireless reliability. */
    for (int i = 0; i < 3; i++) {
        uint8_t hello_payload[6] = {0};
        hello_payload[0] = (OPENDASH_DP_GPS_SPEED >> 8) & 0xFF;
        hello_payload[1] = OPENDASH_DP_GPS_SPEED & 0xFF;
        /* value=0.0f (already zeroed) */
        opendash_i2c_msg_t hello;
        opendash_i2c_build_msg(&hello, OPENDASH_CMD_SET_DATA_POINT,
                                hello_payload, sizeof(hello_payload));
        uint8_t tx[OPENDASH_ESPNOW_MAX_DATA]; uint16_t tl = 0;
        if (opendash_i2c_serialize(&hello, tx, &tl) == OPENDASH_OK) {
            channel_mgr_send_to_node(OPENDASH_NODE_LEFT, tx, tl);
            channel_mgr_send_to_node(OPENDASH_NODE_RIGHT, tx, tl);
            channel_mgr_send_to_node(OPENDASH_NODE_POD1, tx, tl);
            channel_mgr_send_to_node(OPENDASH_NODE_POD2, tx, tl);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGI(TAG, "Handshake sent to all display pods (3×)");

    ESP_LOGI(TAG, "Channel-based master running — 4 channels, zero polling, "
             "node health active (freq-based + ACK + NVS)");
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
    return channel_mgr_force_send_to_node(node, tx_buf, tx_len,
                                          CHANNEL_CONTROL_MAX_RETRIES);
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

    if (subcmd == OPENDASH_SUBCMD_ENTER_BT_OTA) {
        /* Silence ESP-NOW fan-out to ALL slaves for the BLE OTA window.
         * Even traffic addressed to other slaves consumes WiFi ch 1 airtime
         * (same 2.4 GHz band) and causes supervision-timeout disconnects on
         * the slave doing BLE OTA. Pause everyone for the duration.
         * 6 min ≥ BT_OTA_ADV_TIMEOUT_MS (5 min) + push + reboot. */
        for (int n = OPENDASH_NODE_LEFT; n < OPENDASH_NODE_COUNT; n++) {
            channel_mgr_pause_node_traffic((opendash_node_t)n, 360000);
        }
    }

    return channel_mgr_force_send_to_node(node, tx_buf, tx_len,
                                          CHANNEL_CONTROL_MAX_RETRIES);
}

esp_err_t espnow_master_send_screen_layout(opendash_node_t node,
                                           const screen_layout_v1_t *layout)
{
    if (!layout) {
        return ESP_ERR_INVALID_ARG;
    }
    if (layout->version != OPENDASH_LAYOUT_VERSION) {
        ESP_LOGW(TAG, "Refusing to send layout with version %u",
                 (unsigned)layout->version);
        return ESP_ERR_INVALID_VERSION;
    }
    if (layout->slot_count > OPENDASH_LAYOUT_MAX_SLOTS) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Pack to wire format. */
    uint8_t payload[OPENDASH_LAYOUT_WIRE_MAX];
    size_t payload_len = 0;
    if (opendash_layout_serialize(layout, payload, sizeof(payload),
                                  &payload_len) != 0) {
        return ESP_FAIL;
    }

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_SET_SCREEN_LAYOUT,
                           payload, (uint8_t)payload_len);

    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t tx_len = 0;
    if (opendash_i2c_serialize(&msg, tx_buf, &tx_len) != OPENDASH_OK) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SET_SCREEN_LAYOUT → node=%d mode=%u slots=%u arc=0x%04X",
             (int)node, (unsigned)layout->mode,
             (unsigned)layout->slot_count, (unsigned)layout->arc_dp_id);

    return channel_mgr_send_to_node(node, tx_buf, tx_len);
}

/* ────────────────────────────────────────────────────────────────────────────
 * OBD Command Relay (Center → Left pod)
 *
 * These functions are still needed by ui_manager.c for the OBD config
 * screen DTC clear/read/VIN buttons.
 * ──────────────────────────────────────────────────────────────────────────── */

/** DTC data from left pod (populated when DTC_REPORT arrives) */
static char s_dtc_codes[16][6];
static uint8_t s_dtc_count = 0;
static bool s_dtc_valid = false;

esp_err_t espnow_master_send_obd_command(uint8_t obd_cmd)
{
    uint8_t payload[1] = { obd_cmd };

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_OBD_COMMAND,
                            payload, sizeof(payload));

    uint8_t tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t tx_len = 0;
    opendash_err_t od_ret = opendash_i2c_serialize(&msg, tx_buf, &tx_len);
    if (od_ret != OPENDASH_OK) {
        return ESP_FAIL;
    }

    return channel_mgr_send_to_node(OPENDASH_NODE_LEFT, tx_buf, tx_len);
}

void espnow_master_get_dtc_data(char codes[][6], uint8_t *count, bool *valid)
{
    if (count) *count = s_dtc_count;
    if (valid) *valid = s_dtc_valid;
    if (codes && s_dtc_count > 0) {
        memcpy(codes, s_dtc_codes, s_dtc_count * 6);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Additive helpers (see espnow_master.h)
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t espnow_master_send_raw(opendash_node_t node, uint8_t cmd,
                                  const void *payload, uint16_t length)
{
    if (node >= OPENDASH_NODE_COUNT) return ESP_ERR_INVALID_ARG;
    if (length > (OPENDASH_ESPNOW_MAX_DATA - 4)) return ESP_ERR_INVALID_SIZE;
    if (length > 0 && payload == NULL)            return ESP_ERR_INVALID_ARG;

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, cmd, (const uint8_t *)payload, length);

    uint8_t  tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t tx_len = 0;
    if (opendash_i2c_serialize(&msg, tx_buf, &tx_len) != OPENDASH_OK) {
        return ESP_FAIL;
    }

    return channel_mgr_send_to_node(node, tx_buf, tx_len);
}

bool espnow_master_node_online(opendash_node_t node)
{
    if (node >= OPENDASH_NODE_COUNT) return false;
    const channel_node_t *n = channel_mgr_get_node(node);
    return (n && n->online);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Parachute / Deployment System senders + status cache accessor.
 * ──────────────────────────────────────────────────────────────────────────── */

/* Parachute commands are user-initiated, safety-critical control frames. They
 * must reach the MOS even if the regular data plane has put the peer in a
 * quarantine/pause back-off window (which would otherwise fast-fail the send
 * with ESP_ERR_TIMEOUT and force the user to mash REFRESH). Route them through
 * the force-send path, which ignores the dead-window checks and retries. */
static esp_err_t espnow_master_send_raw_force(opendash_node_t node, uint8_t cmd,
                                              const void *payload, uint16_t length,
                                              uint8_t max_retries)
{
    if (node >= OPENDASH_NODE_COUNT) return ESP_ERR_INVALID_ARG;
    if (length > (OPENDASH_ESPNOW_MAX_DATA - 4)) return ESP_ERR_INVALID_SIZE;
    if (length > 0 && payload == NULL)            return ESP_ERR_INVALID_ARG;

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, cmd, (const uint8_t *)payload, length);

    uint8_t  tx_buf[OPENDASH_ESPNOW_MAX_DATA];
    uint16_t tx_len = 0;
    if (opendash_i2c_serialize(&msg, tx_buf, &tx_len) != OPENDASH_OK) {
        return ESP_FAIL;
    }
    return channel_mgr_force_send_to_node(node, tx_buf, tx_len, max_retries);
}

esp_err_t espnow_master_send_parachute_config(opendash_node_t node,
                                               const opendash_parachute_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    esp_err_t err = espnow_master_send_raw_force(node, OPENDASH_CMD_PARACHUTE_SET_CONFIG,
                                                 cfg, sizeof(*cfg), 3);
    ESP_LOGI(TAG, "TX parachute config -> node %d (en=%d mask=0x%X) err=0x%x",
             node, cfg->enabled, cfg->channel_mask, err);
    return err;
}

esp_err_t espnow_master_send_parachute_arm(opendash_node_t node, bool armed)
{
    uint8_t b = armed ? 1 : 0;
    return espnow_master_send_raw_force(node, OPENDASH_CMD_PARACHUTE_SET_ARM, &b, 1, 3);
}

esp_err_t espnow_master_send_parachute_pull(opendash_node_t node)
{
    esp_err_t err = espnow_master_send_raw_force(node, OPENDASH_CMD_PARACHUTE_PULL_ALL, NULL, 0, 3);
    ESP_LOGI(TAG, "TX parachute pull -> node %d err=0x%x", node, err);
    return err;
}

esp_err_t espnow_master_send_parachute_deploy(opendash_node_t node)
{
    esp_err_t err = espnow_master_send_raw_force(node, OPENDASH_CMD_PARACHUTE_DEPLOY, NULL, 0, 3);
    ESP_LOGW(TAG, "TX parachute DEPLOY -> node %d err=0x%x", node, err);
    return err;
}

esp_err_t espnow_master_send_parachute_calibrate(opendash_node_t node)
{
    esp_err_t err = espnow_master_send_raw_force(node, OPENDASH_CMD_PARACHUTE_CALIBRATE, NULL, 0, 3);
    ESP_LOGI(TAG, "TX parachute CALIBRATE -> node %d err=0x%x", node, err);
    return err;
}

bool espnow_master_get_parachute_status(opendash_node_t node,
                                         opendash_parachute_status_t *out)
{
    if (node >= OPENDASH_NODE_COUNT || !out) return false;
    if (!s_para_status_valid[node]) return false;
    *out = s_para_status[node];
    return true;
}

uint8_t espnow_master_parachute_reserved_mask(opendash_node_t node)
{
    if (node >= OPENDASH_NODE_COUNT) return 0;
    if (!s_para_status_valid[node])  return 0;
    return s_para_status[node].cfg.channel_mask & 0x0Fu;
}

void espnow_master_set_aux_rx_callback(espnow_master_rx_cb_t cb)
{
    s_aux_rx_cb = cb;
}

void espnow_master_snapshot_engine(float *rpm, float *boost_cbar, float *egt_c,
                                    float *afr, float *fuel_kpa,
                                    float *throttle_pct, float *gear,
                                    uint32_t *last_update_ms)
{
    if (rpm)            *rpm            = s_engine_cache.rpm;
    if (boost_cbar)     *boost_cbar     = s_engine_cache.boost_cbar;
    if (egt_c)          *egt_c          = s_engine_cache.egt_c;
    if (afr)            *afr            = s_engine_cache.afr;
    if (fuel_kpa)       *fuel_kpa       = s_engine_cache.fuel_kpa;
    if (throttle_pct)   *throttle_pct   = s_engine_cache.throttle_pct;
    if (gear)           *gear           = s_engine_cache.gear;
    if (last_update_ms) *last_update_ms = s_engine_cache.last_update_ms;
}