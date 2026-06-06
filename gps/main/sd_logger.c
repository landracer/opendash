/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file sd_logger.c
 * @brief OpenDash SD Card Logger — High-rate CSV telemetry to TF card
 *
 * Logs GPS + IMU data at 10 Hz to a FAT32-formatted TF card.
 * Uses ESP-IDF's SDMMC host in 1-line mode (not SPI) to avoid
 * DMA channel conflicts with the CO5300 QSPI AMOLED display.
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 *   SDMMC CLK  = GPIO2  (was SCK)
 *   SDMMC CMD  = GPIO1  (was MOSI)
 *   SDMMC D0   = GPIO3  (was MISO)
 *   (GPIO41 unused in SDMMC mode — no CS needed)
 *
 * Copyright (c) 2024-2026 uknowmelast & Axiom
 * All rights reserved.
 */

#include "sd_logger.h"
#include "opendash_data_model.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "sd_logger";

/* ────────────────────────────────────────────────────────────────────────────
 * Hardware Pin Definitions — Waveshare ESP32-S3-Touch-AMOLED-1.75
 * SDMMC 1-line mode (uses SDMMC peripheral, NOT SPI — avoids DMA conflict)
 * ──────────────────────────────────────────────────────────────────────────── */

#define SD_PIN_CLK       2   /* SDMMC CLK (was SPI SCK)  */
#define SD_PIN_CMD       1   /* SDMMC CMD (was SPI MOSI) */
#define SD_PIN_D0        3   /* SDMMC D0  (was SPI MISO) */

/* ────────────────────────────────────────────────────────────────────────────
 * Internal types
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Log entry type discriminator. */
typedef enum {
    LOG_TYPE_SNAPSHOT,     /**< Full GPS + IMU snapshot */
    LOG_TYPE_DATAPOINT,    /**< Single named data point */
} log_type_t;

/** @brief Full GPS + IMU snapshot entry. */
typedef struct {
    float   gps_speed;
    float   heading;
    double  latitude;
    double  longitude;
    float   altitude;
    int     satellites;
    float   hdop;
    bool    fix_valid;
    float   g_lat;
    float   g_long;
    float   g_vert;
} snapshot_entry_t;

/** @brief Single data point entry. */
typedef struct {
    uint16_t dp_id;
    float    value;
} datapoint_entry_t;

/** @brief Queued log entry (union of snapshot or datapoint). */
typedef struct {
    uint32_t    timestamp_ms;
    log_type_t  type;
    union {
        snapshot_entry_t  snap;
        datapoint_entry_t dp;
    };
} log_entry_t;

/* ────────────────────────────────────────────────────────────────────────────
 * State
 * ──────────────────────────────────────────────────────────────────────────── */

static bool            s_card_mounted = false;
static bool            s_active       = false;
static uint32_t        s_session      = 0;
static uint32_t        s_entries      = 0;
static uint32_t        s_drops        = 0;
static FILE           *s_file         = NULL;
static QueueHandle_t   s_queue        = NULL;
static TaskHandle_t    s_task         = NULL;
static sdmmc_card_t   *s_card         = NULL;

/* ────────────────────────────────────────────────────────────────────────────
 * Data point name lookup (mirrors opendash_logger.c)
 * ──────────────────────────────────────────────────────────────────────────── */

