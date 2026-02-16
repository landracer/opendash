/**
 * @file display_init.h
 * @brief OpenDash GPS / Telemetry Unit — Hardware Initialization
 *
 * Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
 * Display Controller: CO5300 (QSPI interface)
 * Touch Controller: CST816S (I2C)
 * Resolution: 466×466 Round AMOLED
 */

#ifndef DISPLAY_INIT_H
#define DISPLAY_INIT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the display hardware.
 *
 * @return ESP_OK on success, error code on failure.
 */
esp_err_t display_init(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_INIT_H */
