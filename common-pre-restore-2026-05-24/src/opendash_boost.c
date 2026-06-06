/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_boost.c
 * @brief OpenDash Active Boost Controller — slave-side runtime.
 *
 * See opendash_boost.h for the architectural overview. This file
 * implements the per-gear PID, safety overlay, NVS persistence and
 * thread-safe accessors. The transport layer (ESP-NOW dispatch) lives
 * in the node application — main.c on mos-4ch-a hands frames here.
 */

#include "opendash_boost.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "boost";

#define NVS_KEY_PARAMS    "params"
#define NVS_KEY_DUTY_FMT  "d_%u_%u"   /* d_<mode>_<gear> */
#define NVS_KEY_SET_FMT   "s_%u_%u"
#define NVS_KEY_THROT     "throt"

/* ────────────────────────────────────────────────────────────────────────────
 * State
 * ──────────────────────────────────────────────────────────────────────────── */

static SemaphoreHandle_t      s_lock;
static opendash_boost_params_t  s_params;
static uint8_t                  s_duty[OPENDASH_BOOST_MODES][OPENDASH_BOOST_GEARS][OPENDASH_BOOST_MAP_POINTS];
static uint16_t                 s_setp[OPENDASH_BOOST_MODES][OPENDASH_BOOST_GEARS][OPENDASH_BOOST_MAP_POINTS];
static opendash_boost_throttle_curve_t s_throt;

static opendash_boost_live_t    s_live;
static int64_t                  s_live_us = 0;        /* esp_timer_get_time() of last feed */
static opendash_boost_telemetry_t s_telem;

/* PID state */
static float    s_pid_integral;
static float    s_pid_prev_err;
static int64_t  s_pid_prev_us;

/* ────────────────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────────────────── */

