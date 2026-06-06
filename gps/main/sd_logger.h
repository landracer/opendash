/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file sd_logger.h
 * @brief OpenDash SD Card Logger — High-rate CSV telemetry to TF card
 *
 * Logs GPS + IMU + engine data to the onboard TF card slot on the
 * Waveshare ESP32-S3-Touch-AMOLED-1.75 using SPI interface.
 *
 * SPI Pins (Waveshare hardware):
 *   CS   = GPIO41
 *   MOSI = GPIO1
 *   MISO = GPIO3
 *   SCK  = GPIO2
 *
 * Design:
 *   - Session-based: new CSV file per power cycle (log_NNNN.csv)
 *   - Non-blocking: log calls queue data; background task writes to card
 *   - Graceful: if no SD card inserted, all calls are no-ops
 *   - Batched writes: flushes to card every N entries or 1 second
 *   - Logs at GPS broadcast rate (10 Hz) — full GPS + IMU snapshot
 *
 * CSV format:
 *   timestamp_ms,gps_speed,heading,latitude,longitude,altitude,sats,hdop,fix,g_lat,g_long,g_vert,rpm,coolant,boost,afr
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

/** @brief SD card mount point. */
#define SD_MOUNT_POINT    "/sdcard"

/** @brief Maximum queued log entries before drops. */
#define SD_LOG_QUEUE_SIZE 128

/**
 * @brief Initialize the SD card logger.
 *
 * Mounts the TF card via SPI, loads session counter from a file on the card,
 * and prepares for logging.  If no card is inserted, returns ESP_FAIL
 * and all subsequent log/start/stop calls become no-ops.
 *
 * @return ESP_OK if card mounted, ESP_FAIL if no card or mount error.
 */
esp_err_t sd_logger_init(void);

/**
 * @brief Start a new logging session.
 *
 * Creates a new CSV file and spawns the background flush task.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if no card or already logging.
 */
esp_err_t sd_logger_start(void);

/**
 * @brief Log a complete GPS + IMU snapshot.
 *
 * Non-blocking.  Queues the data for the background writer.
 *
 * @param gps_speed   Speed in km/h
 * @param heading     Heading in degrees
 * @param latitude    Latitude (double-precision)
 * @param longitude   Longitude (double-precision)
 * @param altitude    Altitude in meters
 * @param satellites  Number of satellites
 * @param hdop        Horizontal dilution of precision
 * @param fix_valid   true if GPS has valid fix
 * @param g_lat       Lateral g-force
 * @param g_long      Longitudinal g-force
 * @param g_vert      Vertical g-force
 */
void sd_logger_log_snapshot(float gps_speed, float heading,
                            double latitude, double longitude,
                            float altitude, int satellites, float hdop,
                            bool fix_valid,
                            float g_lat, float g_long, float g_vert);

/**
 * @brief Log a single named data point (engine data from center).
 *
 * @param dp_id   Data point ID (OPENDASH_DP_*)
 * @param value   Data value
 */
void sd_logger_log_datapoint(uint16_t dp_id, float value);

/**
 * @brief Stop the current logging session.
 *
 * Flushes remaining data and closes the file.
 *
 * @return ESP_OK on success.
 */
esp_err_t sd_logger_stop(void);

/**
 * @brief Check if SD card is available and mounted.
 *
 * @return true if card is mounted and ready.
 */
bool sd_logger_is_available(void);

/**
 * @brief Get current session statistics.
 *
 * @param[out] entries  Total entries written this session
 * @param[out] drops    Total entries dropped (queue full)
 * @param[out] session  Current session number
 */
void sd_logger_get_stats(uint32_t *entries, uint32_t *drops, uint32_t *session);

#ifdef __cplusplus
}
#endif
