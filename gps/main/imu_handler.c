/**
 * @file imu_handler.c
 * @brief OpenDash IMU Handler — QMI8658 via I2C
 *
 * Interfaces with the QMI8658 6-axis IMU via I2C to read accelerometer
 * and gyroscope data for g-force calculations and motion tracking.
 *
 * QMI8658 register-level driver:
 *   I2C Address: 0x6B (SDO high on Waveshare board)
 *   Accel range: ±4g (sensitivity: 8192 LSB/g)
 *   Gyro range:  ±512 deg/s (sensitivity: 64 LSB/deg/s)
 *   Data rate:   100 Hz (both accel and gyro)
 *
 * @see QMI8658 Datasheet for register map.
 * @see ESP32 I2C API:
 *      https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html
 */

#include "imu_handler.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "display_init.h"
#include <string.h>
#include <math.h>

static const char *TAG = "imu_handler";

/* QMI8658 I2C address (7-bit) */
#define QMI8658_I2C_ADDR        0x6B

/* QMI8658 Register addresses */
#define QMI8658_REG_WHO_AM_I    0x00    /* Device ID (should read 0x05) */
#define QMI8658_REG_CTRL1       0x02    /* SPI/I2C config, sensor enable */
#define QMI8658_REG_CTRL2       0x03    /* Accelerometer config */
#define QMI8658_REG_CTRL3       0x04    /* Gyroscope config */
#define QMI8658_REG_CTRL5       0x06    /* Low-pass filter config */
#define QMI8658_REG_CTRL7       0x08    /* Enable sensors */
#define QMI8658_REG_CTRL9       0x0A    /* Host command register */
#define QMI8658_REG_STATUS0     0x2E    /* Output data status */
#define QMI8658_REG_AX_L        0x35    /* Accel X low byte */
#define QMI8658_REG_GX_L        0x3B    /* Gyro X low byte */
#define QMI8658_REG_RESET       0x60    /* Software reset */

/* QMI8658 expected WHO_AM_I value */
#define QMI8658_DEVICE_ID       0x05

/* Accelerometer range: ±4g → sensitivity 8192 LSB/g */
#define QMI8658_ACCEL_RANGE_4G  0x02    /* CTRL2[6:4] = 010 */
#define QMI8658_ACCEL_ODR_100HZ 0x05    /* CTRL2[3:0] = 0101 */
#define QMI8658_ACCEL_SENSITIVITY (8192.0f)

/* Gyroscope range: ±512 dps → sensitivity 64 LSB/dps */
#define QMI8658_GYRO_RANGE_512  0x03    /* CTRL3[6:4] = 011 */
#define QMI8658_GYRO_ODR_100HZ  0x05    /* CTRL3[3:0] = 0101 */
#define QMI8658_GYRO_SENSITIVITY (64.0f)

/* Task config */
#define IMU_READ_INTERVAL_MS    10      /* 100 Hz update rate */

/* IMU state */
static imu_data_t current_imu_data = {0};
static SemaphoreHandle_t imu_mutex = NULL;
static TaskHandle_t imu_task_handle = NULL;
static i2c_master_dev_handle_t qmi8658_handle = NULL;

/* ──────────────────────────────────────────────────────────────────────────
 * QMI8658 Register-Level I2C
 * ──────────────────────────────────────────────────────────────────────── */

static esp_err_t qmi8658_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(qmi8658_handle, buf, 2, 100);
}

static esp_err_t qmi8658_read_reg(uint8_t reg, uint8_t *value)
{
    esp_err_t ret = i2c_master_transmit_receive(qmi8658_handle, &reg, 1, value, 1, 100);
    return ret;
}

static esp_err_t qmi8658_read_regs(uint8_t reg, uint8_t *buf, size_t len)
{
    return i2c_master_transmit_receive(qmi8658_handle, &reg, 1, buf, len, 100);
}

/**
 * @brief Configure QMI8658 accelerometer and gyroscope.
 */
