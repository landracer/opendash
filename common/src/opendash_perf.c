/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_perf.c
 * @brief Perf telemetry sampler — per-core idle %, render cost EMA/max, rx drops
 *
 * See opendash_perf.h. All math is done on 1 Hz samples; the sample window
 * is the esp_timer period so the idle% reads are window-local, which is
 * exactly what we want for correlating tearing bursts with CPU pressure.
 */

#include "opendash_perf.h"
#include "opendash_espnow.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>

static const char *TAG = "perf";

#define PERF_MAX_TASKS 24
#define PERF_PERIOD_US 1000000ULL   /* 1 Hz */

static esp_timer_handle_t s_timer = NULL;
static bool             s_running = false;

/* Render-cost trackers (written from the UI task, read from timer ctx) */
static volatile uint64_t s_render_ema_us = 0;
static volatile uint64_t s_render_max_us = 0;

/* Idle-run-time bookkeeping for the windowed idle% computation */
static opendash_perf_t s_snap = {0};

/* ──────────────────────────────────────────────────────────────────────── */

void opendash_perf_render_us(uint64_t us)
{
    /* Integer EMA, alpha = 1/8: ema += (x - ema) / 8.
     * Signed correction so a drop in cost pulls the average down too.   */
    uint64_t ema = s_render_ema_us;
    if (ema == 0) ema = us;
    else if (us > ema) ema += (us - ema) / 8;
    else               ema -= (ema - us) / 8;
    s_render_ema_us = ema;

    if (us > s_render_max_us) s_render_max_us = us;
}

#if !CONFIG_FREERTOS_USE_TRACE_FACILITY || !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS

/* Run-time stats not configured on this node — sampler is a no-op stub so
 * common/ links everywhere; opendash_perf_init() reports NOT_SUPPORTED.  */
__attribute__((unused)) static void perf_sample_cb(void *arg) { (void)arg; }

#else /* run-time stats available */

static uint64_t s_idle_last[2] = {0, 0};
static int64_t  s_last_sample_us = 0;

static void perf_sample_cb(void *arg)
{
    (void)arg;

    TaskStatus_t tasks[PERF_MAX_TASKS];
    UBaseType_t n = uxTaskGetSystemState(tasks, (UBaseType_t)PERF_MAX_TASKS, NULL);
    if (n == 0) return;   /* trace facility off / no data — stay silent */

    uint64_t idle_now[2] = {0, 0};
    bool     idle_found[2] = {false, false};
    for (UBaseType_t i = 0; i < n; i++) {
        /* IDF's SMP kernel names the idle tasks "IDLE0"/"IDLE1"
         * (configIDLE_TASK_NAME "IDLE" + core-index suffix). */
        if (strncmp(tasks[i].pcTaskName, "IDLE", 4) == 0) {
            BaseType_t core = tasks[i].xCoreID;
            if (core == 0 || core == 1) {
                idle_now[core] = (uint64_t)tasks[i].ulRunTimeCounter;
                idle_found[core] = true;
            }
        }
    }

    int64_t now = esp_timer_get_time();
    int64_t window = now - s_last_sample_us;
    s_last_sample_us = now;
    if (window <= 0) return;

    /* U32 µs counter wraps every ~71 min of cumulative idle — skip that one sample */
    if (!idle_found[0] || !idle_found[1] ||
        idle_now[0] < s_idle_last[0] || idle_now[1] < s_idle_last[1]) return;

    s_snap.core0_idle_pct = idle_found[0]
        ? (float)((uint32_t)(idle_now[0] - s_idle_last[0])) * 100.0f / (float)window
        : 0.0f;
    s_snap.core1_idle_pct = idle_found[1]
        ? (float)((uint32_t)(idle_now[1] - s_idle_last[1])) * 100.0f / (float)window
        : 0.0f;
    s_idle_last[0] = idle_now[0];
    s_idle_last[1] = idle_now[1];

    s_snap.render_us_ema = s_render_ema_us;
    s_snap.render_us_max = s_render_max_us;
    s_snap.rx_drops      = opendash_espnow_get_rx_drops();
    s_snap.task_count    = (uint32_t)n;
    s_snap.valid         = true;

    /* Auto-print every 5th sample (5 s). The center's USB serial is
     * effectively read-only from the host, so we cannot rely on typing
     * 'perf' — the snapshot must stream on its own for field capture. */
    static uint32_t s_div;
    if (++s_div >= 5) {
        s_div = 0;
        opendash_perf_log();
    }
}

#endif /* run-time stats config guard */

esp_err_t opendash_perf_init(void)
{
    if (s_running) return ESP_OK;

#if !CONFIG_FREERTOS_USE_TRACE_FACILITY || !CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    ESP_LOGW(TAG, "run-time stats disabled in sdkconfig — perf inactive");
    return ESP_ERR_NOT_SUPPORTED;
#else
    const esp_timer_create_args_t args = {
        .callback = perf_sample_cb,
        .name     = "perf_sample",
    };
    esp_err_t ret = esp_timer_create(&args, &s_timer);
    if (ret != ESP_OK) return ret;
    ret = esp_timer_start_periodic(s_timer, PERF_PERIOD_US);
    if (ret != ESP_OK) {
        esp_timer_delete(s_timer);
        s_timer = NULL;
        return ret;
    }

    /* Prime the window basis so the first delta is meaningful */
    s_last_sample_us = esp_timer_get_time();
    s_running = true;
    ESP_LOGI(TAG, "perf sampler running @1Hz (task snapshot, us counters)");
    return ESP_OK;
#endif /* run-time stats config guard */
}

void opendash_perf_get(opendash_perf_t *out)
{
    if (out) *out = s_snap;
}

void opendash_perf_log(void)
{
    opendash_perf_t p;
    opendash_perf_get(&p);
    if (!p.valid) {
        ESP_LOGW(TAG, "perf sampling inactive (trace facility / run-time stats off)");
        return;
    }
    ESP_LOGI(TAG, "idle: core0=%.1f%% core1=%.1f%% | render ema=%lluus max=%lluus | "
             "rx_drops=%llu tasks=%lu",
             (double)p.core0_idle_pct, (double)p.core1_idle_pct,
             (unsigned long long)p.render_us_ema, (unsigned long long)p.render_us_max,
             (unsigned long long)p.rx_drops, (unsigned long)p.task_count);
}
