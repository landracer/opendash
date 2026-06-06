/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file system_config.c
 * @brief Implementation of center-side persistent system configuration.
 */

#include "system_config.h"

#include <string.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "system_config";

#define NVS_NAMESPACE   "boost_ui"
#define NVS_KEY_TARGET  "boost_target"
#define NVS_KEY_PUNIT   "press_unit"

opendash_node_t          g_boost_target_node   = OPENDASH_NODE_MOS_4CH_A;
opendash_pressure_unit_t g_boost_pressure_unit = OPENDASH_PRESSURE_PSI;

esp_err_t system_config_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved boost target — defaulting to MOS_4CH_A");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t v = (uint8_t)g_boost_target_node;
    size_t  sz = sizeof(v);
    err = nvs_get_blob(h, NVS_KEY_TARGET, &v, &sz);
    nvs_close(h);

    if (err == ESP_OK && v < OPENDASH_NODE_COUNT) {
        g_boost_target_node = (opendash_node_t)v;
        ESP_LOGI(TAG, "Loaded boost target = %u", v);
    } else if (err == ESP_OK) {
        ESP_LOGW(TAG, "Stored boost target %u out of range — using default", v);
    }

    /* Pressure unit (best-effort; default already set). */
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        uint8_t u = (uint8_t)g_boost_pressure_unit;
        size_t  usz = sizeof(u);
        if (nvs_get_blob(h, NVS_KEY_PUNIT, &u, &usz) == ESP_OK && u <= OPENDASH_PRESSURE_PSI) {
            g_boost_pressure_unit = (opendash_pressure_unit_t)u;
            ESP_LOGI(TAG, "Loaded pressure unit = %u", u);
        }
        nvs_close(h);
    }
    return ESP_OK;
}

esp_err_t system_config_save_boost_target(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t v = (uint8_t)g_boost_target_node;
    err = nvs_set_blob(h, NVS_KEY_TARGET, &v, sizeof(v));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved boost target = %u", v);
    } else {
        ESP_LOGW(TAG, "Save failed: %s", esp_err_to_name(err));
    }
    return err;
}

esp_err_t system_config_save_pressure_unit(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    uint8_t v = (uint8_t)g_boost_pressure_unit;
    err = nvs_set_blob(h, NVS_KEY_PUNIT, &v, sizeof(v));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved pressure unit = %u", v);
    }
    return err;
}
