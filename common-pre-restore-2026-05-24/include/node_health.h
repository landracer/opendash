/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file node_health.h
 * @brief OpenDash Node Health Monitor — Multi-Layered Online/Offline Detection
 *
 * Replaces the old single-timeout data-absence detection with a robust
 * multi-signal approach that virtually eliminates false offline readings.
 *
 * DESIGN PHILOSOPHY:
 *   A node is ONLINE if ANY evidence of life exists.
 *   A node is OFFLINE only when ALL signals agree it's gone.
 *   If you can't trust online/offline, you can't trust any data.
 *
 * THREE DETECTION LAYERS:
 *
 *   Layer 1 — DATA FLOW (primary)
 *     Tracks the actual receive rate from each node and compares against
 *     that node's expected transmit frequency.  A node sending 50 Hz data
 *     that drops to 0 for 100ms is immediately suspicious.
 *
 *   Layer 2 — MAC-LAYER ACK (bidirectional link proof)
 *     When center sends data TO a node (e.g., data points to pods, relay
 *     commands), the ESP-NOW hardware ACK callback tells us immediately
 *     whether the peer's radio responded.  If we get an ACK, the node IS
 *     alive — period.  No timeout needed.
 *
 *   Layer 3 — NVS REGISTRY (persistent memory)
 *     On every state change, the node's MAC + last known status is persisted
 *     to NVS flash.  On reboot, center immediately knows which nodes SHOULD
 *     exist and starts in "AWAITING" state rather than "OFFLINE" — giving
 *     nodes time to boot without false offline flickers.
 *
 * STATE MACHINE (per node):
 *
 *   UNKNOWN  → never seen (no NVS record)
 *   AWAITING → known from NVS, waiting for first data after boot
 *   ONLINE   → actively receiving data at expected rate
 *   DEGRADED → receiving data but below expected rate (link quality issue)
 *   OFFLINE  → all signals confirm no communication
 *
 * KEY DESIGN DECISIONS:
 *   - No polling, no pinging — fully push-based + passive ACK monitoring
 *   - Per-node expected frequency (not one-size-fits-all timeout)
 *   - NVS persistence so reboot doesn't wipe known topology
 *   - RSSI history for signal quality trending
 *   - Hysteresis: must miss N consecutive windows before OFFLINE
 *   - Sub-second detection: 50 Hz node missing for 60ms = one missed window
 *
 * @see channel_management.h for the routing layer
 * @see espnow_master.h for the master controller
 */

#ifndef NODE_HEALTH_H
#define NODE_HEALTH_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Node Health States
 * ──────────────────────────────────────────────────────────────────────────── */

typedef enum {
    NODE_STATE_UNKNOWN  = 0,  /**< Never seen — no NVS record exists */
    NODE_STATE_AWAITING = 1,  /**< Known from NVS, waiting for first data post-boot */
    NODE_STATE_ONLINE   = 2,  /**< Active — data flowing at expected rate */
    NODE_STATE_DEGRADED = 3,  /**< Data arriving but below expected rate */
    NODE_STATE_OFFLINE  = 4,  /**< All signals confirm: node is gone */
} node_health_state_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Expected Node Frequencies (ESP-NOW packets/sec received by center)
 *
 * These values represent the ACTUAL measured packet rates from each node.
 * The health monitor tracks actual receive rate vs expected.  If a node
 * doesn't hit its expected rate, something is wrong.
 *
 * VALUES ARE PACKETS PER SECOND (not loop Hz):
 *   - Left: 15-26 data points per 200ms cycle = 75-130 pps (set 50 conservative)
 *   - Right: I2C slave display — NOT an ESP-NOW sender (set 0 = skip)
 *   - GPS: Broadcasts at 5Hz but reception is sporadic due to weak RSSI and
 *          no unicast retry (center never sends GPS anything → GPS stays in
 *          broadcast mode). Set 1 = heartbeat-mode for stability.
 *   - BMS: 22+14×5+5×5 = ~117 pps (set 100 conservative)
 *   - Pod1/Pod2: 3 data points per 200ms cycle = ~15 pps
 *   - Relay/MOS: Reactive only (45s heartbeat). Set 1 = heartbeat-timeout mode.
 *
 * SPECIAL VALUES:
 *   - 0 = Skip entirely (don't evaluate — node doesn't send ESP-NOW data)
 *   - 1 = Heartbeat-timeout mode (reactive nodes, uses last_rx/last_ack timeout)
 *   - >1 = Frequency-ratio mode (active senders, uses window counting)
 * ──────────────────────────────────────────────────────────────────────────── */

