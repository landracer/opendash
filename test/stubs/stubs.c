/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file stubs.c — host-test fake implementations (virtual clock, fake
 *                semaphore, in-memory NVS). Single-threaded by design:
 *                tests exercise logic, not concurrency.
 */
#include "esp_err.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <stdlib.h>
#include <string.h>

/* ── Silent-by-default log sink (evaluates all args — see esp_log.h) ───── */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

void esp_stub_logf(const char *lvl, const char *tag, const char *fmt, ...)
{
    static int verbose = -1;
    if (verbose < 0) verbose = getenv("OD_VERBOSE_LOG") ? 1 : 0;
    if (!verbose) return;
    (void)lvl; (void)tag; (void)fmt;
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "[%s][%s] ", tag, lvl);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

/* ── Virtual clock ─────────────────────────────────────────────────────── */
static int64_t s_now_us = 0;

int64_t esp_timer_get_time(void) { return s_now_us; }

void od_stub_clock_reset(void) { s_now_us = 0; }

void od_stub_clock_advance_ms(uint32_t ms) { s_now_us += (int64_t)ms * 1000; }

/* ── Fake counting semaphore (contention impossible single-threaded) ───── */
static int s_fake_sem;

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)&s_fake_sem; }

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    (void)s; (void)ticks;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    (void)s;
    return pdTRUE;
}

/* ── In-memory NVS fake ────────────────────────────────────────────────── */
#define FAKE_NVS_MAX 64

typedef struct {
    char     ns[16];
    char     key[16];
    uint8_t *buf;
    size_t   len;
} fake_nvs_entry_t;

static fake_nvs_entry_t s_entries[FAKE_NVS_MAX];
static int              s_n_open_handle;

static fake_nvs_entry_t *find_entry(nvs_handle_t h, const char *key)
{
    (void)h;
    for (int i = 0; i < FAKE_NVS_MAX; i++)
        if (s_entries[i].buf && strcmp(s_entries[i].key, key) == 0)
            return &s_entries[i];
    return NULL;
}

esp_err_t nvs_flash_init(void)  { return ESP_OK; }
esp_err_t nvs_flash_deinit(void){ return ESP_OK; }

const char *esp_err_to_name(esp_err_t err) { (void)err; return "STUB"; }

esp_err_t nvs_open(const char *namespace_, nvs_open_mode_t mode, nvs_handle_t *out)
{
    (void)mode;
    if (!namespace_ || !out) return ESP_ERR_INVALID_ARG;
    s_n_open_handle = 1;
    *out = 1;
    /* Remember namespace on entries created later — single-namespace test
     * usage is sufficient: keys are globally unique in this project. */
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *buf, size_t *len)
{
    fake_nvs_entry_t *e = find_entry(h, key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (!buf) { *len = e->len; return ESP_OK; }
    if (*len < e->len) return ESP_ERR_INVALID_SIZE;
    memcpy(buf, e->buf, e->len);
    *len = e->len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *buf, size_t len)
{
    (void)h;
    if (!key || !buf) return ESP_ERR_INVALID_ARG;
    fake_nvs_entry_t *e = NULL;
    for (int i = 0; i < FAKE_NVS_MAX; i++) {
        if (s_entries[i].buf && strcmp(s_entries[i].key, key) == 0) { e = &s_entries[i]; break; }
    }
    if (!e) {
        for (int i = 0; i < FAKE_NVS_MAX; i++) {
            if (!s_entries[i].buf) { e = &s_entries[i]; break; }
        }
    }
    if (!e) return ESP_ERR_NO_MEM;
    if (e->buf) {
        uint8_t *grown = (uint8_t *)realloc(e->buf, len);
        if (!grown) return ESP_ERR_NO_MEM;
        e->buf = grown;
    } else {
        e->buf = (uint8_t *)malloc(len);
        if (!e->buf) return ESP_ERR_NO_MEM;
        strncpy(e->key, key, sizeof(e->key) - 1);
    }
    memcpy(e->buf, buf, len);
    e->len = len;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void      nvs_close(nvs_handle_t h)  { (void)h; }

void od_stub_nvs_reset(void)
{
    for (int i = 0; i < FAKE_NVS_MAX; i++) {
        free(s_entries[i].buf);
        memset(&s_entries[i], 0, sizeof(s_entries[i]));
    }
}
