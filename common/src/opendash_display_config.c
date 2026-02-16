/**
 * @file opendash_display_config.c
 * @brief OpenDash Display Configuration Implementation
 *
 * Implements the display configuration API for loading, saving, and
 * managing display layouts using ESP-IDF's NVS storage.
 *
 * @see opendash_display_config.h for API documentation.
 */

#include "opendash_display_config.h"
#include "opendash_data_model.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "od_config";

/** @brief NVS key for storing display layout (max 15 chars). */
#define NVS_KEY_LAYOUT  "layout"

/* ────────────────────────────────────────────────────────────────────────────
 * Default Configurations per Node Type
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Get default layout for Center display (4.3" 800x480).
 */
static void get_center_defaults(opendash_display_layout_t *layout)
{
    memset(layout, 0, sizeof(*layout));

    /* Center display: 4 main sections + status bar */
    layout->num_sections = 4;
    layout->brightness = 200;
    layout->theme = 0;
    layout->use_metric = false;

    /* Section 0: RPM (large arc gauge in center) */
    layout->sections[0].data_point_id = OPENDASH_DP_RPM;
    layout->sections[0].display_mode = OPENDASH_DISP_ARC;
    layout->sections[0].font_size = 2;  /* Large */

    /* Section 1: Speed */
    layout->sections[1].data_point_id = OPENDASH_DP_VEHICLE_SPEED;
    layout->sections[1].display_mode = OPENDASH_DISP_NUMERIC;
    layout->sections[1].font_size = 2;

    /* Section 2: Coolant temperature */
    layout->sections[2].data_point_id = OPENDASH_DP_COOLANT_TEMP;
    layout->sections[2].display_mode = OPENDASH_DISP_NUMERIC;
    layout->sections[2].font_size = 1;  /* Medium */

    /* Section 3: Fuel level */
    layout->sections[3].data_point_id = OPENDASH_DP_FUEL_LEVEL;
    layout->sections[3].display_mode = OPENDASH_DISP_BAR;
    layout->sections[3].font_size = 1;
}

/**
 * @brief Get default layout for Left/Right gauge pod (2.8" round).
 */
static void get_gauge_defaults(opendash_display_layout_t *layout, opendash_node_t node)
{
    memset(layout, 0, sizeof(*layout));

    /* Round displays: single main gauge + optional secondary readout */
    layout->num_sections = 2;
    layout->brightness = 200;
    layout->theme = 0;
    layout->use_metric = false;

    if (node == OPENDASH_NODE_LEFT) {
        /* Left pod: Oil pressure primary */
        layout->sections[0].data_point_id = OPENDASH_DP_OIL_PRESSURE;
        layout->sections[0].display_mode = OPENDASH_DISP_ARC;
        layout->sections[0].font_size = 2;

        layout->sections[1].data_point_id = OPENDASH_DP_OIL_TEMP;
        layout->sections[1].display_mode = OPENDASH_DISP_NUMERIC;
        layout->sections[1].font_size = 0;  /* Small */
    } else {
        /* Right pod: Boost/vacuum primary */
        layout->sections[0].data_point_id = OPENDASH_DP_BOOST_PRESSURE;
        layout->sections[0].display_mode = OPENDASH_DISP_ARC;
        layout->sections[0].font_size = 2;

        layout->sections[1].data_point_id = OPENDASH_DP_AFR;
        layout->sections[1].display_mode = OPENDASH_DISP_NUMERIC;
        layout->sections[1].font_size = 0;
    }
}

/**
 * @brief Get default layout for GPS unit (1.75" AMOLED).
 */
static void get_gps_defaults(opendash_display_layout_t *layout)
{
    memset(layout, 0, sizeof(*layout));

    /* GPS display: lap timer focused layout */
    layout->num_sections = 3;
    layout->brightness = 255;  /* Max brightness for outdoor visibility */
    layout->theme = 0;
    layout->use_metric = false;

    /* Section 0: Current lap time */
    layout->sections[0].data_point_id = OPENDASH_DP_LAP_TIME;
    layout->sections[0].display_mode = OPENDASH_DISP_NUMERIC;
    layout->sections[0].font_size = 2;

    /* Section 1: Best lap time */
    layout->sections[1].data_point_id = OPENDASH_DP_BEST_LAP_TIME;
    layout->sections[1].display_mode = OPENDASH_DISP_NUMERIC;
    layout->sections[1].font_size = 1;

    /* Section 2: Delta to best */
    layout->sections[2].data_point_id = OPENDASH_DP_LAP_DELTA;
    layout->sections[2].display_mode = OPENDASH_DISP_NUMERIC;
    layout->sections[2].font_size = 1;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API Implementation
 * ──────────────────────────────────────────────────────────────────────────── */

opendash_err_t opendash_config_load(opendash_node_t node,
                                     opendash_display_layout_t *layout)
{
    if (layout == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OPENDASH_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS namespace not found, using defaults");
        return opendash_config_reset_defaults(node, layout);
    }

    size_t required_size = sizeof(opendash_display_layout_t);
    err = nvs_get_blob(handle, NVS_KEY_LAYOUT, layout, &required_size);
    nvs_close(handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved config, loading defaults for node %d", node);
        return opendash_config_reset_defaults(node, layout);
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error reading config: %s", esp_err_to_name(err));
        return OPENDASH_ERR_GENERAL;
    }

    ESP_LOGI(TAG, "Loaded config: %d sections, brightness=%d",
             layout->num_sections, layout->brightness);
    return OPENDASH_OK;
}

opendash_err_t opendash_config_save(const opendash_display_layout_t *layout)
{
    if (layout == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OPENDASH_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return OPENDASH_ERR_GENERAL;
    }

    err = nvs_set_blob(handle, NVS_KEY_LAYOUT, layout, sizeof(opendash_display_layout_t));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write config: %s", esp_err_to_name(err));
        nvs_close(handle);
        return OPENDASH_ERR_GENERAL;
    }

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to commit config: %s", esp_err_to_name(err));
        return OPENDASH_ERR_GENERAL;
    }

    ESP_LOGI(TAG, "Configuration saved to NVS");
    return OPENDASH_OK;
}

opendash_err_t opendash_config_reset_defaults(opendash_node_t node,
                                               opendash_display_layout_t *layout)
{
    if (layout == NULL) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    switch (node) {
        case OPENDASH_NODE_CENTER:
            get_center_defaults(layout);
            break;
        case OPENDASH_NODE_LEFT:
        case OPENDASH_NODE_RIGHT:
            get_gauge_defaults(layout, node);
            break;
        case OPENDASH_NODE_GPS:
            get_gps_defaults(layout);
            break;
        default:
            ESP_LOGW(TAG, "Unknown node type %d, using center defaults", node);
            get_center_defaults(layout);
            break;
    }

    ESP_LOGI(TAG, "Loaded default config for node %d: %d sections",
             node, layout->num_sections);
    return OPENDASH_OK;
}

opendash_err_t opendash_config_set_section(opendash_display_layout_t *layout,
                                            uint8_t section,
                                            uint16_t dp_id,
                                            opendash_display_mode_t mode)
{
    if (layout == NULL || section >= layout->num_sections) {
        return OPENDASH_ERR_INVALID_ARG;
    }

    layout->sections[section].data_point_id = dp_id;
    layout->sections[section].display_mode = (uint8_t)mode;

    /* Auto-save after modification */
    return opendash_config_save(layout);
}
