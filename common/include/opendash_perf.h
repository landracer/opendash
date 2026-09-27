/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_perf.h
 * @brief OpenDash perf telemetry — per-core CPU idle %, LVGL render cost,
 *        ESP-NOW rx-queue drops.
 *
 * Every number here is evidence for the display-tearing / CPU-saturation
 * investigation (TEARING.md): before tuning anything we measure what the
 * cores are actually doing and how long a frame render takes.
 *
 * Requires (per-project sdkconfig):
 *   CONFIG_FREERTOS_USE_TRACE_FACILITY=y
 *   CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y
 *
 * Sampling runs in a periodic esp_timer callback (interrupt context) and is
 * fire-and-forget: if the trace facility is not configured, init returns
 * ESP_ERR_NOT_SUPPORTED and callers degrade silently.
 */

#ifndef OPENDASH_PERF_H
#define OPENDASH_PERF_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Snapshot of all perf counters. */
typedef struct {
    float    core0_idle_pct;   /**< CPU0 idle share since last sample (0-100) */
    float    core1_idle_pct;   /**< CPU1 idle share since last sample (0-100) */
    uint64_t render_us_ema;    /**< Exponential moving avg of lv_timer_handler() */
    uint64_t render_us_max;    /**< Worst observed render duration since boot   */
    uint64_t rx_drops;         /**< ESP-NOW rx queue overflows since boot       */
    uint32_t task_count;       /**< Tasks visible to the sampler                */
    bool     valid;            /**< false if run-time stats unavailable         */
} opendash_perf_t;

/** Start the 1 Hz perf sampler. Call once after espnow/registry init. */
esp_err_t opendash_perf_init(void);

/** Read the latest snapshot (atomically copied). */
void opendash_perf_get(opendash_perf_t *out);

/** Feed one lv_timer_handler() duration (microseconds). Called by the UI task. */
void opendash_perf_render_us(uint64_t us);

/** Log a one-shot summary via ESP_LOGI (console 'perf' command). */
void opendash_perf_log(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_PERF_H */
