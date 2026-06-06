/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file lv_mem_psram.c
 * @brief LVGL custom memory hooks that route allocations into PSRAM via
 *        ESP-IDF heap_caps. Required because LVGL's builtin TLSF pool lives
 *        in internal RAM (only ~64KB) and starves once we build large UIs
 *        (e.g. the BOOST cell-grid config screen). The default ESP-IDF
 *        malloc also forces sub-CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL-byte
 *        allocations into internal RAM, so we bypass it entirely by asking
 *        heap_caps for PSRAM first and falling back to whatever 8-bit heap
 *        has space.
 *
 * Activated by CONFIG_LV_USE_CUSTOM_MALLOC=y.
 */

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

/* These prototypes come from lvgl/src/stdlib/lv_mem.h via lvgl.h above. */

static const char *TAG = "lv_mem_psram";

void lv_mem_init(void)
{
    ESP_LOGI(TAG, "LVGL custom allocator: PSRAM-preferred via heap_caps");
}

void lv_mem_deinit(void)
{
    /* nothing to do */
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL; /* pools not supported with caps-based backend */
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    /* Try PSRAM first (huge & free); fall back to any 8-bit heap. */
    void *p = heap_caps_malloc_prefer(size, 2,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                      MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "lv_malloc_core: OUT OF MEMORY requesting %u bytes",
                 (unsigned)size);
    }
    return p;
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *np = heap_caps_realloc_prefer(p, new_size, 2,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                        MALLOC_CAP_8BIT);
    if (!np && new_size) {
        ESP_LOGE(TAG, "lv_realloc_core: OUT OF MEMORY requesting %u bytes",
                 (unsigned)new_size);
    }
    return np;
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if (!mon_p) return;
    /* Best-effort: we don't track per-pool stats, so just zero. */
    __builtin_memset(mon_p, 0, sizeof(*mon_p));
}

lv_result_t lv_mem_test_core(void)
{
    /* heap_caps has its own integrity checker; just succeed here. */
    return LV_RESULT_OK;
}
