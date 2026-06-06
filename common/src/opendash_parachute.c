/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_parachute.c
 * @brief OpenDash Parachute / Deployment — config store (MOS-side).
 *
 * Holds the authoritative, NVS-persisted deployment configuration for a MOS
 * actuator node. Center pushes updates over ESP-NOW; this module validates,
 * caches, and persists them. See opendash_parachute.h for the schema and
 * opendash_parachute_actuator.c for the armed/fire state machine.
 *
 * NOTE: ARM state lives in the actuator (transient), NOT in this config —
 * a reboot always comes up DISARMED for safety.
 */

#include "opendash_parachute.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "parachute_cfg";

#define NVS_NAMESPACE  "parachute"
#define NVS_KEY_CONFIG "cfg"

static SemaphoreHandle_t          s_lock;
static opendash_parachute_config_t s_cfg;

static inline void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* ────────────────────────────────────────────────────────────────────────────
 * Validation / clamping — keep a pushed config inside sane bounds so a bad
 * frame can never install a nonsense threshold.
 * ──────────────────────────────────────────────────────────────────────────── */
static void sanitize(opendash_parachute_config_t *c)
{
    c->version      = OPENDASH_PARACHUTE_CONFIG_VERSION;
    c->enabled      = c->enabled ? 1 : 0;
    c->channel_mask &= 0x0F;                 /* only CH1..CH4 exist */
    c->flags        &= OPENDASH_PARACHUTE_FLAG_MASK;  /* drop unknown bits */

    if (!(c->min_speed_mph   >= 0.0f))   c->min_speed_mph   = OPENDASH_PARACHUTE_MIN_SPEED_MPH;
    if (c->min_speed_mph     > 500.0f)   c->min_speed_mph   = 500.0f;
    if (!(c->roll_deploy_deg >= 0.0f))   c->roll_deploy_deg = OPENDASH_PARACHUTE_ROLL_DEPLOY_DEG;
    if (c->roll_deploy_deg   > 180.0f)   c->roll_deploy_deg = 180.0f;
    if (!(c->roll_rate_deg_s >= 0.0f))   c->roll_rate_deg_s = OPENDASH_PARACHUTE_ROLL_RATE_DEG_S;
    if (c->roll_rate_deg_s   > 2000.0f)  c->roll_rate_deg_s = 2000.0f;
    if (c->sustain_ms        > 5000)     c->sustain_ms      = 5000;
    if (c->pulse_ms          > 10000)    c->pulse_ms        = 10000;
}

void opendash_parachute_config_default(opendash_parachute_config_t *cfg)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->version        = OPENDASH_PARACHUTE_CONFIG_VERSION;
    cfg->enabled        = OPENDASH_PARACHUTE_DEFAULT_ENABLED;
    cfg->channel_mask   = OPENDASH_PARACHUTE_DEFAULT_CHANNEL_MASK;
    cfg->min_speed_mph  = OPENDASH_PARACHUTE_MIN_SPEED_MPH;
    cfg->roll_deploy_deg= OPENDASH_PARACHUTE_ROLL_DEPLOY_DEG;
    cfg->roll_rate_deg_s= OPENDASH_PARACHUTE_ROLL_RATE_DEG_S;
    cfg->sustain_ms     = OPENDASH_PARACHUTE_ROLL_SUSTAIN_MS;
    cfg->pulse_ms       = OPENDASH_PARACHUTE_DEPLOY_PULSE_MS;
}

static void nvs_save(const opendash_parachute_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open(rw) failed — config not persisted");
        return;
    }
    if (nvs_set_blob(h, NVS_KEY_CONFIG, cfg, sizeof(*cfg)) == ESP_OK) {
        nvs_commit(h);
    }
    nvs_close(h);
}

static bool nvs_load(opendash_parachute_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = sizeof(*cfg);
    bool ok = (nvs_get_blob(h, NVS_KEY_CONFIG, cfg, &len) == ESP_OK
               && len == sizeof(*cfg)
               && cfg->version == OPENDASH_PARACHUTE_CONFIG_VERSION);
    nvs_close(h);
    return ok;
}

esp_err_t opendash_parachute_config_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    lock();
    if (nvs_load(&s_cfg)) {
        sanitize(&s_cfg);
        ESP_LOGI(TAG, "Loaded persisted deploy config (v%u)", s_cfg.version);
    } else {
        opendash_parachute_config_default(&s_cfg);
        nvs_save(&s_cfg);
        ESP_LOGI(TAG, "No saved deploy config — installed safe defaults");
    }
    unlock();
    return ESP_OK;
}

esp_err_t opendash_parachute_config_get(opendash_parachute_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    lock();
    *out = s_cfg;
    unlock();
    return ESP_OK;
}

esp_err_t opendash_parachute_config_set(const opendash_parachute_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;
    opendash_parachute_config_t c = *cfg;
    sanitize(&c);

    lock();
    s_cfg = c;
    nvs_save(&s_cfg);
    unlock();
    return ESP_OK;
}
