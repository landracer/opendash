/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file node_definitions.h
 * @brief Node-to-channel mapping and capability declarations
 *
 * Maps every opendash_node_t to its default channel and declares
 * per-node capabilities so the channel dispatcher knows what each
 * node can send and receive.
 *
 * Uses OPENDASH_NODE_COUNT from opendash_common.h (currently 18).
 */

#ifndef NODE_DEFINITIONS_H
#define NODE_DEFINITIONS_H

#include "opendash_common.h"
#include "channel_config.h"

/* ────────────────────────────────────────────────────────────────────────────
 * Default Channel Assignment Table
 *
 * Index = opendash_node_t value.  Value = CHANNEL_* id.
 *
 * Rules:
 *   - Safety-critical / high-frequency data sources → CHANNEL_CRITICAL
 *   - Display pods (data consumers + relay feedback) → CHANNEL_MEDIUM
 *   - Relay/MOS modules (receive commands, report state) → CHANNEL_LOW for
 *     state reports;  commands arrive on CHANNEL_CONTROL
 *   - Center is the dispatcher — it doesn't have its own channel assignment.
 * ──────────────────────────────────────────────────────────────────────────── */

static const uint8_t NODE_DEFAULT_CHANNEL[OPENDASH_NODE_COUNT] = {
    [OPENDASH_NODE_CENTER]      = CHANNEL_CONTROL,  /* Master — dispatches all */
    [OPENDASH_NODE_LEFT]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_RIGHT]       = CHANNEL_MEDIUM,
    [OPENDASH_NODE_GPS]         = CHANNEL_CRITICAL,
    [OPENDASH_NODE_BMS]         = CHANNEL_CRITICAL,
    [OPENDASH_NODE_POD1]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_POD2]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_POD3]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_POD4]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_POD5]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_POD6]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_POD7]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_POD8]        = CHANNEL_MEDIUM,
    [OPENDASH_NODE_RELAY_4CH]   = CHANNEL_LOW,
    [OPENDASH_NODE_RELAY_8CH_A] = CHANNEL_LOW,
    [OPENDASH_NODE_RELAY_8CH_B] = CHANNEL_LOW,
    [OPENDASH_NODE_MOS_4CH_A]   = CHANNEL_LOW,
    [OPENDASH_NODE_MOS_4CH_B]   = CHANNEL_LOW,
};

/* ────────────────────────────────────────────────────────────────────────────
 * Node Capability Flags
 *
 * Bitfield — a node may have multiple capabilities.
 * ──────────────────────────────────────────────────────────────────────────── */

#define NODE_CAP_PUSH_DATA      (1 << 0)    /**< Node pushes data points */
#define NODE_CAP_RECV_DATA      (1 << 1)    /**< Node receives data points */
#define NODE_CAP_RELAY_CMD      (1 << 2)    /**< Node accepts relay ON/OFF */
#define NODE_CAP_SYSTEM_CMD     (1 << 3)    /**< Node accepts system commands */
#define NODE_CAP_OTA            (1 << 4)    /**< Node supports BLE OTA */
#define NODE_CAP_SD_LOG         (1 << 5)    /**< Node logs to SD card */
#define NODE_CAP_UART_RELAY     (1 << 6)    /**< Node relays UART data to center */

/**
 * @brief Per-node capability table.
 *
 * Index = opendash_node_t value.
 */
static const uint8_t NODE_CAPABILITIES[OPENDASH_NODE_COUNT] = {
    [OPENDASH_NODE_CENTER]      = NODE_CAP_RECV_DATA | NODE_CAP_SD_LOG | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_LEFT]        = NODE_CAP_PUSH_DATA | NODE_CAP_RECV_DATA | NODE_CAP_UART_RELAY | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_RIGHT]       = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_GPS]         = NODE_CAP_PUSH_DATA | NODE_CAP_SD_LOG | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_BMS]         = NODE_CAP_PUSH_DATA | NODE_CAP_SYSTEM_CMD,
    [OPENDASH_NODE_POD1]        = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_POD2]        = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_POD3]        = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_POD4]        = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_POD5]        = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_POD6]        = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_POD7]        = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_POD8]        = NODE_CAP_RECV_DATA | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_RELAY_4CH]   = NODE_CAP_PUSH_DATA | NODE_CAP_RELAY_CMD | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_RELAY_8CH_A] = NODE_CAP_PUSH_DATA | NODE_CAP_RELAY_CMD | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_RELAY_8CH_B] = NODE_CAP_PUSH_DATA | NODE_CAP_RELAY_CMD | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_MOS_4CH_A]   = NODE_CAP_PUSH_DATA | NODE_CAP_RELAY_CMD | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
    [OPENDASH_NODE_MOS_4CH_B]   = NODE_CAP_PUSH_DATA | NODE_CAP_RELAY_CMD | NODE_CAP_SYSTEM_CMD | NODE_CAP_OTA,
};

