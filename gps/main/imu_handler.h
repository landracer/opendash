/**
 * @file imu_handler.h
 * @brief OpenDash IMU Handler
 *
 * Manages the QMI8658 6-axis IMU (accelerometer + gyroscope) for
 * g-force measurements and motion tracking.
 */

#ifndef IMU_HANDLER_H
#define IMU_HANDLER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IMU data structure.
 */
typedef struct {
    float accel_x;      /**< X-axis acceleration (g) */
    float accel_y;      /**< Y-axis acceleration (g) */
    float accel_z;      /**< Z-axis acceleration (g) */
    float gyro_x;       /**< X-axis rotation rate (°/s) */
    float gyro_y;       /**< Y-axis rotation rate (°/s) */
    float gyro_z;       /**< Z-axis rotation rate (°/s) */
    float g_lateral;    /**< Lateral g-force (left/right) */
    float g_longitudinal; /**< Longitudinal g-force (forward/back) */
    float g_vertical;   /**< Vertical g-force (up/down) */
} imu_data_t;

/**
 * @brief Initialize the IMU handler.
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
 * @brief Get the latest IMU data.
 *
 * @param[out] data  Pointer to IMU data structure to populate.
 *
 * @return ESP_OK on success.
 */
esp_err_t imu_handler_get_data(imu_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* IMU_HANDLER_H */
