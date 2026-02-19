/**
 * @file imu_handler.h
 * @brief OpenDash IMU Handler
 *
 * Manages the QMI8658 6-axis IMU (accelerometer + gyroscope) for
 * g-force measurements and motion tracking.
 *
 * The QMI8658 is connected via I2C on the shared bus (SDA=GPIO15, SCL=GPIO14).
 * I2C address: 0x6B (SDO pulled high on Waveshare board).
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
 * The BSP I2C bus must already be initialized.
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

#ifdef __cplusplus
}
#endif

#endif /* IMU_HANDLER_H */
