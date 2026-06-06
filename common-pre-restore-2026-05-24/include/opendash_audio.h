/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_audio.h
 * @brief OpenDash Audio Alert Driver
 *
 * I2S-based audio playback for ESP32-S3 pods. Plays 8-bit mono WAV files
 * from SPIFFS at 8kHz sample rate. Supports priority-based interruption.
 *
 * Hardware: External amplifier + speaker connected to I2S GPIO.
 * Default: GPIO 13 (BCK), GPIO 16 (WS), GPIO 17 (DOUT).
 */

#ifndef OPENDASH_AUDIO_H
#define OPENDASH_AUDIO_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Audio alert priority levels. */
typedef enum {
    OPENDASH_AUDIO_PRI_LOW    = 0,  /**< Queue behind current playback */
    OPENDASH_AUDIO_PRI_NORMAL = 1,  /**< Queue at front */
    OPENDASH_AUDIO_PRI_HIGH   = 2,  /**< Interrupt current playback immediately */
} opendash_audio_priority_t;

/** @brief Audio alert request (queued from ESP-NOW callback). */
typedef struct {
    uint8_t  sound_id;      /**< 0-63: index into SPIFFS sound table */
    uint8_t  priority;      /**< opendash_audio_priority_t */
    uint16_t duration_ms;   /**< 0 = full clip, else max playback time */
} opendash_audio_alert_t;

/**
 * @brief Initialize the audio subsystem.
 *
 * Sets up I2S driver, mounts SPIFFS, creates playback task.
 * Call once during app_main() after SPIFFS is mounted.
 *
 * @return ESP_OK on success.
 */
esp_err_t opendash_audio_init(void);

/**
 * @brief Queue an audio alert for playback.
 *
 * Thread-safe. Can be called from ESP-NOW callback context.
 *
 * @param alert  Alert parameters (sound_id, priority, duration).
 * @return ESP_OK if queued, ESP_ERR_TIMEOUT if queue full.
 */
esp_err_t opendash_audio_play(const opendash_audio_alert_t *alert);

/**
 * @brief Stop current playback immediately.
 */
void opendash_audio_stop(void);

/**
 * @brief Check if audio is currently playing.
 * @return true if playback is active.
 */
bool opendash_audio_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_AUDIO_H */
