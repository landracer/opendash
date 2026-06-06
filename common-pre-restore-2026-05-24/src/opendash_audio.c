/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_audio.c
 * @brief OpenDash Audio Alert Driver — I2S WAV playback from SPIFFS
 *
 * Plays 8-bit mono PCM WAV files (8kHz) through I2S.
 * Sound files stored in /spiffs/snd_XX_name.wav (XX = 00-63).
 *
 * Uses a FreeRTOS queue + task pattern so ESP-NOW callbacks can trigger
 * audio without blocking.
 */

#include "opendash_audio.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_spiffs.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "audio";

/* ── I2S Pin Configuration ─────────────────────────────────────────────── */
/* Adjust these to match your wiring.  These GPIOs are free on the
 * Waveshare ESP32-S3-Touch-AMOLED-1.75 used by pod1/pod2. */
#define AUDIO_I2S_BCK_GPIO   13
#define AUDIO_I2S_WS_GPIO    16
#define AUDIO_I2S_DOUT_GPIO  17

#define AUDIO_SAMPLE_RATE    8000
#define AUDIO_QUEUE_DEPTH    4
#define AUDIO_TASK_STACK     4096
#define AUDIO_TASK_PRIO      3
#define AUDIO_BUF_SIZE       512

/* ── State ─────────────────────────────────────────────────────────────── */
static i2s_chan_handle_t s_tx_chan = NULL;
static QueueHandle_t     s_audio_queue = NULL;
static TaskHandle_t      s_audio_task = NULL;
static volatile bool     s_playing = false;
static volatile bool     s_stop_requested = false;

/* Sound file lookup table — populated at init by scanning SPIFFS */
#define MAX_SOUNDS  64
static char s_sound_paths[MAX_SOUNDS][64];  /* "/spiffs/snd_XX_name.wav" */
static int  s_sound_count = 0;

/* ── WAV Header Parsing ──────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    char     riff[4];       /* "RIFF" */
    uint32_t file_size;
    char     wave[4];       /* "WAVE" */
    char     fmt_id[4];     /* "fmt " */
    uint32_t fmt_size;
    uint16_t audio_format;  /* 1 = PCM */
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_header_t;

static bool parse_wav_header(FILE *f, uint32_t *data_offset, uint32_t *data_size)
{
    wav_header_t hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) < sizeof(hdr)) return false;
    if (memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0) return false;
    if (hdr.audio_format != 1) {
        ESP_LOGW(TAG, "Non-PCM WAV (format=%d)", hdr.audio_format);
        return false;
    }

    /* Find "data" chunk */
    char chunk_id[4];
    uint32_t chunk_size;
    while (fread(chunk_id, 1, 4, f) == 4 && fread(&chunk_size, 1, 4, f) == 4) {
        if (memcmp(chunk_id, "data", 4) == 0) {
            *data_offset = (uint32_t)ftell(f);
            *data_size = chunk_size;
            return true;
        }
        fseek(f, (long)chunk_size, SEEK_CUR);
    }
    return false;
}

