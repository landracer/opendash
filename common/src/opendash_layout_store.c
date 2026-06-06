/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_layout_store.c
 * @brief NVS persistence for screen layouts.
 *
 * @see PER_NODE_DISPLAY_CONFIG_SPEC.md §4.3
 */

#include "opendash_layout_store.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "od_layout_store";

static void make_key(uint8_t mode, char out[8])
{
    /* "m255" + NUL fits in 5 chars; round to 8 for safety. */
    snprintf(out, 8, "m%u", (unsigned)mode);
}

esp_err_t opendash_layout_load(uint8_t mode, screen_layout_v1_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(OPENDASH_LAYOUT_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    char key[8];
    make_key(mode, key);

    size_t sz = sizeof(*out);
    err = nvs_get_blob(h, key, out, &sz);
    nvs_close(h);

    if (err != ESP_OK) {
        return err;
    }
    if (sz != sizeof(*out)) {
        ESP_LOGW(TAG, "Layout blob size mismatch (got %u, want %u) for %s",
                 (unsigned)sz, (unsigned)sizeof(*out), key);
        return ESP_ERR_INVALID_SIZE;
    }
    if (out->version != OPENDASH_LAYOUT_VERSION) {
        ESP_LOGW(TAG, "Unsupported layout version %u for %s",
                 (unsigned)out->version, key);
        return ESP_ERR_INVALID_VERSION;
    }
    return ESP_OK;
}

esp_err_t opendash_layout_save(uint8_t mode, const screen_layout_v1_t *in)
{
    if (!in) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(OPENDASH_LAYOUT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    char key[8];
    make_key(mode, key);

    err = nvs_set_blob(h, key, in, sizeof(*in));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved layout %s (arc=0x%04X, slots=%u)",
                 key, in->arc_dp_id, in->slot_count);
    }
    return err;
}

esp_err_t opendash_layout_load_or_default(uint8_t mode,
                                          const screen_layout_v1_t *defaults,
                                          screen_layout_v1_t *out)
{
    if (!defaults || !out) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = opendash_layout_load(mode, out);
    if (err == ESP_OK) {
        return ESP_OK;
    }

    /* Anything other than NVS_NOT_FOUND we still recover from by
     * falling back to defaults — better to render *something* than to
     * brick the screen. */
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Layout load m%u failed (0x%x), using defaults",
                 (unsigned)mode, err);
    }

    memcpy(out, defaults, sizeof(*out));
    /* Persist defaults so user has a known baseline to edit from. */
    return opendash_layout_save(mode, out);
}

esp_err_t opendash_layout_factory_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(OPENDASH_LAYOUT_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "All saved layouts erased");
    }
    return err;
}
