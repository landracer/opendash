/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_obd_config.c
 * @brief OBD2 feature configuration — NVS persistence implementation
 */

#include "opendash_obd_config.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "obd_config";
#define NVS_NAMESPACE "obd_cfg"
#define NVS_KEY_BLOB  "cfg_v1"

static obd_config_t s_config;
static bool s_loaded = false;

/** Default warning thresholds (American units: °F, PSI) */
static const obd_config_t s_defaults = {
    .obd_enabled = false,
    .mil_indicator_enabled = true,
    .warnings = {
        [OBD_WARN_COOLANT_TEMP] = { .caution = 220.0f, .critical = 240.0f, .above = true },
        [OBD_WARN_OIL_TEMP]     = { .caution = 260.0f, .critical = 280.0f, .above = true },
        [OBD_WARN_OIL_PRESSURE] = { .caution = 20.0f,  .critical = 10.0f,  .above = false },
        [OBD_WARN_BATTERY_VOLT] = { .caution = 12.0f,  .critical = 11.0f,  .above = false },
        [OBD_WARN_BOOST_PSI]    = { .caution = 18.0f,  .critical = 22.0f,  .above = true },
        [OBD_WARN_AFR]          = { .caution = 0.0f,    .critical = 0.0f,   .above = true },  /* disabled */
    }
};

void obd_config_load(void)
{
    memcpy(&s_config, &s_defaults, sizeof(obd_config_t));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No OBD config in NVS, using defaults");
        s_loaded = true;
        return;
    }

    size_t len = sizeof(obd_config_t);
    err = nvs_get_blob(h, NVS_KEY_BLOB, &s_config, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(obd_config_t)) {
        ESP_LOGW(TAG, "NVS blob read failed or size mismatch, using defaults");
        memcpy(&s_config, &s_defaults, sizeof(obd_config_t));
    } else {
        ESP_LOGI(TAG, "OBD config loaded from NVS (obd_enabled=%d, mil=%d)",
                 s_config.obd_enabled, s_config.mil_indicator_enabled);
    }
    s_loaded = true;
}

void obd_config_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for write: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_blob(h, NVS_KEY_BLOB, &s_config, sizeof(obd_config_t));
    if (err == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "OBD config saved to NVS");
    } else {
        ESP_LOGE(TAG, "NVS blob write failed: %s", esp_err_to_name(err));
    }
    nvs_close(h);
}

const obd_config_t *obd_config_get(void)
{
    if (!s_loaded) obd_config_load();
    return &s_config;
}

bool obd_config_toggle_enabled(void)
{
    s_config.obd_enabled = !s_config.obd_enabled;
    obd_config_save();
    ESP_LOGI(TAG, "OBD enabled toggled → %s", s_config.obd_enabled ? "ON" : "OFF");
    return s_config.obd_enabled;
}

bool obd_config_toggle_mil_indicator(void)
{
    s_config.mil_indicator_enabled = !s_config.mil_indicator_enabled;
    obd_config_save();
    ESP_LOGI(TAG, "MIL indicator toggled → %s", s_config.mil_indicator_enabled ? "ON" : "OFF");
    return s_config.mil_indicator_enabled;
}

void obd_config_set_warning(obd_warn_sensor_t sensor,
                             float caution, float critical, bool above)
{
    if (sensor >= OBD_WARN_THRESHOLD_COUNT) return;
    s_config.warnings[sensor].caution  = caution;
    s_config.warnings[sensor].critical = critical;
    s_config.warnings[sensor].above    = above;
    obd_config_save();
    ESP_LOGI(TAG, "Warning threshold [%d] set: caution=%.1f critical=%.1f above=%d",
             sensor, caution, critical, above);
}
