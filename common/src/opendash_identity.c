/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_identity.c
 * @brief OpenDash Device Identity & Serial Number System
 *
 * Stores device identity in NVS namespace "od_identity" with keys:
 *   "node_type" (uint8_t) — the expected node type for this hardware
 *   "mac"       (blob, 6B) — stored MAC for cross-reference
 *
 * The ESP32's base MAC address serves as the unique serial number.
 * No two ESP32 chips share the same MAC, making it a reliable identifier.
 */

#include "opendash_identity.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "od_identity";

#define NVS_NAMESPACE   "od_identity"
#define NVS_KEY_NODE    "node_type"

static opendash_identity_t s_identity = {0};
static bool s_initialized = false;

const char *opendash_node_name(opendash_node_t node)
{
    switch (node) {
        case OPENDASH_NODE_CENTER:      return "CENTER";
        case OPENDASH_NODE_LEFT:        return "LEFT";
        case OPENDASH_NODE_RIGHT:       return "RIGHT";
        case OPENDASH_NODE_GPS:         return "GPS";
        case OPENDASH_NODE_BMS:         return "BMS";
        case OPENDASH_NODE_POD1:        return "POD1";
        case OPENDASH_NODE_POD2:        return "POD2";
        case OPENDASH_NODE_POD3:        return "POD3";
        case OPENDASH_NODE_POD4:        return "POD4";
        case OPENDASH_NODE_POD5:        return "POD5";
        case OPENDASH_NODE_POD6:        return "POD6";
        case OPENDASH_NODE_POD7:        return "POD7";
        case OPENDASH_NODE_POD8:        return "POD8";
        case OPENDASH_NODE_RELAY_4CH:   return "RELAY-4CH";
        case OPENDASH_NODE_RELAY_8CH_A: return "RELAY-8CH-A";
        case OPENDASH_NODE_RELAY_8CH_B: return "RELAY-8CH-B";
        case OPENDASH_NODE_MOS_4CH_A:   return "MOS-4CH-A";
        case OPENDASH_NODE_MOS_4CH_B:   return "MOS-4CH-B";
        default:                        return "UNKNOWN";
    }
}

esp_err_t opendash_identity_init(opendash_node_t compiled_node)
{
    /* Read hardware MAC address */
    esp_err_t ret = esp_read_mac(s_identity.mac, ESP_MAC_WIFI_STA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC address: %s", esp_err_to_name(ret));
        return ret;
    }

    snprintf(s_identity.serial_str, sizeof(s_identity.serial_str),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             s_identity.mac[0], s_identity.mac[1], s_identity.mac[2],
             s_identity.mac[3], s_identity.mac[4], s_identity.mac[5]);

    s_identity.compiled_node = compiled_node;

    /* Open NVS namespace */
    nvs_handle_t nvs;
    ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Try to read stored node type */
    uint8_t stored = 0;
    ret = nvs_get_u8(nvs, NVS_KEY_NODE, &stored);

    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        /* First boot — store the compiled node type */
        s_identity.first_boot = true;
        s_identity.stored_node = compiled_node;
        s_identity.identity_match = true;

        nvs_set_u8(nvs, NVS_KEY_NODE, (uint8_t)compiled_node);
        nvs_commit(nvs);

        ESP_LOGI(TAG, "First boot — identity stored: %s",
                 opendash_node_name(compiled_node));
    } else if (ret == ESP_OK) {
        /* Existing identity — compare */
        s_identity.first_boot = false;
        s_identity.stored_node = (opendash_node_t)stored;
        s_identity.identity_match = (stored == (uint8_t)compiled_node);
    } else {
        ESP_LOGE(TAG, "NVS read failed: %s", esp_err_to_name(ret));
        nvs_close(nvs);
        return ret;
    }

    nvs_close(nvs);
    s_initialized = true;

    /* ── Boot identity banner ── */
    ESP_LOGI(TAG, "┌──────────────────────────────────────────────┐");
    ESP_LOGI(TAG, "│  DEVICE IDENTITY                             │");
    ESP_LOGI(TAG, "│  Serial: %s              │", s_identity.serial_str);
    ESP_LOGI(TAG, "│  Node:   %-10s  (compiled)               │",
             opendash_node_name(compiled_node));

    if (!s_identity.identity_match) {
        ESP_LOGE(TAG, "│  ╔════════════════════════════════════════╗   │");
        ESP_LOGE(TAG, "│  ║  !! FIRMWARE / HARDWARE MISMATCH !!   ║   │");
        ESP_LOGE(TAG, "│  ║  NVS says: %-10s                 ║   │",
                 opendash_node_name(s_identity.stored_node));
        ESP_LOGE(TAG, "│  ║  Firmware:  %-10s                ║   │",
                 opendash_node_name(compiled_node));
        ESP_LOGE(TAG, "│  ║  Wrong firmware on this unit?         ║   │");
        ESP_LOGE(TAG, "│  ╚════════════════════════════════════════╝   │");
    } else {
        ESP_LOGI(TAG, "│  Status: MATCH OK                            │");
    }

    ESP_LOGI(TAG, "└──────────────────────────────────────────────┘");

    return ESP_OK;
}

esp_err_t opendash_identity_get(opendash_identity_t *id)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (id == NULL) return ESP_ERR_INVALID_ARG;
    memcpy(id, &s_identity, sizeof(opendash_identity_t));
    return ESP_OK;
}

esp_err_t opendash_identity_reset(void)
{
    nvs_handle_t nvs;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) return ret;

    nvs_erase_key(nvs, NVS_KEY_NODE);
    nvs_commit(nvs);
    nvs_close(nvs);

    s_initialized = false;
    ESP_LOGW(TAG, "Identity reset — will re-register on next boot");
    return ESP_OK;
}