static esp_err_t qmi8658_configure(void)
{
    /* Verify device identity */
    uint8_t who_am_i = 0;
    esp_err_t ret = qmi8658_read_reg(QMI8658_REG_WHO_AM_I, &who_am_i);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 WHO_AM_I read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (who_am_i != QMI8658_DEVICE_ID) {
        ESP_LOGE(TAG, "QMI8658 WHO_AM_I mismatch: got 0x%02X, expected 0x%02X",
                 who_am_i, QMI8658_DEVICE_ID);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "QMI8658 detected (WHO_AM_I=0x%02X)", who_am_i);

    /* Software reset */
    qmi8658_write_reg(QMI8658_REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* CTRL1: Auto-increment, little-endian */
    qmi8658_write_reg(QMI8658_REG_CTRL1, 0x40);

    /* CTRL2: Accel ±4g, 100 Hz */
    qmi8658_write_reg(QMI8658_REG_CTRL2, (QMI8658_ACCEL_RANGE_4G << 4) | QMI8658_ACCEL_ODR_100HZ);

    /* CTRL3: Gyro ±512 dps, 100 Hz */
    qmi8658_write_reg(QMI8658_REG_CTRL3, (QMI8658_GYRO_RANGE_512 << 4) | QMI8658_GYRO_ODR_100HZ);

    /* CTRL5: Enable low-pass filter for both accel and gyro */
    qmi8658_write_reg(QMI8658_REG_CTRL5, 0x11);

    /* CTRL7: Enable accelerometer + gyroscope */
    qmi8658_write_reg(QMI8658_REG_CTRL7, 0x03);

    vTaskDelay(pdMS_TO_TICKS(30));  /* Wait for sensor startup */

    ESP_LOGI(TAG, "QMI8658 configured: Accel ±4g @ 100Hz, Gyro ±512dps @ 100Hz");
    return ESP_OK;
}

/**
 * @brief Read 6 bytes of accelerometer + 6 bytes of gyroscope data.
 */
static esp_err_t qmi8658_read_sensor_data(imu_data_t *data)
{
    uint8_t raw[12];  /* 6 bytes accel + 6 bytes gyro */

    /* Read accel (6 bytes starting at AX_L) */
    esp_err_t ret = qmi8658_read_regs(QMI8658_REG_AX_L, raw, 6);
    if (ret != ESP_OK) return ret;

    /* Read gyro (6 bytes starting at GX_L) */
    ret = qmi8658_read_regs(QMI8658_REG_GX_L, raw + 6, 6);
    if (ret != ESP_OK) return ret;

    /* Convert accel: signed 16-bit → g */
    int16_t ax = (int16_t)((raw[1] << 8) | raw[0]);
    int16_t ay = (int16_t)((raw[3] << 8) | raw[2]);
    int16_t az = (int16_t)((raw[5] << 8) | raw[4]);
    data->accel_x = ax / QMI8658_ACCEL_SENSITIVITY;
    data->accel_y = ay / QMI8658_ACCEL_SENSITIVITY;
    data->accel_z = az / QMI8658_ACCEL_SENSITIVITY;

    /* Convert gyro: signed 16-bit → deg/s */
    int16_t gx = (int16_t)((raw[7]  << 8) | raw[6]);
    int16_t gy = (int16_t)((raw[9]  << 8) | raw[8]);
    int16_t gz = (int16_t)((raw[11] << 8) | raw[10]);
    data->gyro_x = gx / QMI8658_GYRO_SENSITIVITY;
    data->gyro_y = gy / QMI8658_GYRO_SENSITIVITY;
    data->gyro_z = gz / QMI8658_GYRO_SENSITIVITY;

    /* Vehicle-frame g-forces (assumes board orientation) */
    data->g_lateral       = data->accel_y;       /* Left/right */
    data->g_longitudinal  = data->accel_x;       /* Forward/back */
    data->g_vertical      = data->accel_z;       /* Up/down */

    /* Total g-force magnitude */
    data->total_g = sqrtf(data->accel_x * data->accel_x +
                          data->accel_y * data->accel_y +
                          data->accel_z * data->accel_z);

    /* Simple tilt angles from accelerometer (degrees) */
    data->pitch = atan2f(data->accel_x,
                         sqrtf(data->accel_y * data->accel_y +
                               data->accel_z * data->accel_z)) * (180.0f / M_PI);
    data->roll  = atan2f(data->accel_y,
                         sqrtf(data->accel_x * data->accel_x +
                               data->accel_z * data->accel_z)) * (180.0f / M_PI);

    return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * IMU Task
 * ──────────────────────────────────────────────────────────────────────── */

static void imu_task(void *pvParameters)
{
    ESP_LOGI(TAG, "IMU task started (100 Hz)");
    imu_data_t local_data = {0};

    while (1) {
        if (qmi8658_read_sensor_data(&local_data) == ESP_OK) {
            if (imu_mutex && xSemaphoreTake(imu_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
                memcpy(&current_imu_data, &local_data, sizeof(imu_data_t));
                xSemaphoreGive(imu_mutex);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(IMU_READ_INTERVAL_MS));
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────── */

esp_err_t imu_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing IMU handler (QMI8658 via I2C, addr=0x%02X)", QMI8658_I2C_ADDR);

    imu_mutex = xSemaphoreCreateMutex();
    if (imu_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create IMU mutex");
        return ESP_FAIL;
    }

    /* Get shared I2C bus handle from display_init */
    i2c_master_bus_handle_t i2c_bus = display_get_i2c_handle();
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2C bus handle is NULL — call display_init() first");
        return ESP_FAIL;
    }

    /* Add QMI8658 to I2C bus */
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_I2C_ADDR,
        .scl_speed_hz = 400000,  /* QMI8658 supports 400 kHz */
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &dev_cfg, &qmi8658_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add QMI8658 device: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Configure the sensor */
    ret = qmi8658_configure();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 configuration failed");
        return ret;
    }

    memset(&current_imu_data, 0, sizeof(imu_data_t));
    current_imu_data.accel_z = 1.0f;  /* Gravity at rest */

    ESP_LOGI(TAG, "IMU handler initialized");
    return ESP_OK;
}

esp_err_t imu_handler_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        imu_task, "imu_task", 4096, NULL,
        5,   /* High priority for real-time motion data */
        &imu_task_handle,
        0    /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create IMU task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "IMU task started on core 0");
    return ESP_OK;
}

esp_err_t imu_handler_get_data(imu_data_t *data)
{
    if (data == NULL) return ESP_ERR_INVALID_ARG;

    if (imu_mutex && xSemaphoreTake(imu_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        memcpy(data, &current_imu_data, sizeof(imu_data_t));
        xSemaphoreGive(imu_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}