static const char *dp_name(uint16_t id)
{
    switch (id) {
        case OPENDASH_DP_RPM:              return "RPM";
        case OPENDASH_DP_VEHICLE_SPEED:    return "SPEED";
        case OPENDASH_DP_COOLANT_TEMP:     return "COOLANT";
        case OPENDASH_DP_INTAKE_TEMP:      return "INTAKE_TEMP";
        case OPENDASH_DP_BOOST_PRESSURE:   return "BOOST";
        case OPENDASH_DP_OIL_TEMP:         return "OIL_TEMP";
        case OPENDASH_DP_OIL_PRESSURE:     return "OIL_PRESS";
        case OPENDASH_DP_AFR:              return "AFR";
        case OPENDASH_DP_BATTERY_VOLTAGE:  return "BATT_V";
        case OPENDASH_DP_EGT:              return "EGT";
        case OPENDASH_DP_TIMING_ADVANCE:   return "TIMING";
        case OPENDASH_DP_MAF_RATE:         return "MAF";
        case OPENDASH_DP_FUEL_LEVEL:       return "FUEL";
        case OPENDASH_DP_TRANS_TEMP:       return "TRANS_TEMP";
        case OPENDASH_DP_GPS_SPEED:        return "GPS_SPEED";
        case OPENDASH_DP_GPS_HEADING:      return "GPS_HDG";
        case OPENDASH_DP_LATITUDE:         return "GPS_LAT";
        case OPENDASH_DP_LONGITUDE:        return "GPS_LON";
        case OPENDASH_DP_ALTITUDE:         return "GPS_ALT";
        case OPENDASH_DP_SAT_COUNT:        return "GPS_SATS";
        case OPENDASH_DP_GPS_FIX:          return "GPS_FIX";
        case OPENDASH_DP_GFORCE_LAT:       return "LAT_G";
        case OPENDASH_DP_GFORCE_LONG:      return "LONG_G";
        case OPENDASH_DP_GFORCE_VERT:      return "VERT_G";
        default:                           return "UNK";
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Session counter — stored in a small file on the SD card
 * ──────────────────────────────────────────────────────────────────────────── */

static uint32_t load_session_counter(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/session.txt", SD_MOUNT_POINT);

    FILE *f = fopen(path, "r");
    if (!f) return 1;

    uint32_t val = 0;
    fscanf(f, "%lu", &val);
    fclose(f);
    return (val > 0) ? val : 1;
}

static void save_session_counter(uint32_t session)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/session.txt", SD_MOUNT_POINT);

    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "%lu\n", session);
        fclose(f);
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Background flush task
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Batch size before fflush() to card. */
#define FLUSH_BATCH_SIZE    16

static void sd_logger_task(void *pvParameters)
{
    ESP_LOGI(TAG, "SD logger task started");
    log_entry_t entry;
    uint32_t batch_count = 0;

    while (s_active) {
        if (xQueueReceive(s_queue, &entry, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (!s_file) continue;

            if (entry.type == LOG_TYPE_SNAPSHOT) {
                /* Full GPS + IMU snapshot row */
                fprintf(s_file,
                    "%lu,SNAP,%.2f,%.1f,%.7f,%.7f,%.1f,%d,%.2f,%d,"
                    "%.3f,%.3f,%.3f\n",
                    entry.timestamp_ms,
                    entry.snap.gps_speed,
                    entry.snap.heading,
                    entry.snap.latitude,
                    entry.snap.longitude,
                    entry.snap.altitude,
                    entry.snap.satellites,
                    entry.snap.hdop,
                    entry.snap.fix_valid ? 1 : 0,
                    entry.snap.g_lat,
                    entry.snap.g_long,
                    entry.snap.g_vert);
            } else {
                /* Single data point row */
                fprintf(s_file, "%lu,DP,%s,%.4f\n",
                        entry.timestamp_ms,
                        dp_name(entry.dp.dp_id),
                        entry.dp.value);
            }

            s_entries++;
            batch_count++;

            if (batch_count >= FLUSH_BATCH_SIZE) {
                fflush(s_file);
                batch_count = 0;
            }
        } else {
            /* Queue timeout — flush any partial batch */
            if (batch_count > 0 && s_file) {
                fflush(s_file);
                batch_count = 0;
            }
        }
    }

    /* Final flush and close */
    if (s_file) {
        fflush(s_file);
        fclose(s_file);
        s_file = NULL;
    }

    ESP_LOGI(TAG, "SD logger task stopped (entries: %lu, drops: %lu)",
             s_entries, s_drops);
    vTaskDelete(NULL);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Public API
 * ════════════════════════════════════════════════════════════════════════════ */

esp_err_t sd_logger_init(void)
{
    ESP_LOGI(TAG, "Initializing SD card (SDMMC 1-line: CLK=%d, CMD=%d, D0=%d)",
             SD_PIN_CLK, SD_PIN_CMD, SD_PIN_D0);

    /* ── Mount FAT filesystem via SDMMC host (not SPI — avoids DMA conflict) ─ */
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,   /* Don't format user's card */
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  /* 20 MHz — conservative for 1-line mode */

    sdmmc_slot_config_t slot_cfg = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_cfg.clk = SD_PIN_CLK;
    slot_cfg.cmd = SD_PIN_CMD;
    slot_cfg.d0  = SD_PIN_D0;
    slot_cfg.d1  = -1;         /* 1-line mode */
    slot_cfg.d2  = -1;
    slot_cfg.d3  = -1;
    slot_cfg.d4  = -1;
    slot_cfg.d5  = -1;
    slot_cfg.d6  = -1;
    slot_cfg.d7  = -1;
    slot_cfg.width = 1;        /* 1-bit bus width */
    slot_cfg.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  /* Enable internal pull-ups on CMD/D0 */

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot_cfg,
                                            &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "No SD card inserted or mount failed — logging disabled");
        } else {
            ESP_LOGW(TAG, "SD card mount error: %s — logging disabled",
                     esp_err_to_name(ret));
        }
        s_card_mounted = false;
        return ESP_FAIL;
    }

    /* Print card info */
    sdmmc_card_print_info(stdout, s_card);

    s_card_mounted = true;
    s_session = load_session_counter();
    ESP_LOGI(TAG, "SD card mounted at %s (next session: %lu)",
             SD_MOUNT_POINT, s_session);

    return ESP_OK;
}

