/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_odometer.c
 * @brief OpenDash Odometer & Trip Meter — Implementation
 *
 * Stores distance in NVS with rate-limited writes to prevent flash wear.
 * All values stored internally as meters (uint32_t).
 *
 * @see opendash_odometer.h for API documentation.
 */

#include "opendash_odometer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "od_odometer";

/* NVS keys (max 15 characters) */
#define NVS_KEY_TOTAL   "odo_total"
#define NVS_KEY_TRIP_A  "odo_trip_a"
#define NVS_KEY_TRIP_B  "odo_trip_b"

/* ────────────────────────────────────────────────────────────────────────────
 * Internal: NVS read/write helpers
 * ──────────────────────────────────────────────────────────────────────────── */

static esp_err_t nvs_read_u32(const char *key, uint32_t *out_value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ODOMETER_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;

    err = nvs_get_u32(handle, key, out_value);
    nvs_close(handle);
    return err;
}

static esp_err_t nvs_write_u32(const char *key, uint32_t value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ODOMETER_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_odometer_init(opendash_odometer_t *odo)
{
    if (odo == NULL) return OPENDASH_ERR_INVALID_ARG;

    memset(odo, 0, sizeof(*odo));

    /* Load total odometer from NVS (0 if not found = first boot) */
    if (nvs_read_u32(NVS_KEY_TOTAL, &odo->total_meters) != ESP_OK) {
        odo->total_meters = 0;
        ESP_LOGI(TAG, "No saved odometer — starting from 0");
    } else {
        ESP_LOGI(TAG, "Loaded odometer: %lu m (%.1f km)",
                 (unsigned long)odo->total_meters,
                 (float)odo->total_meters / 1000.0f);
    }

    /* Load trip A from NVS (reset on first boot) */
    if (nvs_read_u32(NVS_KEY_TRIP_A, &odo->trip_a_meters) != ESP_OK) {
        odo->trip_a_meters = 0;
    }

    /* Load trip B from NVS */
    if (nvs_read_u32(NVS_KEY_TRIP_B, &odo->trip_b_meters) != ESP_OK) {
        odo->trip_b_meters = 0;
    }

    odo->last_nvs_save_meters = odo->total_meters;
    odo->initialized = true;

    ESP_LOGI(TAG, "Odometer initialized — total=%.1f km, trip_a=%.1f km, trip_b=%.1f km",
             opendash_odometer_get_km(odo),
             opendash_odometer_get_trip_a_km(odo),
             opendash_odometer_get_trip_b_km(odo));
    return OPENDASH_OK;
}

opendash_err_t opendash_odometer_add_distance(opendash_odometer_t *odo,
                                               uint32_t meters)
{
    if (odo == NULL || !odo->initialized) return OPENDASH_ERR_INVALID_ARG;
    if (meters == 0) return OPENDASH_OK;

    odo->total_meters  += meters;
    odo->trip_a_meters += meters;
    odo->trip_b_meters += meters;

    /* Rate-limited NVS save — write every ODOMETER_NVS_SAVE_INTERVAL_M meters */
    uint32_t unsaved = odo->total_meters - odo->last_nvs_save_meters;
    if (unsaved >= ODOMETER_NVS_SAVE_INTERVAL_M) {
        opendash_odometer_save_now(odo);
    }

    return OPENDASH_OK;
}

opendash_err_t opendash_odometer_reset_trip_a(opendash_odometer_t *odo)
{
    if (odo == NULL || !odo->initialized) return OPENDASH_ERR_INVALID_ARG;

    odo->trip_a_meters = 0;
    nvs_write_u32(NVS_KEY_TRIP_A, 0);
    ESP_LOGI(TAG, "Trip A reset");
    return OPENDASH_OK;
}

opendash_err_t opendash_odometer_reset_trip_b(opendash_odometer_t *odo)
{
    if (odo == NULL || !odo->initialized) return OPENDASH_ERR_INVALID_ARG;

    odo->trip_b_meters = 0;
    nvs_write_u32(NVS_KEY_TRIP_B, 0);
    ESP_LOGI(TAG, "Trip B reset");
    return OPENDASH_OK;
}

opendash_err_t opendash_odometer_save_now(const opendash_odometer_t *odo)
{
    if (odo == NULL || !odo->initialized) return OPENDASH_ERR_INVALID_ARG;

    nvs_write_u32(NVS_KEY_TOTAL,  odo->total_meters);
    nvs_write_u32(NVS_KEY_TRIP_A, odo->trip_a_meters);
    nvs_write_u32(NVS_KEY_TRIP_B, odo->trip_b_meters);

    /* Update last-save marker (cast away const for internal bookkeeping) */
    ((opendash_odometer_t *)odo)->last_nvs_save_meters = odo->total_meters;

    ESP_LOGD(TAG, "NVS saved: total=%lu m", (unsigned long)odo->total_meters);
    return OPENDASH_OK;
}
