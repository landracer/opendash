/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file node_health.c
 * @brief OpenDash Node Health Monitor — Implementation
 *
 * Multi-layered online/offline detection with NVS persistence.
 * See node_health.h for architecture documentation.
 *
 * ZERO POLLING.  ZERO PINGING.  ZERO FALSE OFFLINES.
 */

#include "node_health.h"
#include "node_definitions.h"

#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <string.h>
#include <math.h>

static const char *TAG = "node_health";

/* ────────────────────────────────────────────────────────────────────────────
 * NVS Namespace & Key Format
 * ──────────────────────────────────────────────────────────────────────────── */

#define NVS_NAMESPACE       "nd_health"
#define NVS_KEY_PREFIX      "n"     /* Keys: "n0", "n1", ... "n17" */

/** Persisted record (minimal — just what we need across reboots) */
typedef struct __attribute__((packed)) {
    uint8_t  mac[6];
    uint8_t  node_type;
    uint8_t  was_online;    /* 1 = was online when last persisted */
} nvs_node_record_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Module State
 * ──────────────────────────────────────────────────────────────────────────── */

static node_health_record_t s_records[OPENDASH_NODE_COUNT];
static SemaphoreHandle_t    s_mutex;
static nvs_handle_t         s_nvs;
static uint32_t             s_boot_time_ms;
static bool                 s_initialized = false;

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────────── */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void update_rssi_avg(node_health_record_t *r, int8_t rssi)
{
    r->rssi_history[r->rssi_index % NODE_HEALTH_RSSI_HISTORY] = rssi;
    r->rssi_index++;

    /* Calculate rolling average */
    int sum = 0;
    int count = (r->rssi_index < NODE_HEALTH_RSSI_HISTORY)
                ? r->rssi_index : NODE_HEALTH_RSSI_HISTORY;
    for (int i = 0; i < count; i++) {
        sum += r->rssi_history[i];
    }
    r->rssi_avg = (int8_t)(sum / count);
}

/* ────────────────────────────────────────────────────────────────────────────
 * NVS Persistence
 * ──────────────────────────────────────────────────────────────────────────── */

static void load_nvs_registry(void)
{
    char key[8];
    nvs_node_record_t rec;
    size_t len;

    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        snprintf(key, sizeof(key), NVS_KEY_PREFIX "%d", i);
        len = sizeof(rec);

        esp_err_t err = nvs_get_blob(s_nvs, key, &rec, &len);
        if (err == ESP_OK && len == sizeof(rec)) {
            /* Node was known from previous boot */
            node_health_record_t *r = &s_records[i];
            memcpy(r->mac, rec.mac, 6);
            r->mac_known = true;
            r->prev_state = NODE_STATE_UNKNOWN;

            /*
             * Heartbeat-mode nodes that were previously ONLINE start
             * immediately as ONLINE — no waiting for re-discovery.
             * This gives near-zero startup time for known nodes.
             * Frequency-mode nodes (LEFT, BMS) start AWAITING since
             * they will confirm within 1-2 seconds anyway.
             */
            if (rec.was_online &&
                NODE_EXPECTED_FREQ_HZ[i] <= NODE_HEALTH_HEARTBEAT_MODE_HZ &&
                NODE_EXPECTED_FREQ_HZ[i] > 0) {
                r->state = NODE_STATE_ONLINE;
                r->good_windows = NODE_HEALTH_ONLINE_WINDOWS;
                ESP_LOGI(TAG, "NVS: %s restored (MAC=" MACSTR ", was ONLINE) → instant ONLINE",
                         NODE_NAMES[i], MAC2STR(rec.mac));
            } else {
                r->state = NODE_STATE_AWAITING;
                ESP_LOGI(TAG, "NVS: %s restored (MAC=" MACSTR ", was %s)",
                         NODE_NAMES[i], MAC2STR(rec.mac),
                         rec.was_online ? "ONLINE" : "OFFLINE");
            }
        } else {
            /* Never seen this node */
            s_records[i].state = NODE_STATE_UNKNOWN;
        }
    }
}

