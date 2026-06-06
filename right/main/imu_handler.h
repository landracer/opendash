/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file imu_handler.h
 * @brief OpenDash IMU Handler (RIGHT pod — rollover detector)
 *
 * Manages the QMI8658 6-axis IMU (accelerometer + gyroscope) on the
 * Waveshare ESP32-S3-Touch-LCD-2.8C. RIGHT normally mirrors LEFT (display
 * only); this driver is the documented EXCEPTION that gives RIGHT a vote in
 * the distributed rollover-deployment system (see opendash_rollover.h).
 *
 * The QMI8658 sits on the shared I2C master bus (SDA=GPIO15, SCL=GPIO7),
 * I2C address 0x6B. Identical register-level driver to pod1/pod2/gps.
 */

#ifndef IMU_HANDLER_H
#define IMU_HANDLER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IMU data structure.
 */
typedef struct {
    float accel_x;          /**< X-axis acceleration (g) */
    float accel_y;          /**< Y-axis acceleration (g) */
    float accel_z;          /**< Z-axis acceleration (g) */
    float gyro_x;           /**< X-axis rotation rate (deg/s) */
    float gyro_y;           /**< Y-axis rotation rate (deg/s) */
    float gyro_z;           /**< Z-axis rotation rate (deg/s) */
    float g_lateral;        /**< Lateral g-force (left/right) */
    float g_longitudinal;   /**< Longitudinal g-force (forward/back) */
    float g_vertical;       /**< Vertical g-force (up/down) */
    float total_g;          /**< Total g-force magnitude */
    float pitch;            /**< Pitch angle (degrees, from accel) */
    float roll;             /**< Roll angle (degrees, from accel) */
} imu_data_t;

/**
 * @brief Initialize the IMU handler.
 *
 * Configures the QMI8658 accelerometer and gyroscope via I2C.
 * The shared I2C bus must already be initialized (call display_init() first).
 *
 * @return ESP_OK on success.
 */
esp_err_t imu_handler_init(void);

/**
 * @brief Start the IMU reading task.
 *
 * @return ESP_OK on success.
 */
esp_err_t imu_handler_start(void);

/**
 * @brief Get the latest IMU data (thread-safe copy).
 *
 * @param[out] data  Pointer to IMU data structure to populate.
 * @return ESP_OK on success.
 */
esp_err_t imu_handler_get_data(imu_data_t *data);

/**
 * @brief True once the QMI8658 has been detected and configured.
 *
 * RIGHT must be able to tell whether its (optional) IMU is actually present:
 * if the chip is missing, RIGHT must NOT advertise itself as a working
 * detector. Used to gate vote broadcasting.
 *
 * @return true if the IMU initialized successfully.
 */
bool imu_handler_is_present(void);

#ifdef __cplusplus
}
#endif

#endif /* IMU_HANDLER_H */
