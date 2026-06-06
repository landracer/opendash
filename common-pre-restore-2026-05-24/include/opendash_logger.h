/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_logger.h
 * @brief OpenDash Data Logger — CSV telemetry logging to flash storage
 *
 * Logs data points as timestamped CSV records to the SPIFFS "storage"
 * partition.  Each logging session creates a new file (log_NNNN.csv).
 *
 * Design:
 *   - Session-based: call logger_start() to begin, logger_stop() to end
 *   - Non-blocking: log calls queue data; a background task flushes to flash
 *   - Ring-buffer drop policy: oldest entries are dropped if the queue fills
 *   - File rotation: new session number each start (persisted in NVS)
 *   - Can be easily extended to SD card by swapping the file path prefix
 *
 * CSV format:
 *   timestamp_ms,dp_id,dp_name,value
 *   12345,0x0101,RPM,4523.00
 *   12346,0x0102,COOLANT_TEMP,92.30
 *
 * Copyright (c) 2024-2026 uknowmelast & Axiom
 * All rights reserved.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Maximum number of queued log entries before drops occur. */
#define OPENDASH_LOG_QUEUE_SIZE     64

/** @brief Maximum log entries per file before rotation (0 = no limit). */
#define OPENDASH_LOG_MAX_ENTRIES    100000

/** @brief Mount point for SPIFFS storage. */
#define OPENDASH_LOG_MOUNT_POINT    "/storage"

/**
 * @brief Initialize the logger subsystem.
 *
 * Mounts the SPIFFS "storage" partition and loads the session counter
 * from NVS.  Must be called once before logger_start().
 *
 * @return ESP_OK on success.
 */
esp_err_t opendash_logger_init(void);

/**
 * @brief Start a new logging session.
 *
 * Creates a new CSV file (log_NNNN.csv) and spawns the background
 * flush task.  Subsequent calls to opendash_logger_log() will be
 * written to this file.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if already logging.
 */
esp_err_t opendash_logger_start(void);

/**
 * @brief Stop the current logging session.
 *
 * Flushes remaining queued entries, closes the file, and stops the
 * background task.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if not logging.
 */
esp_err_t opendash_logger_stop(void);

/**
 * @brief Log a single data point.
 *
 * Non-blocking — the entry is queued and written by the background task.
 * If the queue is full, the entry is silently dropped and a drop counter
 * is incremented.
 *
 * @param dp_id     Data point ID (e.g. OPENDASH_DP_RPM).
 * @param value     The float value to log.
 * @return ESP_OK if queued, ESP_ERR_NO_MEM if queue full (dropped).
 */
esp_err_t opendash_logger_log(uint16_t dp_id, float value);

/**
 * @brief Check if the logger is currently recording.
 * @return true if a session is active.
 */
bool opendash_logger_is_active(void);

/**
 * @brief Get the current session number.
 * @return Session number (1-based), or 0 if not initialized.
 */
uint32_t opendash_logger_get_session(void);

/**
 * @brief Get the number of entries dropped due to queue overflow.
 * @return Drop count since the session started.
 */
uint32_t opendash_logger_get_drops(void);

/**
 * @brief Get approximate free space on the storage partition (bytes).
 * @return Free bytes, or 0 if not mounted.
 */
uint32_t opendash_logger_get_free_bytes(void);

#ifdef __cplusplus
}
#endif
