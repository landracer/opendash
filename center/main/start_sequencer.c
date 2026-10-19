/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file start_sequencer.c
 * @brief Declarative START ENGINE sequencer implementation.
 *
 * Runner task walk-through: each step either commands a channel, waits for a
 * data point to enter its window, or simply waits. A WAIT that times out fails
 * the whole run — the point of the sequencer is that we never proceed on
 * assumed state; we PROCEED ONLY ON PROOF from the wire.
 */

#include "start_sequencer.h"

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "espnow_master.h"
#include "ui_manager.h"
#include "opendash_identity.h"
#include "opendash_data_model.h"

static const char *TAG = "start_seq";

#define SEQ_NVS_NAMESPACE "chan_reg"
#define SEQ_NVS_KEY       "seqv"
#define SEQ_BLOB_MAGIC    (0x5345)  /* 'SE' */
#define SEQ_BLOB_VERSION  (1)
#define SEQ_POLL_MS       (100)

typedef struct {
    uint16_t           magic;
    uint8_t            version;
    uint8_t            count;
    opendash_seq_step_t step[OPENDASH_SEQ_MAX_STEPS];
} seq_blob_t;

/* Built-in demo program — placeholder routine until the user programs a
 * real one over serial (`seq add ...`). Energizes the default FUEL PUMP
 * button's channel, then proves crank by watching RPM come alive. */
static const opendash_seq_step_t s_default_steps[] = {
    { .kind = OPENDASH_SEQ_SEND,
      .node = OPENDASH_NODE_RELAY_4CH, .channel = 3, .on = 1 },
    { .kind = OPENDASH_SEQ_WAIT_DP,
      .dp_id = OPENDASH_DP_RPM, .dp_min = 150.0f, .dp_max = 20000.0f,
      .timeout_100ms = 80 },  /* 8 s */
};

/* ── Runtime state ─────────────────────────────────────────────────────── */

static opendash_seq_step_t  s_steps[OPENDASH_SEQ_MAX_STEPS];
static int                  s_count = 0;
static volatile opendash_seq_state_t s_state = OPENDASH_SEQ_IDLE;
static volatile int         s_step_idx = 0;
static volatile bool        s_abort = false;
static volatile bool        s_stopped = false;   /* operator-initiated stop */
static opendash_seq_notify_fn s_notify = NULL;

static void notify(int idx)
{
    if (s_notify) s_notify(s_state, idx, s_count);
}

esp_err_t start_sequencer_init(opendash_seq_notify_fn notify_cb)
{
    s_notify = notify_cb;

    nvs_handle_t h;
    esp_err_t err = nvs_open(SEQ_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        seq_blob_t blob;
        size_t sz = sizeof(blob);
        if (nvs_get_blob(h, SEQ_NVS_KEY, &blob, &sz) == ESP_OK &&
            sz == sizeof(blob) && blob.magic == SEQ_BLOB_MAGIC &&
            blob.version == SEQ_BLOB_VERSION &&
            blob.count <= OPENDASH_SEQ_MAX_STEPS) {
            memcpy(s_steps, blob.step, sizeof(s_steps));
            s_count = blob.count;
            ESP_LOGI(TAG, "Loaded %d sequencer steps from NVS", s_count);
        } else {
            ESP_LOGI(TAG, "No valid stored sequence — loading demo defaults");
            s_count = (int)(sizeof(s_default_steps) / sizeof(s_default_steps[0]));
            memcpy(s_steps, s_default_steps, sizeof(s_default_steps));
        }
        nvs_close(h);
    } else {
        ESP_LOGI(TAG, "No %s namespace — demo default sequence active", SEQ_NVS_NAMESPACE);
        s_count = (int)(sizeof(s_default_steps) / sizeof(s_default_steps[0]));
        memcpy(s_steps, s_default_steps, sizeof(s_default_steps));
    }
    s_state = OPENDASH_SEQ_IDLE;
    return ESP_OK;
}

/* ── Runner ────────────────────────────────────────────────────────────── */

static void seq_wait_step(const opendash_seq_step_t *st)
{
    const int64_t deadline = esp_timer_get_time() +
                             (int64_t)st->timeout_100ms * 100 * 1000;
    while (!s_abort) {
        float v;
        if (ui_manager_get_dp(st->dp_id, &v) &&
            v >= st->dp_min && v <= st->dp_max) {
            ESP_LOGI(TAG, "WAIT dp 0x%04" PRIX16 " satisfied (=%.2f in [%g..%g])",
                     st->dp_id, (double)v, (double)st->dp_min, (double)st->dp_max);
            return; /* success — leaves s_state RUNNING; caller finishes */
        }
        if (esp_timer_get_time() >= deadline) break;
        vTaskDelay(pdMS_TO_TICKS(SEQ_POLL_MS));
    }
    s_abort = true; /* failed wait ends the run */
}

