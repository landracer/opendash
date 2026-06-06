/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
#include "opendash_layout_store.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

#define TAG "layout_store"

/**
 * @brief NVS namespace for layout storage
 */
#define LAYOUT_NVS_NAMESPACE "od_layout"

esp_err_t opendash_layout_load(uint8_t mode, screen_layout_v1_t *out)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    err = nvs_open(LAYOUT_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    char key[4];
    snprintf(key, sizeof(key), "m%u", mode);
    
    size_t required_size = sizeof(screen_layout_v1_t);
    err = nvs_get_blob(nvs_handle, key, out, &required_size);
    
    nvs_close(nvs_handle);
    
    if (err != ESP_OK) {
        return err;
    }
    
    // Validate version
    if (out->version != 0x01) {
        ESP_LOGW(TAG, "Layout version %d not supported, using defaults", out->version);
        return ESP_ERR_INVALID_VERSION;
    }
    
    return ESP_OK;
}

esp_err_t opendash_layout_save(uint8_t mode, const screen_layout_v1_t *in)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    err = nvs_open(LAYOUT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    char key[4];
    snprintf(key, sizeof(key), "m%u", mode);
    
    err = nvs_set_blob(nvs_handle, key, in, sizeof(screen_layout_v1_t));
    
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    return err;
}

esp_err_t opendash_layout_load_or_default(uint8_t mode,
                                          const screen_layout_v1_t *defaults,
                                          screen_layout_v1_t *out)
{
    if (defaults == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Try to load from NVS
    esp_err_t err = opendash_layout_load(mode, out);
    
    if (err == ESP_OK) {
        // Successfully loaded from NVS
        return ESP_OK;
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // No layout saved yet, use defaults
        memcpy(out, defaults, sizeof(screen_layout_v1_t));
        // Save defaults to NVS for next boot
        return opendash_layout_save(mode, out);
    } else {
        // Other error, return the error
        return err;
    }
}

esp_err_t opendash_layout_factory_reset(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;
    
    err = nvs_open(LAYOUT_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }
    
    err = nvs_erase_all(nvs_handle);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    
    nvs_close(nvs_handle);
    
    return err;
}