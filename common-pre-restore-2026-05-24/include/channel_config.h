/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file channel_config.h
 * @brief Channel-based communication configuration for OpenDash
 *
 * Defines the 4 priority channels, their timing, offline thresholds,
 * and buffer sizes. This is the central tuning file for the event-driven
 * ESP-NOW architecture.
 *
 * ARCHITECTURE RULE: NO POLLING / NO PINGING
 * Nodes push data on change. Center routes by channel priority.
 * Offline detection uses data-absence timeout, not heartbeat polling.
 */

#ifndef CHANNEL_CONFIG_H
#define CHANNEL_CONFIG_H

/* ────────────────────────────────────────────────────────────────────────────
 * Channel IDs  (array indices — keep sequential starting at 0)
 * ──────────────────────────────────────────────────────────────────────────── */

#define CHANNEL_CRITICAL    0   /**< GPS, BMS, engine — highest priority */
#define CHANNEL_MEDIUM      1   /**< Pod displays, relay feedback */
#define CHANNEL_LOW         2   /**< Diagnostics, config, logging */
#define CHANNEL_CONTROL     3   /**< Immediate commands (relay, OTA, reboot) */
#define CHANNEL_COUNT       4   /**< Total number of channels */

/* ────────────────────────────────────────────────────────────────────────────
 * Channel Processing Intervals (ms)
 *
 * How often the dispatcher checks each channel's inbound queue.
 * This is NOT a poll interval — it's the maximum latency between a
 * message arriving and the app processing it.
 * ──────────────────────────────────────────────────────────────────────────── */

#define CHANNEL_CRITICAL_INTERVAL_MS    10
#define CHANNEL_MEDIUM_INTERVAL_MS      50
#define CHANNEL_LOW_INTERVAL_MS         200
#define CHANNEL_CONTROL_INTERVAL_MS     5

/* ────────────────────────────────────────────────────────────────────────────
 * Node Offline Thresholds (ms) — LEGACY
 *
 * These are now BACKUP thresholds only.  The primary online/offline detection
 * is handled by node_health.h which uses frequency-based monitoring with
 * multi-signal confirmation.  These remain as a hard ceiling for
 * channel_mgr_check_timeouts() which still runs as a safety net.
 *
 * The new node_health system will flag DEGRADED/OFFLINE much faster than
 * these values (typically within 600ms for a 50 Hz node).
 * ──────────────────────────────────────────────────────────────────────────── */

#define CHANNEL_CRITICAL_OFFLINE_MS     1000    /**< 1s hard ceiling — node_health catches faster */
#define CHANNEL_MEDIUM_OFFLINE_MS       1000    /**< 1s hard ceiling — 50 Hz nodes have no excuse */
#define CHANNEL_LOW_OFFLINE_MS          2000    /**< 2s — relay/MOS at 10 Hz, still tight */
#define CHANNEL_CONTROL_OFFLINE_MS      0       /**< N/A — control is command-driven, no timeout */

/* ────────────────────────────────────────────────────────────────────────────
 * Channel Retry Policy
 *
 * When a unicast send fails (no MAC-layer ACK), retry up to N times
 * with exponential backoff per channel priority.
 * ──────────────────────────────────────────────────────────────────────────── */

#define CHANNEL_CRITICAL_MAX_RETRIES    3
#define CHANNEL_MEDIUM_MAX_RETRIES      2
#define CHANNEL_LOW_MAX_RETRIES         1
#define CHANNEL_CONTROL_MAX_RETRIES     5       /**< Commands MUST arrive */

#define CHANNEL_RETRY_BASE_MS           5       /**< Base backoff: 5ms, 10ms, 20ms ... */

/* ────────────────────────────────────────────────────────────────────────────
 * Queue / Buffer Sizes
 * ──────────────────────────────────────────────────────────────────────────── */

#define CHANNEL_QUEUE_DEPTH             32      /**< Inbound messages per channel.
                                                 *   With batched DATA_BATCH packets
                                                 *   the per-channel rate is ~5 msg/s,
                                                 *   so 32 is comfortable. Going higher
                                                 *   starves internal RAM and prevents
                                                 *   ESP-NOW task creation. */
#define CHANNEL_QUEUE_ITEM_SIZE         192     /**< Max serialized message bytes.
                                                 *   Sized for DATA_BATCH (up to ~155 B):
                                                 *   3 B header + 1 B count + 25 × 6 B + 1 B checksum.
                                                 *   Must be >= largest serialized packet. */
#define CHANNEL_MAX_DATA_POINTS         64      /**< Tracked DPs for delta detection */
#define CHANNEL_CMD_QUEUE_DEPTH         16      /**< Outbound command queue (CH3) */

/* ────────────────────────────────────────────────────────────────────────────
 * Message Types (carried inside channel frames)
 * ──────────────────────────────────────────────────────────────────────────── */

#define CHANNEL_MSG_DATA_POINT      0x01    /**< Data point update (dp_id + float) */
#define CHANNEL_MSG_STATUS_REPORT   0x02    /**< Node self-announce / status */
#define CHANNEL_MSG_RELAY_CMD       0x03    /**< Relay/MOS on/off/PWM command */
#define CHANNEL_MSG_SYSTEM_CMD      0x04    /**< Reboot, OTA, brightness, etc. */
#define CHANNEL_MSG_CONFIG          0x05    /**< Configuration exchange */
#define CHANNEL_MSG_ANNOUNCE        0x06    /**< One-time boot announcement (replaces PING) */
#define CHANNEL_MSG_BATCH_DP        0x07    /**< Batched data points (multiple DPs in one frame) */

/* ────────────────────────────────────────────────────────────────────────────
 * Task Priorities (FreeRTOS)
 * ──────────────────────────────────────────────────────────────────────────── */

#define CHANNEL_TASK_PRIORITY_CRITICAL  5
#define CHANNEL_TASK_PRIORITY_MEDIUM    4
#define CHANNEL_TASK_PRIORITY_LOW       3
#define CHANNEL_TASK_PRIORITY_CONTROL   6   /**< Highest — commands must not wait */
#define CHANNEL_TASK_PRIORITY_DISPATCH  4   /**< Main dispatcher */

#define CHANNEL_TASK_STACK_SIZE         4096

#endif /* CHANNEL_CONFIG_H */