static void seq_run_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < s_count; i++) {
        s_step_idx = i;
        notify(i);
        const opendash_seq_step_t *st = &s_steps[i];
        switch (st->kind) {
            case OPENDASH_SEQ_SEND: {
                esp_err_t err = espnow_master_send_relay_command(
                    st->node, st->channel, st->on ? 1 : 0, 255);
                ESP_LOGI(TAG, "STEP %d SEND %s ch%" PRIu8 " → %" PRIu8 " (%s)",
                         i, opendash_node_name(st->node), st->channel, st->on,
                         esp_err_to_name(err));
                break;
            }
            case OPENDASH_SEQ_WAIT_DP:
                seq_wait_step(st);
                break;
            case OPENDASH_SEQ_DELAY: {
                uint32_t ticks = st->delay_100ms;
                for (uint32_t t = 0; t < ticks && !s_abort; t++) {
                    vTaskDelay(pdMS_TO_TICKS(SEQ_POLL_MS * 10));
                }
                break;
            }
            default:
                ESP_LOGW(TAG, "STEP %d has kind %d (unknown) — skipping", i, st->kind);
                break;
        }
        if (s_abort) break;
    }

    s_state = s_stopped ? OPENDASH_SEQ_IDLE
            : (s_abort ? OPENDASH_SEQ_FAILED : OPENDASH_SEQ_DONE);
    notify(s_step_idx);
    ESP_LOGI(TAG, "Sequence %s (steps=%d, failed at %d)",
             s_abort ? "ABORTED/FAILED" : "COMPLETE", s_count,
             s_abort ? s_step_idx : -1);
    vTaskDelete(NULL);
}

esp_err_t start_sequencer_start(void)
{
    if (s_state == OPENDASH_SEQ_RUNNING) return ESP_ERR_INVALID_STATE;

    s_abort    = false;
    s_step_idx = 0;
    s_state    = OPENDASH_SEQ_RUNNING;
    notify(0);

    if (xTaskCreate(seq_run_task, "seq_run", 4096, NULL, 5, NULL) != pdPASS) {
        s_state = OPENDASH_SEQ_IDLE;
        notify(0);
        ESP_LOGE(TAG, "Failed to create sequencer runner task");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void start_sequencer_stop(void)
{
    if (s_state != OPENDASH_SEQ_RUNNING) return;
    s_stopped = true;
    s_abort   = true;
    /* Runner observes s_abort on its next poll tick; because s_stopped is set
     * it finalizes to IDLE instead of FAILED. */
    ESP_LOGW(TAG, "Sequencer stopped by operator at step %d", s_step_idx);
}

opendash_seq_state_t start_sequencer_state(void)
{
    return s_state;
}

int start_sequencer_step_index(void)
{
    return s_step_idx;
}

int start_sequencer_step_count(void)
{
    return s_count;
}

const opendash_seq_step_t *start_sequencer_step(int idx)
{
    if (idx < 0 || idx >= s_count) return NULL;
    return &s_steps[idx];
}

/* ── Editing ───────────────────────────────────────────────────────────── */

esp_err_t start_sequencer_add(const opendash_seq_step_t *step)
{
    if (!step) return ESP_ERR_INVALID_ARG;
    if (s_state == OPENDASH_SEQ_RUNNING) return ESP_ERR_INVALID_STATE;
    if (s_count >= OPENDASH_SEQ_MAX_STEPS) return ESP_ERR_NO_MEM;
    s_steps[s_count++] = *step;
    s_state = OPENDASH_SEQ_IDLE;
    return ESP_OK;
}

esp_err_t start_sequencer_clear(void)
{
    if (s_state == OPENDASH_SEQ_RUNNING) return ESP_ERR_INVALID_STATE;
    memset(s_steps, 0, sizeof(s_steps));
    s_count = 0;
    s_state = OPENDASH_SEQ_IDLE;
    return ESP_OK;
}

esp_err_t start_sequencer_save(void)
{
    seq_blob_t blob = { .magic = SEQ_BLOB_MAGIC, .version = SEQ_BLOB_VERSION,
                        .count = (uint8_t)s_count };
    memcpy(blob.step, s_steps, sizeof(s_steps));

    nvs_handle_t h;
    esp_err_t err = nvs_open(SEQ_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, SEQ_NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Sequencer saved (%d steps, save=%s)", s_count, esp_err_to_name(err));
    return err;
}