/** Expected packets/sec per node type (0=skip, 1=heartbeat mode, >1=freq mode) */
static const uint8_t NODE_EXPECTED_FREQ_HZ[OPENDASH_NODE_COUNT] = {
    [OPENDASH_NODE_CENTER]      = 0,    /* Master — doesn't push to itself */
    [OPENDASH_NODE_LEFT]        = 50,   /* Left: ~75-130 pps (active sender) */
    [OPENDASH_NODE_RIGHT]       = 1,    /* Right: heartbeat-only (I2C display, occasional announce) */
    [OPENDASH_NODE_GPS]         = 1,    /* GPS: broadcasts at 5Hz but center-side reception is
                                           sporadic (~0.2 Hz) due to -77dBm RSSI + broadcast
                                           mode (no unicast/retry). Heartbeat-mode = stable. */
    [OPENDASH_NODE_BMS]         = 40,   /* BMS: actual measured ~45-60 pps (active sender) */
    [OPENDASH_NODE_POD1]        = 1,    /* Pod1: sends brief bursts every ~39s (heartbeat mode) */
    [OPENDASH_NODE_POD2]        = 1,    /* Pod2: sends brief bursts every ~42s (heartbeat mode) */
    [OPENDASH_NODE_POD3]        = 1,    /* Pod3: not deployed (heartbeat mode) */
    [OPENDASH_NODE_POD4]        = 1,    /* Pod4: not deployed */
    [OPENDASH_NODE_POD5]        = 1,    /* Pod5: not deployed */
    [OPENDASH_NODE_POD6]        = 1,    /* Pod6: not deployed */
    [OPENDASH_NODE_POD7]        = 1,    /* Pod7: not deployed */
    [OPENDASH_NODE_POD8]        = 1,    /* Pod8: not deployed */
    [OPENDASH_NODE_RELAY_4CH]   = 1,    /* Relay: reactive only, 45s heartbeat */
    [OPENDASH_NODE_RELAY_8CH_A] = 1,    /* Relay: reactive only */
    [OPENDASH_NODE_RELAY_8CH_B] = 1,    /* Relay: reactive only */
    [OPENDASH_NODE_MOS_4CH_A]   = 1,    /* MOS: reactive only */
    [OPENDASH_NODE_MOS_4CH_B]   = 1,    /* MOS: reactive only */
};

/* ────────────────────────────────────────────────────────────────────────────
 * Tuning Constants
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * Measurement window for frequency calculation (ms).
 * MUST be large enough to span multiple node burst cycles.
 * Nodes send in bursts every 200ms — a 1000ms window captures ~5 bursts,
 * smoothing over jitter and window misalignment.
 */
#define NODE_HEALTH_WINDOW_MS           1000

/**
 * Number of consecutive missed windows before declaring OFFLINE.
 * With 1000ms windows, 3 missed = 3 seconds of total silence.
 * That's a REAL offline — not jitter.
 */
#define NODE_HEALTH_OFFLINE_WINDOWS     3

/**
 * Threshold ratio: actual_rate / expected_rate below this = DEGRADED.
 * 0.25 is very generous — node can deliver 25% of expected and still be
 * considered ONLINE.  This prevents oscillation from bursty senders.
 */
#define NODE_HEALTH_DEGRADED_RATIO      0.25f

/**
 * Threshold ratio: below this for one window = missed window.
 * 0.05 means basically zero data in that 1-second window.
 */
#define NODE_HEALTH_MISSED_RATIO        0.05f

/**
 * Number of consecutive GOOD windows required to upgrade from DEGRADED
 * to ONLINE.  Prevents rapid DEGRADED↔ONLINE bouncing.
 */
#define NODE_HEALTH_ONLINE_WINDOWS      2

/**
 * Grace period after boot before AWAITING nodes go OFFLINE (ms).
 * Must be longer than the slowest node's boot + first heartbeat.
 * BMS takes up to 22s, nodes with 45s heartbeat need margin.
 * 60s ensures no node goes OFFLINE during boot phase.
 */
#define NODE_HEALTH_BOOT_GRACE_MS       60000

/**
 * RSSI history depth for signal quality trending.
 */
#define NODE_HEALTH_RSSI_HISTORY        8

/**
 * Maximum time an ACK-confirmed node can be silent before we override (ms).
 * If center successfully sent TO a node (ACK received) within this window,
 * the node is definitely alive regardless of whether it sent data back.
 */
#define NODE_HEALTH_ACK_ALIVE_MS        500

/**
 * Heartbeat-timeout mode: max silence before declaring OFFLINE (ms).
 * Used for reactive nodes (relay/MOS/pods) that only send heartbeats
 * every ~30-45s or respond to commands.  120s allows for 1+ fully missed
 * heartbeat cycles.  Only after 2 minutes of total radio silence will a
 * heartbeat-mode node be declared OFFLINE.
 */
#define NODE_HEALTH_HEARTBEAT_TIMEOUT_MS 120000

/**
 * Heartbeat-timeout mode: threshold below which we use timeout instead of
 * frequency-ratio detection.  Nodes with expected_freq <= this value use
 * simple last-seen / last-ACK timeout logic instead of window counting.
 */
#define NODE_HEALTH_HEARTBEAT_MODE_HZ   1

