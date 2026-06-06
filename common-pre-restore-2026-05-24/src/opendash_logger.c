/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_logger.c
 * @brief OpenDash Data Logger — CSV telemetry logging to SPIFFS flash
 *
 * Implements session-based CSV logging to the "storage" SPIFFS partition.
 * A background FreeRTOS task drains a queue and writes to the log file
 * in batches to minimize flash wear and maximize throughput.
 *
 * Copyright (c) 2024-2026 uknowmelast & Axiom
 * All rights reserved.
 */

#include "opendash_logger.h"
#include "opendash_data_model.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "od_logger";

/* ────────────────────────────────────────────────────────────────────────────
 * Internal types
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    uint32_t timestamp_ms;   /**< Milliseconds since boot */
    uint16_t dp_id;          /**< Data point ID */
    float    value;          /**< Data value */
} log_entry_t;

/* ────────────────────────────────────────────────────────────────────────────
 * State
 * ──────────────────────────────────────────────────────────────────────────── */

static bool             s_initialized = false;
static bool             s_active      = false;
static bool             s_mounted     = false;
static uint32_t         s_session     = 0;
static uint32_t         s_drops       = 0;
static uint32_t         s_entries     = 0;
static FILE            *s_file        = NULL;
static QueueHandle_t    s_queue       = NULL;
static TaskHandle_t     s_task        = NULL;

/* ────────────────────────────────────────────────────────────────────────────
 * Data point name lookup (for human-readable CSV)
 * ──────────────────────────────────────────────────────────────────────────── */