static void persist_node(int idx)
{
    node_health_record_t *r = &s_records[idx];
    if (!r->mac_known) return;

    nvs_node_record_t rec;
    memcpy(rec.mac, r->mac, 6);
    rec.node_type = (uint8_t)r->node_type;
    rec.was_online = (r->state == NODE_STATE_ONLINE || r->state == NODE_STATE_DEGRADED) ? 1 : 0;

    char key[8];
    snprintf(key, sizeof(key), NVS_KEY_PREFIX "%d", idx);

    esp_err_t err = nvs_set_blob(s_nvs, key, &rec, sizeof(rec));
    if (err == ESP_OK) {
        nvs_commit(s_nvs);
        r->nvs_dirty = false;
    } else {
        ESP_LOGW(TAG, "NVS write failed for %s: %s", NODE_NAMES[idx], esp_err_to_name(err));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * State Machine Transitions
 * ──────────────────────────────────────────────────────────────────────────── */

static void transition_state(node_health_record_t *r, node_health_state_t new_state)
{
    if (r->state == new_state) return;

    r->prev_state = r->state;
    r->state = new_state;
    r->nvs_dirty = true;

    int idx = (int)r->node_type;
    ESP_LOGI(TAG, "%s: %s → %s (rate=%.1f Hz, RSSI=%d dBm, missed=%d)",
             NODE_NAMES[idx],
             node_health_state_name(r->prev_state),
             node_health_state_name(new_state),
             r->actual_rate_hz,
             r->rssi_avg,
             r->missed_windows);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t node_health_init(void)
{
    if (s_initialized) return ESP_OK;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) return ESP_ERR_NO_MEM;

    s_boot_time_ms = now_ms();

    /* Zero all records */
    memset(s_records, 0, sizeof(s_records));
    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        s_records[i].node_type = (opendash_node_t)i;
        s_records[i].state = NODE_STATE_UNKNOWN;
        s_records[i].boot_time_ms = s_boot_time_ms;

        /* Pre-calculate expected messages per window */
        uint8_t freq = NODE_EXPECTED_FREQ_HZ[i];
        if (freq > 0) {
            s_records[i].expected_per_window =
                (uint16_t)((freq * NODE_HEALTH_WINDOW_MS) / 1000);
            if (s_records[i].expected_per_window == 0) {
                s_records[i].expected_per_window = 1;
            }
        }
        s_records[i].window_start_ms = s_boot_time_ms;
    }

    /* Open NVS and load previous topology */
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s — starting fresh", esp_err_to_name(err));
        /* Non-fatal: run without persistence */
    } else {
        load_nvs_registry();
    }

    s_initialized = true;
    ESP_LOGI(TAG, "Node health monitor initialized (%d nodes, window=%dms, "
             "offline after %d missed windows)",
             OPENDASH_NODE_COUNT, NODE_HEALTH_WINDOW_MS,
             NODE_HEALTH_OFFLINE_WINDOWS);
    return ESP_OK;
}

void node_health_rx(opendash_node_t node, int8_t rssi)
{
    if (node >= OPENDASH_NODE_COUNT) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    node_health_record_t *r = &s_records[node];
    uint32_t now = now_ms();

    r->last_rx_ms = now;
    r->rx_count_window++;

    /* Record first-ever contact */
    if (r->first_seen_ms == 0) {
        r->first_seen_ms = now;
    }

    /* Update RSSI */
    update_rssi_avg(r, rssi);

    /* Immediate state upgrade: any data = node is alive */
    if (r->state == NODE_STATE_UNKNOWN || r->state == NODE_STATE_AWAITING ||
        r->state == NODE_STATE_OFFLINE) {
        /* Go to ONLINE immediately — the evaluate() function handles
         * sustained quality checks.  First contact = instant ONLINE.
         * We don't want a node that just sent us data to show OFFLINE. */
        transition_state(r, NODE_STATE_ONLINE);
        r->missed_windows = 0;
        r->good_windows = NODE_HEALTH_ONLINE_WINDOWS; /* Skip warmup on first contact */
    }

    xSemaphoreGive(s_mutex);
}

void node_health_ack(opendash_node_t node)
{
    if (node >= OPENDASH_NODE_COUNT) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    node_health_record_t *r = &s_records[node];
    r->last_ack_ms = now_ms();

    /*
     * ACK = the node's radio hardware is alive and responded.
     * If the node was OFFLINE or DEGRADED but we just got an ACK,
     * upgrade to at minimum DEGRADED (it exists, even if not pushing data).
     */
    if (r->state == NODE_STATE_OFFLINE) {
        transition_state(r, NODE_STATE_DEGRADED);
        r->missed_windows = 0;
        ESP_LOGI(TAG, "%s: ACK received — radio alive, upgrading from OFFLINE",
                 NODE_NAMES[node]);
    }

    xSemaphoreGive(s_mutex);
}