/* ── SPIFFS Sound Index Builder ──────────────────────────────────────── */
static void build_sound_index(void)
{
    DIR *dir = opendir("/spiffs");
    if (!dir) {
        ESP_LOGW(TAG, "No /spiffs directory — no sounds loaded");
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && s_sound_count < MAX_SOUNDS) {
        if (strncmp(entry->d_name, "snd_", 4) != 0) continue;
        size_t namelen = strlen(entry->d_name);
        if (namelen > 54) continue;  /* Skip names too long for buffer */
        /* Parse index from "snd_XX_..." */
        int idx = -1;
        if (sscanf(entry->d_name, "snd_%d", &idx) == 1 && idx >= 0 && idx < MAX_SOUNDS) {
            /* Buffer is 64 chars: "/spiffs/" (8) + name (≤54) + NUL = ≤63 */
            memcpy(s_sound_paths[idx], "/spiffs/", 8);
            memcpy(s_sound_paths[idx] + 8, entry->d_name, namelen + 1);
            s_sound_count++;
            ESP_LOGI(TAG, "Sound[%02d]: %s", idx, s_sound_paths[idx]);
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "Indexed %d sound files", s_sound_count);
}

/* ── Playback Task ───────────────────────────────────────────────────── */
static void play_wav_file(const char *path, uint16_t max_duration_ms)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s", path);
        return;
    }

    uint32_t data_offset = 0, data_size = 0;
    if (!parse_wav_header(f, &data_offset, &data_size)) {
        ESP_LOGW(TAG, "Invalid WAV: %s", path);
        fclose(f);
        return;
    }

    fseek(f, (long)data_offset, SEEK_SET);

    uint32_t max_bytes = data_size;
    if (max_duration_ms > 0) {
        uint32_t byte_limit = (uint32_t)((AUDIO_SAMPLE_RATE * max_duration_ms) / 1000);
        if (byte_limit < max_bytes) max_bytes = byte_limit;
    }

    uint8_t buf[AUDIO_BUF_SIZE];
    /* Convert 8-bit unsigned to 16-bit signed for I2S */
    int16_t i2s_buf[AUDIO_BUF_SIZE];
    uint32_t bytes_played = 0;
    s_playing = true;
    s_stop_requested = false;

    ESP_LOGI(TAG, "Playing %s (%lu bytes)", path, (unsigned long)max_bytes);

    while (bytes_played < max_bytes && !s_stop_requested) {
        uint32_t to_read = max_bytes - bytes_played;
        if (to_read > AUDIO_BUF_SIZE) to_read = AUDIO_BUF_SIZE;
        size_t got = fread(buf, 1, to_read, f);
        if (got == 0) break;

        /* Upsample 8-bit unsigned → 16-bit signed */
        for (size_t i = 0; i < got; i++) {
            i2s_buf[i] = (int16_t)((buf[i] - 128) << 8);
        }

        size_t written = 0;
        i2s_channel_write(s_tx_chan, i2s_buf, got * sizeof(int16_t),
                          &written, pdMS_TO_TICKS(200));
        bytes_played += (uint32_t)got;
    }

    fclose(f);
    s_playing = false;
    ESP_LOGI(TAG, "Playback done (%lu bytes)", (unsigned long)bytes_played);
}

static void audio_task(void *arg)
{
    (void)arg;
    opendash_audio_alert_t alert;

    while (1) {
        if (xQueueReceive(s_audio_queue, &alert, portMAX_DELAY) == pdTRUE) {
            if (alert.sound_id >= MAX_SOUNDS || s_sound_paths[alert.sound_id][0] == '\0') {
                ESP_LOGW(TAG, "Sound ID %d not found", alert.sound_id);
                continue;
            }
            /* Enable I2S TX channel */
            i2s_channel_enable(s_tx_chan);
            play_wav_file(s_sound_paths[alert.sound_id], alert.duration_ms);
            i2s_channel_disable(s_tx_chan);
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

esp_err_t opendash_audio_init(void)
{
    /* Create I2S TX channel */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 4;
    chan_cfg.dma_frame_num = 256;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL), TAG, "I2S new channel");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)AUDIO_I2S_BCK_GPIO,
            .ws   = (gpio_num_t)AUDIO_I2S_WS_GPIO,
            .dout = (gpio_num_t)AUDIO_I2S_DOUT_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_chan, &std_cfg), TAG, "I2S init");

    /* Build sound file index from SPIFFS */
    build_sound_index();

    /* Create playback queue and task */
    s_audio_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(opendash_audio_alert_t));
    if (!s_audio_queue) return ESP_ERR_NO_MEM;

    BaseType_t ret = xTaskCreatePinnedToCore(audio_task, "audio", AUDIO_TASK_STACK,
                                              NULL, AUDIO_TASK_PRIO, &s_audio_task, 1);
    if (ret != pdPASS) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Audio subsystem initialized (I2S BCK=%d WS=%d DOUT=%d)",
             AUDIO_I2S_BCK_GPIO, AUDIO_I2S_WS_GPIO, AUDIO_I2S_DOUT_GPIO);
    return ESP_OK;
}

esp_err_t opendash_audio_play(const opendash_audio_alert_t *alert)
{
    if (!s_audio_queue || !alert) return ESP_ERR_INVALID_STATE;

    if (alert->priority >= OPENDASH_AUDIO_PRI_HIGH && s_playing) {
        /* Interrupt current playback */
        s_stop_requested = true;
        vTaskDelay(pdMS_TO_TICKS(50)); /* Let playback task notice */
        xQueueReset(s_audio_queue);    /* Clear any queued low-priority */
    }

    if (xQueueSend(s_audio_queue, alert, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Audio queue full — dropping sound %d", alert->sound_id);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void opendash_audio_stop(void)
{
    s_stop_requested = true;
    xQueueReset(s_audio_queue);
}

bool opendash_audio_is_playing(void)
{
    return s_playing;
}
