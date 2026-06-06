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
    p->mode                   = OPENDASH_BOOST_MODE_NORMAL;
    p->use_pid                = 1;
    p->output_mask            = 0x08;       /* CH4 by default (legacy N75 wiring) */

    p->aKp = 4.0f;  p->aKi = 1.0f;   p->aKd = 0.20f;
    p->cKp = 1.0f;  p->cKi = 0.25f;  p->cKd = 0.05f;
    p->aggressive_threshold   = 0.50f;
    p->conservative_threshold = 0.85f;

    p->overboost_bar          = 2.80f;          /* Hard cut above 36 PSI RACE peak + headroom */
    p->egt_warn_c             = 880.0f;
    p->egt_critical_c         = 950.0f;
    p->afr_lean_limit         = 16.0f;
    p->fuel_pressure_min_kpa  = 200.0f;

    p->throttle_min_pct       = 25;
    p->rpm_min                = 2000;
}

/** Rough spool curve: ramp on, hold near peak, taper at redline. Higher
 *  gears get more boost (more aggressive) since lower gears struggle for
 *  traction. RACE slot adds ~30 PWM counts on top of NORMAL.
 *
 *  Axis is 32 points across 0..OPENDASH_BOOST_RPM_MAX (16,000) RPM,
 *  presented in the UI as two pages of 16. Spool ramp lives in roughly
 *  the first half (i.e. ~0..8000 RPM), with a long high-RPM taper.
 */
void opendash_boost_default_duty_row(uint8_t mode, uint8_t gear, uint8_t out[OPENDASH_BOOST_MAP_POINTS])
{
    static const uint8_t shape[OPENDASH_BOOST_MAP_POINTS] = {
        /* 0    500   1k   1.5k  2k    2.5k  3k    3.5k  4k    4.5k  5k    5.5k  6k    6.5k  7k    7.5k */
          0,     0,   10,   30,   60,   95,  130,  155,  175,  185,  195,  200,  202,  205,  205,  205,
        /* 8k   8.5k  9k    9.5k  10k   10.5k 11k   11.5k 12k   12.5k 13k   13.5k 14k   14.5k 15k   15.5k */
          200,  195,  185,  175,  165,  155,  145,  135,  125,  115,  105,   95,   85,   75,   65,   55
    };
    /* Higher gears get more — lower gears are traction-limited */
    int gear_bias = (int)gear * 6;
    int mode_bias = (mode == OPENDASH_BOOST_SLOT_RACE) ? 30 : 0;
    for (int i = 0; i < OPENDASH_BOOST_MAP_POINTS; ++i) {
        int v = (int)shape[i] + gear_bias + mode_bias;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        out[i] = (uint8_t)v;
    }
}

/** Setpoint curve in centi-bar.
 *  NORMAL peaks ~97 cBar (≈0.97 BAR / 14 PSI — conservative "safe street").
 *  RACE   peaks ~248 cBar (≈2.48 BAR / 36 PSI — aggressive race target).
 *  Higher gears slightly higher. Axis matches duty: 32 points across 0..16k RPM.
 */
void opendash_boost_default_setpoint_row(uint8_t mode, uint8_t gear, uint16_t out[OPENDASH_BOOST_MAP_POINTS])
{
    static const uint16_t shape_normal[OPENDASH_BOOST_MAP_POINTS] = {
        /* 0..7.5k  RPM — spool to NORMAL peak ~97 cBar */
          0,   0,  15,  35,  55,  72,  85,  92,  95,  97,  97,  97,  97,  96,  95,  93,
        /* 8..15.5k RPM — hold high, gentle taper toward redline */
         90,  88,  85,  82,  78,  74,  70,  66,  62,  58,  54,  50,  46,  42,  38,  35
    };
    static const uint16_t shape_race[OPENDASH_BOOST_MAP_POINTS] = {
        /* 0..7.5k  RPM — quick ramp to RACE peak ~248 cBar */
          0,   0,  30,  70, 110, 150, 185, 215, 235, 245, 248, 248, 248, 247, 246, 245,
        /* 8..15.5k RPM — sustain near peak, fall off mildly */
        242, 238, 232, 225, 215, 205, 195, 185, 175, 165, 155, 145, 135, 125, 115, 105
    };
    const uint16_t *src = (mode == OPENDASH_BOOST_SLOT_RACE) ? shape_race : shape_normal;
    uint16_t gear_bias = (uint16_t)gear * 4;  /* +4 cBar per gear */
    for (int i = 0; i < OPENDASH_BOOST_MAP_POINTS; ++i) {
        uint32_t v = (uint32_t)src[i] + gear_bias;
        if (v > 300) v = 300;       /* hard cap at 3.0 BAR (≈44 PSI) */
        out[i] = (uint16_t)v;
    }
}

static void install_default_throttle_curve(opendash_boost_throttle_curve_t *c)
{
    /* No reduction once throttle ≥ ~30%, ramped pull below that.
     * Extended to 32 points so it lines up with the duty/setpoint axis. */
    static const uint16_t shape[OPENDASH_BOOST_MAP_POINTS] = {
          0, 100, 250, 450, 650, 800, 900, 950,1000,1000,1000,1000,1000,1000,1000,1000,
       1000,1000,1000,1000,1000,1000,1000,1000,1000,1000,1000,1000,1000,1000,1000,1000
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
        ESP_LOGI(TAG, "Loaded boost config from NVS (mode=%u, mask=0x%X)",
                 s_params.mode, s_params.output_mask);
    }
    return ESP_OK;
}

esp_err_t opendash_boost_set_params(const opendash_boost_params_t *p)
{
    if (!p) return ESP_ERR_INVALID_ARG;
    if (p->version != OPENDASH_BOOST_PARAMS_VERSION) return ESP_ERR_INVALID_VERSION;
    if (p->output_mask & ~0x0Fu) return ESP_ERR_INVALID_ARG;
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

uint8_t opendash_boost_get_output_mask(void)
{
    /* Single-byte read of an aligned field — atomic enough for the PWM task. */
    return s_params.output_mask & 0x0Fu;
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
     * before doing FP math. */
    uint8_t  duty_row[OPENDASH_BOOST_MAP_POINTS];
    uint16_t setp_row[OPENDASH_BOOST_MAP_POINTS];
    uint16_t throt_row[OPENDASH_BOOST_MAP_POINTS];
    uint8_t  mode_slot = (p.mode == OPENDASH_BOOST_MODE_RACE)
                            ? OPENDASH_BOOST_SLOT_RACE : OPENDASH_BOOST_SLOT_NORMAL;
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