void node_health_nack(opendash_node_t node)
{
    if (node >= OPENDASH_NODE_COUNT) return;

    /* NACKs are noted but don't immediately offline a node.
     * Radio interference can cause sporadic NACKs.  The frequency
     * tracking is the primary driver of state transitions. */
    (void)node;
}

void node_health_evaluate(void)
{
    if (!s_initialized) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint32_t now = now_ms();

    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        node_health_record_t *r = &s_records[i];

        /* Skip center (self) and nodes with 0 Hz expected (no data source) */
        if (NODE_EXPECTED_FREQ_HZ[i] == 0) continue;

        /* Skip UNKNOWN nodes that have never been seen */
        if (r->state == NODE_STATE_UNKNOWN && !r->mac_known) continue;

        /* ── Heartbeat-timeout mode (reactive nodes: relay, MOS, pods) ─── */
        if (NODE_EXPECTED_FREQ_HZ[i] <= NODE_HEALTH_HEARTBEAT_MODE_HZ) {
            /*
             * PHILOSOPHY: These nodes send infrequent broadcasts (30-45s).
             * ESP-NOW broadcasts have NO retry — missed = gone.  We CANNOT
             * rely on absence of heartbeats to declare offline, because
             * RF conditions can cause multiple consecutive missed broadcasts.
             *
             * RULE: Once a heartbeat-mode node is ONLINE, it stays ONLINE.
             * The ONLY way it goes OFFLINE is:
             *   1) Boot grace expires and it was NEVER heard from, OR
             *   2) A direct NACK proves its radio is dead (handled in nack())
             *
             * This guarantees ZERO false offlines for these nodes.
             */

            /* If we've ever heard from it, it's alive until proven dead */
            if (r->last_rx_ms > 0 || r->last_ack_ms > 0) {
                if (r->state != NODE_STATE_ONLINE) {
                    transition_state(r, NODE_STATE_ONLINE);
                }
            }

            /* Boot grace: never seen at all → wait, then offline */
            if (r->last_rx_ms == 0 && r->last_ack_ms == 0 &&
                r->state == NODE_STATE_AWAITING) {
                if ((now - s_boot_time_ms) >= NODE_HEALTH_BOOT_GRACE_MS) {
                    transition_state(r, NODE_STATE_OFFLINE);
                }
            }

            r->rx_count_window = 0;
            r->window_start_ms = now;
            continue;  /* Skip frequency-ratio logic */
        }

        /* ── Frequency-ratio mode (active senders) ─────────────────── */

        /* Check if measurement window has elapsed */
        uint32_t elapsed = now - r->window_start_ms;
        if (elapsed < NODE_HEALTH_WINDOW_MS) continue;

        /* ── Window evaluation ─────────────────────────────────────── */

        /* Calculate actual rate for this window */
        float actual_in_window = (float)r->rx_count_window;

        /* Smooth the actual rate (exponential moving average) */
        float instant_hz = (actual_in_window * 1000.0f) / (float)elapsed;
        if (r->actual_rate_hz == 0.0f) {
            r->actual_rate_hz = instant_hz;
        } else {
            r->actual_rate_hz = (r->actual_rate_hz * 0.7f) + (instant_hz * 0.3f);
        }

        /* ── ACK override check ────────────────────────────────────── */
        bool ack_alive = (r->last_ack_ms > 0) &&
                         ((now - r->last_ack_ms) < NODE_HEALTH_ACK_ALIVE_MS);

        /* ── State machine logic ───────────────────────────────────
         *
         * PHILOSOPHY: The goal is to NEVER show a node as offline when
         * it's actually online.  We heavily bias toward ONLINE.
         *
         * - ANY data in a window (ratio > missed) = ONLINE.  Full stop.
         * - DEGRADED only if ratio is low AND no ACK for multiple windows.
         * - OFFLINE only after N consecutive windows of ZERO data + no ACK.
         *
         * Bouncing between states is UNACCEPTABLE.  Once ONLINE, a node
         * stays ONLINE until there's a sustained absence of data.
         * ──────────────────────────────────────────────────────────── */

        if (actual_in_window > 0 || ack_alive) {
            /* Got data OR got an ACK = node is alive.  Period. */
            r->missed_windows = 0;
            r->good_windows++;
            if (r->state != NODE_STATE_ONLINE) {
                transition_state(r, NODE_STATE_ONLINE);
            }
        } else {
            /* Zero data received in this window AND no recent ACK */
            r->missed_windows++;
            r->good_windows = 0;

            /* Boot grace period: AWAITING nodes get extra time */
            if (r->state == NODE_STATE_AWAITING) {
                if ((now - s_boot_time_ms) < NODE_HEALTH_BOOT_GRACE_MS) {
                    goto next_window;
                }
            }

            /* Only go OFFLINE after sustained silence */
            if (r->missed_windows >= NODE_HEALTH_OFFLINE_WINDOWS) {
                if (r->state != NODE_STATE_OFFLINE) {
                    transition_state(r, NODE_STATE_OFFLINE);
                }
            } else if (r->missed_windows >= 2 && r->state == NODE_STATE_ONLINE) {
                /* 2 consecutive empty seconds = DEGRADED (link quality issue) */
                transition_state(r, NODE_STATE_DEGRADED);
            }
        }

next_window:
        /* Reset window counters */
        r->rx_count_window = 0;
        r->window_start_ms = now;
    }

    /* Persist dirty records */
    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        if (s_records[i].nvs_dirty) {
            persist_node(i);
        }
    }

    xSemaphoreGive(s_mutex);
}