static inline void lock(void)   { xSemaphoreTake(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGive(s_lock); }

static inline bool valid_mg(uint8_t mode, uint8_t gear) {
    return mode < OPENDASH_BOOST_MODES && gear < OPENDASH_BOOST_GEARS;
}

/** Map an RPM value onto the 0..15 axis with sub-bin fraction. */
static void rpm_index(uint16_t rpm, int *idx_lo, int *idx_hi, float *frac)
{
    const float bin = (float)OPENDASH_BOOST_RPM_MAX / (OPENDASH_BOOST_MAP_POINTS - 1);
    float pos = (float)rpm / bin;
    if (pos <= 0.0f)                                pos = 0.0f;
    if (pos >= OPENDASH_BOOST_MAP_POINTS - 1)       pos = OPENDASH_BOOST_MAP_POINTS - 1;
    *idx_lo = (int)pos;
    *idx_hi = (*idx_lo < OPENDASH_BOOST_MAP_POINTS - 1) ? *idx_lo + 1 : *idx_lo;
    *frac   = pos - (float)*idx_lo;
}

static float lerp_u8(const uint8_t *row, int lo, int hi, float frac)
{
    return (float)row[lo] + ((float)row[hi] - (float)row[lo]) * frac;
}
static float lerp_u16(const uint16_t *row, int lo, int hi, float frac)
{
    return (float)row[lo] + ((float)row[hi] - (float)row[lo]) * frac;
}

static inline uint8_t clamp_u8_f(float v)
{
    if (v <= 0.0f)   return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

/* ────────────────────────────────────────────────────────────────────────────
 * Defaults
 * ──────────────────────────────────────────────────────────────────────────── */

void opendash_boost_default_params(opendash_boost_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->version                = OPENDASH_BOOST_PARAMS_VERSION;
    p->mode                   = OPENDASH_BOOST_MODE_LOW;
    p->use_pid                = 1;
    p->output_channel         = 0;          /* MOS channel 0 by default */

    p->aKp = 4.0f;  p->aKi = 1.0f;   p->aKd = 0.20f;
    p->cKp = 1.0f;  p->cKi = 0.25f;  p->cKd = 0.05f;
    p->aggressive_threshold   = 0.50f;
    p->conservative_threshold = 0.85f;

    p->overboost_bar          = 1.80f;
    p->egt_warn_c             = 880.0f;
    p->egt_critical_c         = 950.0f;
    p->afr_lean_limit         = 16.0f;
    p->fuel_pressure_min_kpa  = 200.0f;

    p->throttle_min_pct       = 25;
    p->rpm_min                = 2000;
}

/** Rough spool curve: ramp on, hold near peak, taper at redline. Higher
 *  gears get more boost (more aggressive) since lower gears struggle for
 *  traction. Per-slot duty bias: LOW=0, MED=+15, HIGH=+30 (over the base
 *  shape) so the three modes are clearly distinct out of the box. */
void opendash_boost_default_duty_row(uint8_t mode, uint8_t gear, uint8_t out[OPENDASH_BOOST_MAP_POINTS])
{
    static const uint8_t shape[OPENDASH_BOOST_MAP_POINTS] = {
        0,  0,  20, 60, 110, 150, 175, 190, 200, 205, 205, 200, 190, 175, 155, 130
    };
    /* Higher gears get more — lower gears are traction-limited */
    int gear_bias = (int)gear * 6;
    int mode_bias;
    switch (mode) {
        case OPENDASH_BOOST_SLOT_HIGH: mode_bias = 30; break;
        case OPENDASH_BOOST_SLOT_MED:  mode_bias = 15; break;
        case OPENDASH_BOOST_SLOT_LOW:
        default:                       mode_bias = 0;  break;
    }
    for (int i = 0; i < OPENDASH_BOOST_MAP_POINTS; ++i) {
        int v = (int)shape[i] + gear_bias + mode_bias;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        out[i] = (uint8_t)v;
    }
}

/** Setpoint curve in centi-bar. LOW peaks ~120 cBar (1.2 BAR / 17 PSI),
 *  MED peaks ~140 cBar (1.4 BAR / 20 PSI), HIGH peaks ~160 cBar
 *  (1.6 BAR / 23 PSI). Higher gears slightly higher. */
void opendash_boost_default_setpoint_row(uint8_t mode, uint8_t gear, uint16_t out[OPENDASH_BOOST_MAP_POINTS])
{
    static const uint16_t shape_low[OPENDASH_BOOST_MAP_POINTS] = {
        0, 0, 20, 50, 80, 100, 110, 115, 118, 120, 120, 118, 115, 110, 100, 90
    };
    static const uint16_t shape_med[OPENDASH_BOOST_MAP_POINTS] = {
        0, 0, 25, 60, 95, 117, 130, 137, 140, 140, 138, 135, 130, 122, 110, 100
    };
    static const uint16_t shape_high[OPENDASH_BOOST_MAP_POINTS] = {
        0, 0, 30, 70, 110, 135, 150, 158, 160, 160, 158, 155, 150, 140, 125, 110
    };
    const uint16_t *src;
    switch (mode) {
        case OPENDASH_BOOST_SLOT_HIGH: src = shape_high; break;
        case OPENDASH_BOOST_SLOT_MED:  src = shape_med;  break;
        case OPENDASH_BOOST_SLOT_LOW:
        default:                       src = shape_low;  break;
    }
    uint16_t gear_bias = (uint16_t)gear * 4;  /* +4 cBar per gear */
    for (int i = 0; i < OPENDASH_BOOST_MAP_POINTS; ++i) {
        uint32_t v = (uint32_t)src[i] + gear_bias;
        if (v > 250) v = 250;       /* cap at 2.5 BAR */
        out[i] = (uint16_t)v;
    }
}

static void install_default_throttle_curve(opendash_boost_throttle_curve_t *c)
{
    /* No reduction once throttle ≥ ~30%, ramped pull below that. */
    static const uint16_t shape[OPENDASH_BOOST_MAP_POINTS] = {
        0, 100, 250, 450, 650, 800, 900, 950, 1000, 1000, 1000, 1000, 1000, 1000, 1000, 1000
    };
    memcpy(c->reduction_x1000, shape, sizeof(shape));
}

/* ────────────────────────────────────────────────────────────────────────────
 * NVS Persistence
 * ──────────────────────────────────────────────────────────────────────────── */

static void nvs_save_params(void)
{
    nvs_handle_t h;
    if (nvs_open(OPENDASH_BOOST_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_PARAMS, &s_params, sizeof(s_params));
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_duty_row(uint8_t mode, uint8_t gear)
{
    nvs_handle_t h;
    if (nvs_open(OPENDASH_BOOST_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16]; snprintf(key, sizeof(key), NVS_KEY_DUTY_FMT, mode, gear);
    nvs_set_blob(h, key, s_duty[mode][gear], OPENDASH_BOOST_MAP_POINTS);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_setp_row(uint8_t mode, uint8_t gear)
{
    nvs_handle_t h;
    if (nvs_open(OPENDASH_BOOST_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    char key[16]; snprintf(key, sizeof(key), NVS_KEY_SET_FMT, mode, gear);
    nvs_set_blob(h, key, s_setp[mode][gear], sizeof(s_setp[mode][gear]));
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_save_throt(void)
{
    nvs_handle_t h;
    if (nvs_open(OPENDASH_BOOST_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_THROT, &s_throt, sizeof(s_throt));
    nvs_commit(h);
    nvs_close(h);
}

static bool nvs_load_all(void)
{
    nvs_handle_t h;
    if (nvs_open(OPENDASH_BOOST_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;

    size_t len = sizeof(s_params);
    bool ok = (nvs_get_blob(h, NVS_KEY_PARAMS, &s_params, &len) == ESP_OK
               && len == sizeof(s_params)
               && s_params.version == OPENDASH_BOOST_PARAMS_VERSION);

    if (ok) {
        for (uint8_t m = 0; m < OPENDASH_BOOST_MODES; ++m) {
            for (uint8_t g = 0; g < OPENDASH_BOOST_GEARS; ++g) {
                char key[16];
                size_t l = OPENDASH_BOOST_MAP_POINTS;
                snprintf(key, sizeof(key), NVS_KEY_DUTY_FMT, m, g);
                if (nvs_get_blob(h, key, s_duty[m][g], &l) != ESP_OK
                    || l != OPENDASH_BOOST_MAP_POINTS) {
                    opendash_boost_default_duty_row(m, g, s_duty[m][g]);
                }
                l = sizeof(s_setp[m][g]);
                snprintf(key, sizeof(key), NVS_KEY_SET_FMT, m, g);
                if (nvs_get_blob(h, key, s_setp[m][g], &l) != ESP_OK
                    || l != sizeof(s_setp[m][g])) {
                    opendash_boost_default_setpoint_row(m, g, s_setp[m][g]);
                }
            }
        }
        len = sizeof(s_throt);
        if (nvs_get_blob(h, NVS_KEY_THROT, &s_throt, &len) != ESP_OK
            || len != sizeof(s_throt)) {
            install_default_throttle_curve(&s_throt);
        }
    }

    nvs_close(h);
    return ok;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t opendash_boost_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    memset(&s_live,  0, sizeof(s_live));
    memset(&s_telem, 0, sizeof(s_telem));

    if (!nvs_load_all()) {
        ESP_LOGI(TAG, "No saved boost config — installing defaults");
        opendash_boost_default_params(&s_params);
        for (uint8_t m = 0; m < OPENDASH_BOOST_MODES; ++m) {
            for (uint8_t g = 0; g < OPENDASH_BOOST_GEARS; ++g) {
                opendash_boost_default_duty_row(m, g, s_duty[m][g]);
                opendash_boost_default_setpoint_row(m, g, s_setp[m][g]);
            }
        }
        install_default_throttle_curve(&s_throt);
        nvs_save_params();
        for (uint8_t m = 0; m < OPENDASH_BOOST_MODES; ++m) {
            for (uint8_t g = 0; g < OPENDASH_BOOST_GEARS; ++g) {
                nvs_save_duty_row(m, g);
                nvs_save_setp_row(m, g);
            }
        }
        nvs_save_throt();
    } else {
        ESP_LOGI(TAG, "Loaded boost config from NVS (mode=%u, ch=%u)",
                 s_params.mode, s_params.output_channel);
    }
    return ESP_OK;
}

esp_err_t opendash_boost_set_params(const opendash_boost_params_t *p)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    if (p->version != OPENDASH_BOOST_PARAMS_VERSION) return ESP_ERR_INVALID_VERSION;
    if (p->output_channel > 3) return ESP_ERR_INVALID_ARG;
    lock();
    s_params = *p;
    nvs_save_params();
    unlock();
    /* Reset PID state when params change */
    s_pid_integral = 0;
    s_pid_prev_err = 0;
    s_pid_prev_us  = 0;
    return ESP_OK;
}

esp_err_t opendash_boost_get_params(opendash_boost_params_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    lock(); *out = s_params; unlock();
    return ESP_OK;
}

esp_err_t opendash_boost_set_duty_row(uint8_t mode, uint8_t gear,
                                       const uint8_t duty[OPENDASH_BOOST_MAP_POINTS])
{
    if (!duty || !valid_mg(mode, gear)) return ESP_ERR_INVALID_ARG;
    lock();
    memcpy(s_duty[mode][gear], duty, OPENDASH_BOOST_MAP_POINTS);
    nvs_save_duty_row(mode, gear);
    unlock();
    return ESP_OK;
}

esp_err_t opendash_boost_set_setpoint_row(uint8_t mode, uint8_t gear,
                                           const uint16_t setpoint_cbar[OPENDASH_BOOST_MAP_POINTS])
{
    if (!setpoint_cbar || !valid_mg(mode, gear)) return ESP_ERR_INVALID_ARG;
    lock();
    memcpy(s_setp[mode][gear], setpoint_cbar, sizeof(s_setp[mode][gear]));
    nvs_save_setp_row(mode, gear);
    unlock();
    return ESP_OK;
}

esp_err_t opendash_boost_get_duty_row(uint8_t mode, uint8_t gear,
                                       uint8_t out[OPENDASH_BOOST_MAP_POINTS])
{
    if (!out || !valid_mg(mode, gear)) return ESP_ERR_INVALID_ARG;
    lock();
    memcpy(out, s_duty[mode][gear], OPENDASH_BOOST_MAP_POINTS);
    unlock();
    return ESP_OK;
}

esp_err_t opendash_boost_get_setpoint_row(uint8_t mode, uint8_t gear,
                                           uint16_t out[OPENDASH_BOOST_MAP_POINTS])
{
    if (!out || !valid_mg(mode, gear)) return ESP_ERR_INVALID_ARG;
    lock();
    memcpy(out, s_setp[mode][gear], sizeof(s_setp[mode][gear]));
    unlock();
    return ESP_OK;
}

esp_err_t opendash_boost_set_throttle_curve(const opendash_boost_throttle_curve_t *c)
{
    if (!c) return ESP_ERR_INVALID_ARG;
    lock(); s_throt = *c; nvs_save_throt(); unlock();
    return ESP_OK;
}

esp_err_t opendash_boost_get_throttle_curve(opendash_boost_throttle_curve_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    lock(); *out = s_throt; unlock();
    return ESP_OK;
}

void opendash_boost_feed_live(const opendash_boost_live_t *live)
{
    if (!live) return;
    lock();
    s_live    = *live;
    s_live_us = esp_timer_get_time();
    unlock();
}

void opendash_boost_get_telemetry(opendash_boost_telemetry_t *out)
{
    if (!out) return;
    lock(); *out = s_telem; unlock();
}

/* ────────────────────────────────────────────────────────────────────────────
 * Compute step
 * ──────────────────────────────────────────────────────────────────────────── */

uint8_t opendash_boost_compute(opendash_boost_telemetry_t *out_telem)
{
    opendash_boost_telemetry_t t;
    memset(&t, 0, sizeof(t));

    lock();
    opendash_boost_params_t p = s_params;
    opendash_boost_live_t   live = s_live;
    int64_t live_age_us = esp_timer_get_time() - s_live_us;
    /* Snapshot whichever map slot we're using so we can release the lock
     * before doing FP math. Mode → slot mapping (with OFF handled above):
     *   LOW (1)  → slot 0
     *   MED (2)  → slot 1
     *   HIGH (3) → slot 2 */
    uint8_t  duty_row[OPENDASH_BOOST_MAP_POINTS];
    uint16_t setp_row[OPENDASH_BOOST_MAP_POINTS];
    uint16_t throt_row[OPENDASH_BOOST_MAP_POINTS];
    uint8_t  mode_slot;
    switch (p.mode) {
        case OPENDASH_BOOST_MODE_HIGH: mode_slot = OPENDASH_BOOST_SLOT_HIGH; break;
        case OPENDASH_BOOST_MODE_MED:  mode_slot = OPENDASH_BOOST_SLOT_MED;  break;
        case OPENDASH_BOOST_MODE_LOW:
        default:                       mode_slot = OPENDASH_BOOST_SLOT_LOW;  break;
    }
    uint8_t  gear_idx = (live.gear == 0) ? 0
                       : (live.gear > OPENDASH_BOOST_GEARS) ? (OPENDASH_BOOST_GEARS - 1)
                       : (live.gear - 1);
    memcpy(duty_row, s_duty[mode_slot][gear_idx], sizeof(duty_row));
    memcpy(setp_row, s_setp[mode_slot][gear_idx], sizeof(setp_row));
    memcpy(throt_row, s_throt.reduction_x1000, sizeof(throt_row));
    unlock();

    t.mode = p.mode;
    t.rpm  = live.rpm;
    t.gear = live.gear;
    t.boost_cbar = live.boost_cbar;

    /* ---------------- Hard cuts ---------------- */
    if (p.mode == OPENDASH_BOOST_MODE_OFF) {
        t.safety_flags |= OPENDASH_BOOST_SAFE_MODE_OFF;
        goto cut;
    }
    if (live_age_us < 0 || live_age_us > (int64_t)OPENDASH_BOOST_DATA_TIMEOUT_MS * 1000) {
        t.safety_flags |= OPENDASH_BOOST_SAFE_DATA_STALE;
        goto cut;
    }
    if (live.afr_x10 > (uint16_t)(p.afr_lean_limit * 10.0f)) {
        t.safety_flags |= OPENDASH_BOOST_SAFE_AFR_LEAN;
        goto cut;
    }
    if (live.fuel_press_kpa < (uint16_t)p.fuel_pressure_min_kpa) {
        t.safety_flags |= OPENDASH_BOOST_SAFE_FUEL_LOW;
        goto cut;
    }
    if (live.throttle_pct < p.throttle_min_pct) {
        t.safety_flags |= OPENDASH_BOOST_SAFE_THROTTLE;
        goto cut;
    }
    if (live.rpm < p.rpm_min) {
        goto cut; /* below spool — silent zero */
    }

    /* ---------------- Map lookup ---------------- */
    int lo, hi; float frac;
    rpm_index(live.rpm, &lo, &hi, &frac);
    float base_duty = lerp_u8(duty_row, lo, hi, frac);
    float setpoint  = lerp_u16(setp_row, lo, hi, frac);
    t.setpoint_cbar = (uint16_t)setpoint;

    /* ---------------- PID overlay ---------------- */
    float output = base_duty;
    if (p.use_pid && setpoint > 0.0f) {
        float err = setpoint - (float)live.boost_cbar;     /* cBar */
        float ratio = (float)live.boost_cbar / setpoint;
        bool aggr = (ratio < p.aggressive_threshold);
        bool engage = (ratio > p.aggressive_threshold * 0.5f);  /* don't fight at idle */
        t.pid_active = engage ? 1 : 0;
        t.aggressive = aggr ? 1 : 0;

        if (engage) {
            int64_t now_us = esp_timer_get_time();
            float dt = (s_pid_prev_us == 0) ? 0.05f
                      : (float)(now_us - s_pid_prev_us) / 1e6f;
            if (dt < 0.001f) dt = 0.001f;
            if (dt > 0.5f)   dt = 0.5f;
            s_pid_prev_us = now_us;

            float Kp = aggr ? p.aKp : p.cKp;
            float Ki = aggr ? p.aKi : p.cKi;
            float Kd = aggr ? p.aKd : p.cKd;

            s_pid_integral += err * dt;
            /* Anti-windup */
            if (s_pid_integral >  500.0f) s_pid_integral =  500.0f;
            if (s_pid_integral < -500.0f) s_pid_integral = -500.0f;

            float deriv = (err - s_pid_prev_err) / dt;
            s_pid_prev_err = err;

            output = base_duty + (Kp * err + Ki * s_pid_integral + Kd * deriv);
        } else {
            s_pid_integral = 0;
            s_pid_prev_err = err;
        }
    }

    /* ---------------- Safety attenuation ---------------- */
    float attenuation = 1.0f;
    if ((float)live.boost_cbar > p.overboost_bar * 100.0f) {
        attenuation *= 0.50f;
        t.safety_flags |= OPENDASH_BOOST_SAFE_OVERBOOST;
    }
    if ((float)live.egt_c > p.egt_critical_c) {
        attenuation *= 0.50f;
        t.safety_flags |= OPENDASH_BOOST_SAFE_EGT_CRIT;
    } else if ((float)live.egt_c > p.egt_warn_c) {
        attenuation *= 0.75f;
        t.safety_flags |= OPENDASH_BOOST_SAFE_EGT_WARN;
    }

    /* ---------------- Throttle reduction curve ---------------- */
    int t_lo = live.throttle_pct / 7;       /* 0..100 mapped onto 0..15 */
    if (t_lo > OPENDASH_BOOST_MAP_POINTS - 1) t_lo = OPENDASH_BOOST_MAP_POINTS - 1;
    float t_factor = (float)throt_row[t_lo] / 1000.0f;
    output *= t_factor;
    output *= attenuation;

    t.duty = clamp_u8_f(output);
    lock(); s_telem = t; unlock();
    if (out_telem) *out_telem = t;
    return t.duty;

cut:
    s_pid_integral = 0;
    s_pid_prev_err = 0;
    t.duty = 0;
    lock(); s_telem = t; unlock();
    if (out_telem) *out_telem = t;
    return 0;
}
