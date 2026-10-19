/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file channel_registry.c
 * @brief Implementation of the center-side persistent channel registry.
 *
 * Factory defaults below mirror the physical harness of the chassis as built;
 * every button is re-bindable on screen (long-press a panel box) or over the
 * serial console without ever touching firmware.
 */

#include "channel_registry.h"

#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "chan_reg";

#define NVS_NAMESPACE   "chan_reg"
#define NVS_KEY_MAP     "cmap"
#define NVS_KEY_AUTO    "auto"
#define BLOB_MAGIC      (0x4F44)  /* 'OD' — our own format magic */
#define BLOB_VERSION    (1)

typedef struct {
    uint16_t          magic;
    uint8_t           version;
    channel_binding_t entry[CHANNEL_REGISTRY_COUNT];
} chan_map_blob_t;

/* ── Factory defaults ─────────────────────────────────────────────────── */

static const channel_binding_t s_defaults[CHANNEL_REGISTRY_COUNT] = {
    /* Buttons 0-3: 4-CH HD relay (fans/pumps) */
    { .label = "RAD FAN 1",  .node = OPENDASH_NODE_RELAY_4CH,   .channel = 0, .is_on = false },
    { .label = "RAD FAN 2",  .node = OPENDASH_NODE_RELAY_4CH,   .channel = 1, .is_on = false },
    { .label = "WATER PUMP", .node = OPENDASH_NODE_RELAY_4CH,   .channel = 2, .is_on = false },
    { .label = "FUEL PUMP",  .node = OPENDASH_NODE_RELAY_4CH,   .channel = 3, .is_on = false },
    /* Buttons 4-11: RELAY 8CH A */
    { .label = "R-A CH1",    .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 0, .is_on = false },
    { .label = "R-A CH2",    .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 1, .is_on = false },
    { .label = "R-A CH3",    .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 2, .is_on = false },
    { .label = "R-A CH4",    .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 3, .is_on = false },
    { .label = "R-A CH5",    .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 4, .is_on = false },
    { .label = "R-A CH6",    .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 5, .is_on = false },
    { .label = "R-A CH7",    .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 6, .is_on = false },
    { .label = "R-A CH8",    .node = OPENDASH_NODE_RELAY_8CH_A, .channel = 7, .is_on = false },
    /* Buttons 12-19: RELAY 8CH B */
    { .label = "R-B CH1",    .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 0, .is_on = false },
    { .label = "R-B CH2",    .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 1, .is_on = false },
    { .label = "R-B CH3",    .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 2, .is_on = false },
    { .label = "R-B CH4",    .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 3, .is_on = false },
    { .label = "R-B CH5",    .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 4, .is_on = false },
    { .label = "R-B CH6",    .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 5, .is_on = false },
    { .label = "R-B CH7",    .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 6, .is_on = false },
    { .label = "R-B CH8",    .node = OPENDASH_NODE_RELAY_8CH_B, .channel = 7, .is_on = false },
    /* Buttons 20-23: MOS-FET A */
    { .label = "MOS-A CH1",  .node = OPENDASH_NODE_MOS_4CH_A,   .channel = 0, .is_on = false },
    { .label = "MOS-A CH2",  .node = OPENDASH_NODE_MOS_4CH_A,   .channel = 1, .is_on = false },
    { .label = "MOS-A CH3",  .node = OPENDASH_NODE_MOS_4CH_A,   .channel = 2, .is_on = false },
    { .label = "MOS-A CH4",  .node = OPENDASH_NODE_MOS_4CH_A,   .channel = 3, .is_on = false },
    /* Buttons 24-27: MOS-FET B */
    { .label = "MOS-B CH1",  .node = OPENDASH_NODE_MOS_4CH_B,   .channel = 0, .is_on = false },
    { .label = "MOS-B CH2",  .node = OPENDASH_NODE_MOS_4CH_B,   .channel = 1, .is_on = false },
    { .label = "MOS-B CH3",  .node = OPENDASH_NODE_MOS_4CH_B,   .channel = 2, .is_on = false },
    { .label = "MOS-B CH4",  .node = OPENDASH_NODE_MOS_4CH_B,   .channel = 3, .is_on = false },
};

/* ── RAM image + persistence ─────────────────────────────────────────── */

static channel_binding_t s_entries[CHANNEL_REGISTRY_COUNT];
static bool s_loaded     = false;
static bool s_panel_auto = true;  /* boot to panel until engine runs (default) */