esp_err_t sd_logger_start(void)
{
    if (!s_card_mounted) {
        ESP_LOGD(TAG, "SD card not mounted — start ignored");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_active) {
        ESP_LOGW(TAG, "Logger already active");
        return ESP_ERR_INVALID_STATE;
    }

    /* Create log file */
    char filepath[80];
    snprintf(filepath, sizeof(filepath), "%s/log_%04lu.csv",
             SD_MOUNT_POINT, s_session);

    s_file = fopen(filepath, "w");
    if (!s_file) {
        ESP_LOGE(TAG, "Failed to create log file: %s", filepath);
        return ESP_FAIL;
    }

    /* Write CSV header */
    fprintf(s_file,
        "timestamp_ms,type,speed,heading,latitude,longitude,"
        "altitude,sats,hdop,fix,g_lat,g_long,g_vert\n");
    fflush(s_file);

    /* Create queue */
    s_queue = xQueueCreate(SD_LOG_QUEUE_SIZE, sizeof(log_entry_t));
    if (!s_queue) {
        ESP_LOGE(TAG, "Failed to create log queue");
        fclose(s_file);
        s_file = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_entries = 0;
    s_drops   = 0;
    s_active  = true;

    /* Start background writer task */
    BaseType_t xr = xTaskCreatePinnedToCore(
        sd_logger_task, "sd_logger", 4096, NULL,
        2,        /* Low-ish priority — data integrity, not latency */
        &s_task,
        1         /* Core 1 — keep off WiFi-heavy core 0 */
    );
    if (xr != pdPASS) {
        ESP_LOGE(TAG, "Failed to create logger task");
        s_active = false;
        fclose(s_file);
        s_file = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_FAIL;
    }

    /* Increment session for next time */
    s_session++;
    save_session_counter(s_session);

    ESP_LOGI(TAG, "Logging started: %s (session %lu)",
             filepath, s_session - 1);
    return ESP_OK;
}

void sd_logger_log_snapshot(float gps_speed, float heading,
                            double latitude, double longitude,
                            float altitude, int satellites, float hdop,
                            bool fix_valid,
                            float g_lat, float g_long, float g_vert)
{
    if (!s_active || !s_queue) return;

    log_entry_t entry = {
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .type         = LOG_TYPE_SNAPSHOT,
        .snap = {
            .gps_speed  = gps_speed,
            .heading    = heading,
            .latitude   = latitude,
            .longitude  = longitude,
            .altitude   = altitude,
            .satellites = satellites,
            .hdop       = hdop,
            .fix_valid  = fix_valid,
            .g_lat      = g_lat,
            .g_long     = g_long,
            .g_vert     = g_vert,
        },
    };

    if (xQueueSend(s_queue, &entry, 0) != pdTRUE) {
        s_drops++;
    }
}

void sd_logger_log_datapoint(uint16_t dp_id, float value)
{
    if (!s_active || !s_queue) return;

    log_entry_t entry = {
        .timestamp_ms = (uint32_t)(esp_timer_get_time() / 1000),
        .type         = LOG_TYPE_DATAPOINT,
        .dp = {
            .dp_id = dp_id,
            .value = value,
        },
    };

    if (xQueueSend(s_queue, &entry, 0) != pdTRUE) {
        s_drops++;
    }
}

esp_err_t sd_logger_stop(void)
{
    if (!s_active) return ESP_OK;

    ESP_LOGI(TAG, "Stopping SD logger...");
    s_active = false;

    /* Wait for task to finish flushing */
    if (s_task) {
        /* Give the task time to drain and self-delete */
        vTaskDelay(pdMS_TO_TICKS(500));
        s_task = NULL;
    }

    if (s_queue) {
        vQueueDelete(s_queue);
        s_queue = NULL;
    }

    ESP_LOGI(TAG, "SD logger stopped (total entries: %lu, drops: %lu)",
             s_entries, s_drops);
    return ESP_OK;
}

bool sd_logger_is_available(void)
{
    return s_card_mounted;
}

void sd_logger_get_stats(uint32_t *entries, uint32_t *drops, uint32_t *session)
{
    if (entries) *entries = s_entries;
    if (drops)   *drops   = s_drops;
    if (session) *session = s_session;
}