/* ────────────────────────────────────────────────────────────────────────────
 * Per-Node Health Record
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    /* Identity */
    opendash_node_t     node_type;
    uint8_t             mac[6];
    bool                mac_known;

    /* State machine */
    node_health_state_t state;
    node_health_state_t prev_state;     /**< For transition detection */

    /* Timing */
    uint32_t            first_seen_ms;  /**< When node first appeared (this boot) */
    uint32_t            last_rx_ms;     /**< Last time we received ANY data */
    uint32_t            last_ack_ms;    /**< Last time a send TO this node was ACKed */
    uint32_t            boot_time_ms;   /**< When center booted (for grace period) */

    /* Frequency tracking */
    uint16_t            rx_count_window;    /**< Messages received in current window */
    uint16_t            expected_per_window;/**< Expected messages per window */
    uint32_t            window_start_ms;    /**< Current measurement window start */
    uint8_t             missed_windows;     /**< Consecutive windows with no/low data */
    uint8_t             good_windows;       /**< Consecutive windows above degraded ratio */
    float               actual_rate_hz;     /**< Smoothed actual receive rate */

    /* Signal quality */
    int8_t              rssi_history[NODE_HEALTH_RSSI_HISTORY];
    uint8_t             rssi_index;
    int8_t              rssi_avg;           /**< Rolling average RSSI */

    /* Persistence flag */
    bool                nvs_dirty;          /**< Needs to be written to NVS */
} node_health_record_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the node health system.
 *
 * Loads NVS registry (known nodes from previous boot), sets all known nodes
 * to AWAITING state, unknown nodes to UNKNOWN state.
 * Call once at boot after nvs_flash_init().
 *
 * @return ESP_OK on success.
 */
esp_err_t node_health_init(void);

/**
 * @brief Record that data was received from a node.
 *
 * Called by the dispatcher every time a valid frame arrives.
 * Updates rx timestamp, increments window counter, records RSSI.
 * This is the primary "proof of life" signal.
 *
 * @param node  Which node sent data.
 * @param rssi  Signal strength of this reception.
 */
void node_health_rx(opendash_node_t node, int8_t rssi);

/**
 * @brief Record that a unicast send TO a node was ACKed at MAC layer.
 *
 * Called from the ESP-NOW send callback when status == SUCCESS.
 * This is Layer 2 proof: the node's radio hardware responded.
 * Even if the node hasn't pushed data recently, an ACK means it's ALIVE.
 *
 * @param node  Which node ACKed our transmission.
 */
void node_health_ack(opendash_node_t node);

/**
 * @brief Record that a unicast send TO a node FAILED (no ACK).
 *
 * Called from the ESP-NOW send callback when status == FAIL.
 * A single failure is not conclusive (radio interference), but repeated
 * failures combined with no inbound data = strong offline signal.
 *
 * @param node  Which node failed to ACK.
 */
void node_health_nack(opendash_node_t node);

/**
 * @brief Periodic health evaluation — call at 5 Hz (every 200ms).
 *
 * Evaluates the measurement window for all nodes:
 *   - Compares rx_count vs expected_per_window
 *   - Advances the window
 *   - Runs the state machine transitions
 *   - Persists changes to NVS if dirty
 *
 * Should be called from a timer or periodic task.  NOT from ISR.
 */
void node_health_evaluate(void);

/**
 * @brief Get current health state for a node.
 *
 * @param node  Which node.
 * @return Current state (UNKNOWN/AWAITING/ONLINE/DEGRADED/OFFLINE).
 */
node_health_state_t node_health_get_state(opendash_node_t node);

/**
 * @brief Get full health record for a node (read-only).
 *
 * @param node  Which node.
 * @return Pointer to record, or NULL if invalid.
 */
const node_health_record_t *node_health_get_record(opendash_node_t node);

/**
 * @brief Check if a node should be considered "online" for UI purposes.
 *
 * Returns true for ONLINE and DEGRADED states.
 * Returns true for AWAITING during boot grace period.
 * Returns false only for OFFLINE and UNKNOWN.
 *
 * @param node  Which node.
 * @return true if the node is alive (or presumed alive during boot).
 */
bool node_health_is_alive(opendash_node_t node);

/**
 * @brief Register a node's MAC address (from ANNOUNCE or auto-discovery).
 *
 * Persists to NVS so next boot starts in AWAITING instead of UNKNOWN.
 *
 * @param node  Node type.
 * @param mac   6-byte MAC address.
 */
void node_health_register_mac(opendash_node_t node, const uint8_t mac[6]);

/**
 * @brief Force-persist all dirty records to NVS.
 *
 * Called automatically during evaluate(), but can be called manually
 * before shutdown or OTA.
 */
void node_health_persist(void);

/**
 * @brief Get human-readable state name.
 */
const char *node_health_state_name(node_health_state_t state);

/**
 * @brief Find a node index by its MAC address (NVS registry lookup).
 *
 * Searches all health records for a matching MAC.  Used to re-identify
 * nodes from data frames when channel_mgr hasn't re-registered them yet.
 *
 * @param mac  6-byte MAC address to search for.
 * @return Node index if found, OPENDASH_NODE_COUNT if not found.
 */
opendash_node_t node_health_find_by_mac(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif

#endif /* NODE_HEALTH_H */
