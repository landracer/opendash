/**
 * @file imu_handler.c
 * @brief OpenDash IMU Handler Implementation
 *
 * Interfaces with the QMI8658 6-axis IMU via I2C to read accelerometer
 * and gyroscope data for g-force calculations and motion tracking.
 *
 * @see ESP32 I2C API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/i2c.html
 */

#include "imu_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

static const char *TAG = "imu_handler";

/* Latest IMU data */
static imu_data_t current_imu_data = {0};
static TaskHandle_t imu_task_handle = NULL;

/**
 * @brief Calculate g-forces from raw accelerometer data.
 */
static void calculate_g_forces(imu_data_t *data)
{
    /* Simple calculation assuming vehicle-aligned axes */
    data->g_lateral = data->accel_y;       /* Left/right */
    data->g_longitudinal = data->accel_x;  /* Forward/back */
    data->g_vertical = data->accel_z;      /* Up/down */
}

/**
 * @brief IMU reading task.
 *
 * Continuously reads from the IMU and calculates g-forces.
 */
static void imu_task(void *pvParameters)
{
    ESP_LOGI(TAG, "IMU task started");
    
    while (1) {
        /* Future implementation:
         * 1. Read accelerometer data from QMI8658 via I2C
         * 2. Read gyroscope data from QMI8658
         * 3. Apply calibration and filtering
         * 4. Calculate g-forces in vehicle reference frame
         * 5. Update current_imu_data structure
         */
        
        /* For baseline, set dummy data */
        current_imu_data.accel_x = 0.0f;
        current_imu_data.accel_y = 0.0f;
        current_imu_data.accel_z = 1.0f;  /* 1g from gravity */
        current_imu_data.gyro_x = 0.0f;
        current_imu_data.gyro_y = 0.0f;
        current_imu_data.gyro_z = 0.0f;
        
        calculate_g_forces(&current_imu_data);
        
        vTaskDelay(pdMS_TO_TICKS(20));  /* 50Hz update rate */
    }
}

esp_err_t imu_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing IMU handler for QMI8658 sensor");
    
    /* Future implementation:
     * 1. Configure I2C for IMU communication
     * 2. Initialize QMI8658 (reset, configure ranges, enable sensors)
     * 3. Calibrate accelerometer and gyroscope
     * 4. Set up interrupt pin if needed
     */
    
    /* Initialize IMU data structure */
    memset(&current_imu_data, 0, sizeof(imu_data_t));
    
    ESP_LOGI(TAG, "IMU handler initialized");
    return ESP_OK;
}

esp_err_t imu_handler_start(void)
{
    /* Create IMU reading task */
    BaseType_t ret = xTaskCreatePinnedToCore(
        imu_task,
        "imu_task",
        4096,
        NULL,
        5,  /* High priority for real-time motion data */
        &imu_task_handle,
        0   /* Core 0 */
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IMU task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "IMU task created on core 0");
    return ESP_OK;
}

esp_err_t imu_handler_get_data(imu_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Copy current IMU data */
    memcpy(data, &current_imu_data, sizeof(imu_data_t));
    
    return ESP_OK;
}