/* ────────────────────────────────────────────────────────────────────────────
 * Human-readable node names (for logging)
 * ──────────────────────────────────────────────────────────────────────────── */

static const char *const NODE_NAMES[OPENDASH_NODE_COUNT] = {
    [OPENDASH_NODE_CENTER]      = "CENTER",
    [OPENDASH_NODE_LEFT]        = "LEFT",
    [OPENDASH_NODE_RIGHT]       = "RIGHT",
    [OPENDASH_NODE_GPS]         = "GPS",
    [OPENDASH_NODE_BMS]         = "BMS",
    [OPENDASH_NODE_POD1]        = "POD1",
    [OPENDASH_NODE_POD2]        = "POD2",
    [OPENDASH_NODE_POD3]        = "POD3",
    [OPENDASH_NODE_POD4]        = "POD4",
    [OPENDASH_NODE_POD5]        = "POD5",
    [OPENDASH_NODE_POD6]        = "POD6",
    [OPENDASH_NODE_POD7]        = "POD7",
    [OPENDASH_NODE_POD8]        = "POD8",
    [OPENDASH_NODE_RELAY_4CH]   = "RELAY_4CH",
    [OPENDASH_NODE_RELAY_8CH_A] = "RELAY_8CH_A",
    [OPENDASH_NODE_RELAY_8CH_B] = "RELAY_8CH_B",
    [OPENDASH_NODE_MOS_4CH_A]   = "MOS_4CH_A",
    [OPENDASH_NODE_MOS_4CH_B]   = "MOS_4CH_B",
};

/* ────────────────────────────────────────────────────────────────────────────
 * Channel names (for logging)
 * ──────────────────────────────────────────────────────────────────────────── */

static const char *const CHANNEL_NAMES[CHANNEL_COUNT] = {
    [CHANNEL_CRITICAL] = "CRITICAL",
    [CHANNEL_MEDIUM]   = "MEDIUM",
    [CHANNEL_LOW]      = "LOW",
    [CHANNEL_CONTROL]  = "CONTROL",
};

/* ────────────────────────────────────────────────────────────────────────────
 * Per-channel offline timeout lookup
 * ──────────────────────────────────────────────────────────────────────────── */

static const uint32_t CHANNEL_OFFLINE_TIMEOUT_MS[CHANNEL_COUNT] = {
    [CHANNEL_CRITICAL] = CHANNEL_CRITICAL_OFFLINE_MS,
    [CHANNEL_MEDIUM]   = CHANNEL_MEDIUM_OFFLINE_MS,
    [CHANNEL_LOW]      = CHANNEL_LOW_OFFLINE_MS,
    [CHANNEL_CONTROL]  = CHANNEL_CONTROL_OFFLINE_MS,
};

/* ────────────────────────────────────────────────────────────────────────────
 * Per-channel retry count lookup
 * ──────────────────────────────────────────────────────────────────────────── */

static const uint8_t CHANNEL_MAX_RETRIES[CHANNEL_COUNT] = {
    [CHANNEL_CRITICAL] = CHANNEL_CRITICAL_MAX_RETRIES,
    [CHANNEL_MEDIUM]   = CHANNEL_MEDIUM_MAX_RETRIES,
    [CHANNEL_LOW]      = CHANNEL_LOW_MAX_RETRIES,
    [CHANNEL_CONTROL]  = CHANNEL_CONTROL_MAX_RETRIES,
};

#endif /* NODE_DEFINITIONS_H */