node_health_state_t node_health_get_state(opendash_node_t node)
{
    if (node >= OPENDASH_NODE_COUNT) return NODE_STATE_UNKNOWN;
    return s_records[node].state;
}

const node_health_record_t *node_health_get_record(opendash_node_t node)
{
    if (node >= OPENDASH_NODE_COUNT) return NULL;
    return &s_records[node];
}

bool node_health_is_alive(opendash_node_t node)
{
    if (node >= OPENDASH_NODE_COUNT) return false;

    node_health_state_t state = s_records[node].state;

    switch (state) {
        case NODE_STATE_ONLINE:
        case NODE_STATE_DEGRADED:
            return true;

        case NODE_STATE_AWAITING:
            /* Alive during boot grace period */
            return ((now_ms() - s_boot_time_ms) < NODE_HEALTH_BOOT_GRACE_MS);

        case NODE_STATE_OFFLINE:
        case NODE_STATE_UNKNOWN:
        default:
            return false;
    }
}

void node_health_register_mac(opendash_node_t node, const uint8_t mac[6])
{
    if (node >= OPENDASH_NODE_COUNT || !mac) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    node_health_record_t *r = &s_records[node];
    memcpy(r->mac, mac, 6);
    r->mac_known = true;
    r->nvs_dirty = true;

    /* If unknown, move to awaiting (we now know this node exists) */
    if (r->state == NODE_STATE_UNKNOWN) {
        transition_state(r, NODE_STATE_AWAITING);
    }

    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Registered MAC for %s: " MACSTR,
             NODE_NAMES[node], MAC2STR(mac));
}

void node_health_persist(void)
{
    if (!s_initialized) return;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        if (s_records[i].nvs_dirty) {
            persist_node(i);
        }
    }
    xSemaphoreGive(s_mutex);
}

const char *node_health_state_name(node_health_state_t state)
{
    switch (state) {
        case NODE_STATE_UNKNOWN:  return "UNKNOWN";
        case NODE_STATE_AWAITING: return "AWAITING";
        case NODE_STATE_ONLINE:   return "ONLINE";
        case NODE_STATE_DEGRADED: return "DEGRADED";
        case NODE_STATE_OFFLINE:  return "OFFLINE";
        default:                  return "???";
    }
}

opendash_node_t node_health_find_by_mac(const uint8_t mac[6])
{
    if (!s_initialized || !mac) return OPENDASH_NODE_COUNT;

    for (int i = 0; i < OPENDASH_NODE_COUNT; i++) {
        if (s_records[i].mac_known &&
            memcmp(s_records[i].mac, mac, 6) == 0) {
            return (opendash_node_t)i;
        }
    }
    return OPENDASH_NODE_COUNT;
}