static const char *dp_name(uint16_t id)
{
    switch (id) {
        /* Engine / OBD2 */
        case OPENDASH_DP_RPM:            return "RPM";
        case OPENDASH_DP_VEHICLE_SPEED:  return "SPEED";
        case OPENDASH_DP_COOLANT_TEMP:   return "COOLANT";
        case OPENDASH_DP_INTAKE_TEMP:    return "INTAKE_TEMP";
        case OPENDASH_DP_BOOST_PRESSURE: return "BOOST";
        case OPENDASH_DP_OIL_TEMP:       return "OIL_TEMP";
        case OPENDASH_DP_OIL_PRESSURE:   return "OIL_PRESS";
        case OPENDASH_DP_AFR:            return "AFR";
        case OPENDASH_DP_BATTERY_VOLTAGE:return "BATT_V";
        case OPENDASH_DP_EGT:            return "EGT";
        case OPENDASH_DP_TIMING_ADVANCE:return "TIMING";
        case OPENDASH_DP_MAF_RATE:       return "MAF";
        case OPENDASH_DP_FUEL_LEVEL:     return "FUEL";
        case OPENDASH_DP_TRANS_TEMP:     return "TRANS_TEMP";

        /* GPS */
        case OPENDASH_DP_GPS_SPEED:      return "GPS_SPEED";
        case OPENDASH_DP_GPS_HEADING:    return "GPS_HDG";
        case OPENDASH_DP_LATITUDE:        return "GPS_LAT";
        case OPENDASH_DP_LONGITUDE:       return "GPS_LON";
        case OPENDASH_DP_ALTITUDE:        return "GPS_ALT";
        case OPENDASH_DP_SAT_COUNT:      return "GPS_SATS";
        case OPENDASH_DP_GPS_FIX:        return "GPS_FIX";

        /* IMU */
        case OPENDASH_DP_GFORCE_LAT:      return "LAT_G";
        case OPENDASH_DP_GFORCE_LONG:     return "LONG_G";
        case OPENDASH_DP_GFORCE_VERT:     return "VERT_G";

        /* System */
        case OPENDASH_DP_CPU_TEMP:        return "CPU_TEMP";
        case OPENDASH_DP_FREE_HEAP:      return "FREE_HEAP";

        default: return "UNKNOWN";
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * SPIFFS Mount
 * ──────────────────────────────────────────────────────────────────────────── */

static esp_err_t mount_spiffs(void)
{
    if (s_mounted) return ESP_OK;

    esp_vfs_spiffs_conf_t conf = {
        .base_path       = OPENDASH_LOG_MOUNT_POINT,
        .partition_label = "storage",
        .max_files       = 4,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "SPIFFS partition 'storage' not found");
        } else {
            ESP_LOGE(TAG, "Failed to mount SPIFFS: %s", esp_err_to_name(ret));
        }
        return ret;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %u KB total, %u KB used, %u KB free",
             (unsigned)(total / 1024), (unsigned)(used / 1024),
             (unsigned)((total - used) / 1024));

    s_mounted = true;
    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Session counter (NVS-persisted)
 * ──────────────────────────────────────────────────────────────────────────── */

static uint32_t load_session_counter(void)
{
    nvs_handle_t h;
    uint32_t session = 0;
    if (nvs_open("od_logger", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "session", &session);
        nvs_close(h);
    }
    return session;
}

static void save_session_counter(uint32_t session)
{
    nvs_handle_t h;
    if (nvs_open("od_logger", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "session", session);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Background flush task
 * ──────────────────────────────────────────────────────────────────────────── */

static void logger_task(void *arg)
{
    log_entry_t entry;
    char line[128];
    int flush_count = 0;

    ESP_LOGI(TAG, "Logger task started (session %lu)",
             (unsigned long)s_session);

    while (s_active || uxQueueMessagesWaiting(s_queue) > 0) {
        /* Block up to 200ms for the next entry */
        if (xQueueReceive(s_queue, &entry, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (s_file) {
                int len = snprintf(line, sizeof(line),
                                   "%lu,0x%04X,%s,%.4f\n",
                                   (unsigned long)entry.timestamp_ms,
                                   entry.dp_id,
                                   dp_name(entry.dp_id),
                                   entry.value);
                fwrite(line, 1, len, s_file);
                s_entries++;
                flush_count++;

                /* Flush to flash every 16 entries to balance
                 * write performance vs. data safety */
                if (flush_count >= 16) {
                    fflush(s_file);
                    flush_count = 0;
                }
            }
        }
    }

    /* Final flush */
    if (s_file) {
        fflush(s_file);
    }

    ESP_LOGI(TAG, "Logger task exiting (%lu entries, %lu drops)",
             (unsigned long)s_entries, (unsigned long)s_drops);

    s_task = NULL;
    vTaskDelete(NULL);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t opendash_logger_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t ret = mount_spiffs();
    if (ret != ESP_OK) return ret;

    s_session = load_session_counter();
    s_initialized = true;

    ESP_LOGI(TAG, "Logger initialized (last session: %lu)",
             (unsigned long)s_session);
    return ESP_OK;
}

esp_err_t opendash_logger_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_active) return ESP_ERR_INVALID_STATE;

    /* Increment and persist session number */
    s_session++;
    save_session_counter(s_session);

    /* Create log file */
    char path[64];
    snprintf(path, sizeof(path), OPENDASH_LOG_MOUNT_POINT "/log_%04lu.csv",
             (unsigned long)s_session);

    s_file = fopen(path, "w");
    if (!s_file) {
        ESP_LOGE(TAG, "Failed to create log file: %s", path);
        return ESP_FAIL;
    }

    /* Write CSV header */
    fprintf(s_file, "timestamp_ms,dp_id,dp_name,value\n");
    fflush(s_file);

    /* Create queue */
    s_queue = xQueueCreate(OPENDASH_LOG_QUEUE_SIZE, sizeof(log_entry_t));
    if (!s_queue) {
        fclose(s_file);
        s_file = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_drops   = 0;
    s_entries = 0;
    s_active  = true;

    /* Spawn flush task on CPU 0 at low priority */
    xTaskCreatePinnedToCore(logger_task, "od_logger", 4096, NULL, 2, &s_task, 0);

    ESP_LOGI(TAG, "Logging session %lu started → %s",
             (unsigned long)s_session, path);
    return ESP_OK;
}

esp_err_t opendash_logger_stop(void)
{
    if (!s_active) return ESP_ERR_INVALID_STATE;

    /* Signal the task to drain and exit */
    s_active = false;

    /* Wait for task to finish (max 2 seconds) */
    for (int i = 0; i < 20 && s_task != NULL; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Close file */
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }

    /* Delete queue */
    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    ESP_LOGI(TAG, "Session %lu stopped: %lu entries, %lu drops",
             (unsigned long)s_session,
             (unsigned long)s_entries,
             (unsigned long)s_drops);
    return ESP_OK;
}

esp_err_t opendash_logger_log(uint16_t dp_id, float value)
{
    if (!s_active || !s_queue) return ESP_ERR_INVALID_STATE;

    log_entry_t entry = {
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .dp_id        = dp_id,
        .value        = value,
    };

    if (xQueueSend(s_queue, &entry, 0) != pdTRUE) {
        s_drops++;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

bool opendash_logger_is_active(void)
{
    return s_active;
}

uint32_t opendash_logger_get_session(void)
{
    return s_session;
}

uint32_t opendash_logger_get_drops(void)
{
    return s_drops;
}

uint32_t opendash_logger_get_free_bytes(void)
{
    if (!s_mounted) return 0;

    size_t total = 0, used = 0;
    if (esp_spiffs_info("storage", &total, &used) != ESP_OK) return 0;
    return (uint32_t)(total - used);
}