static esp_err_t registry_persist(void)
{
    if (!s_loaded) return ESP_ERR_INVALID_STATE;

    chan_map_blob_t blob = { .magic = BLOB_MAGIC, .version = BLOB_VERSION };
    memcpy(blob.entry, s_entries, sizeof(s_entries));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_MAP, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t channel_registry_init(void)
{
    /* Defaults first so every getter works even with an empty/corrupt NVS. */
    memcpy(s_entries, s_defaults, sizeof(s_entries));

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        chan_map_blob_t blob;
        size_t sz = sizeof(blob);
        if (nvs_get_blob(h, NVS_KEY_MAP, &blob, &sz) == ESP_OK &&
            sz == sizeof(blob) && blob.magic == BLOB_MAGIC &&
            blob.version == BLOB_VERSION) {
            memcpy(s_entries, blob.entry, sizeof(s_entries));
            ESP_LOGI(TAG, "Channel registry loaded from NVS (%zu bytes)", sz);
        } else {
            ESP_LOGI(TAG, "No valid stored channel map — using defaults");
        }

        uint8_t auto_v = 1;
        size_t  asz = sizeof(auto_v);
        if (nvs_get_blob(h, NVS_KEY_AUTO, &auto_v, &asz) == ESP_OK && auto_v <= 1) {
            s_panel_auto = (auto_v == 1);
        }
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "No chan_reg namespace yet — defaults active");
    }

    s_loaded = true;
    ESP_LOGI(TAG, "Boot-to-panel routing: %s", s_panel_auto ? "AUTO" : "MANUAL");
    return ESP_OK;
}

const channel_binding_t *channel_registry_get(int idx)
{
    if (idx < 0 || idx >= CHANNEL_REGISTRY_COUNT) return NULL;
    return &s_entries[idx];
}

const char *channel_registry_node_short(opendash_node_t node)
{
    switch (node) {
        case OPENDASH_NODE_RELAY_4CH:   return "RLY4";
        case OPENDASH_NODE_RELAY_8CH_A: return "RLY-A";
        case OPENDASH_NODE_RELAY_8CH_B: return "RLY-B";
        case OPENDASH_NODE_MOS_4CH_A:   return "MOS-A";
        case OPENDASH_NODE_MOS_4CH_B:   return "MOS-B";
        default:                        return "???";
    }
}

void channel_registry_format_label(char *out, size_t sz, opendash_node_t node, uint8_t channel)
{
    snprintf(out, sz, "%s CH%" PRIu8, channel_registry_node_short(node),
             (uint8_t)(channel + 1));
}

esp_err_t channel_registry_set_binding(int idx, opendash_node_t node, uint8_t channel)
{
    if (idx < 0 || idx >= CHANNEL_REGISTRY_COUNT) return ESP_ERR_INVALID_ARG;
    if (!OPENDASH_NODE_IS_RELAY(node))            return ESP_ERR_INVALID_ARG;

    s_entries[idx].node    = node;
    s_entries[idx].channel = channel;
    channel_registry_format_label(s_entries[idx].label, sizeof(s_entries[idx].label),
                                  node, channel);
    /* Bound channel keeps its cached state — the wire state didn't change. */
    esp_err_t err = registry_persist();
    ESP_LOGI(TAG, "Button %d rebound → %s (save=%s)", idx, s_entries[idx].label,
             esp_err_to_name(err));
    return err;
}

esp_err_t channel_registry_set_label(int idx, const char *label)
{
    if (idx < 0 || idx >= CHANNEL_REGISTRY_COUNT || !label || label[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    char clean[CHANNEL_LABEL_MAX];
    size_t j = 0;
    for (size_t i = 0; label[i] != '\0' && j < CHANNEL_LABEL_MAX - 1; i++) {
        clean[j++] = (char)toupper((unsigned char)label[i]);
    }
    clean[j] = '\0';

    memset(s_entries[idx].label, 0, sizeof(s_entries[idx].label));
    memcpy(s_entries[idx].label, clean, strlen(clean) + 1);

    esp_err_t err = registry_persist();
    ESP_LOGI(TAG, "Button %d relabeled → '%s' (save=%s)", idx, clean, esp_err_to_name(err));
    return err;
}

void channel_registry_set_state(int idx, bool on)
{
    if (idx < 0 || idx >= CHANNEL_REGISTRY_COUNT) return;
    s_entries[idx].is_on = on;
}

int channel_registry_find(opendash_node_t node, uint8_t channel)
{
    for (int i = 0; i < CHANNEL_REGISTRY_COUNT; i++) {
        if (s_entries[i].node == node && s_entries[i].channel == channel) return i;
    }
    return -1;
}

esp_err_t channel_registry_reset_defaults(void)
{
    memcpy(s_entries, s_defaults, sizeof(s_entries));
    esp_err_t err = registry_persist();
    ESP_LOGW(TAG, "Channel registry reset to defaults (save=%s)", esp_err_to_name(err));
    return err;
}

bool channel_registry_panel_auto(void)
{
    return s_panel_auto;
}

esp_err_t channel_registry_set_panel_auto(bool on)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    uint8_t v = on ? 1 : 0;
    err = nvs_set_blob(h, NVS_KEY_AUTO, &v, sizeof(v));
    if (err == ESP_OK) {
        err = nvs_commit(h);
        s_panel_auto = on;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Boot-to-panel routing %s (save=%s)", on ? "ENABLED" : "DISABLED",
             esp_err_to_name(err));
    return err;
}
