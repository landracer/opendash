/**
 * @file gps_handler.h
 * @brief OpenDash GPS Handler
 *
 * Manages the LC76G GNSS module for GPS positioning, speed, and heading.
 *
 * IMPORTANT: On the Waveshare ESP32-S3-Touch-AMOLED-1.75, the LC76G is
 * connected via I2C (not UART). It shares the same I2C bus (SDA=GPIO15,
 * SCL=GPIO14) with the touch controller, IMU, PMU, and IO expander.
 *
 *   I2C Write Address: 0x50
 *   I2C Read Address:  0x54
 *
 * The module outputs standard NMEA sentences over I2C which are parsed
 * for position, speed, heading, and satellite information.
 */

#ifndef GPS_HANDLER_H
#define GPS_HANDLER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPS data structure.
 */
typedef struct {
    double latitude;        /**< Latitude in degrees (positive = N) */
    double longitude;       /**< Longitude in degrees (positive = E) */
    float altitude;         /**< Altitude in meters above MSL */
    float speed;            /**< Speed in km/h */
    float heading;          /**< Heading in degrees (0-360, true north) */
    uint8_t satellites;     /**< Number of satellites used in fix */
    float hdop;             /**< Horizontal dilution of precision */
    float accuracy;         /**< Estimated horizontal accuracy in meters */
    bool fix_valid;         /**< True if GPS fix is valid (3D preferred) */
    uint8_t fix_quality;    /**< 0=invalid, 1=GPS, 2=DGPS, 6=estimated */
    uint8_t hour;           /**< UTC hour (0-23) */
    uint8_t minute;         /**< UTC minute (0-59) */
    uint8_t second;         /**< UTC second (0-59) */
} gps_data_t;

/**
 * @brief Initialize the GPS handler.
 *
 * Configures the I2C communication with the LC76G module.
 * The BSP I2C bus must already be initialized before calling this.
 *
 * @return ESP_OK on success.
 */
esp_err_t gps_handler_init(void);

/**
 * @brief Start the GPS reading task.
 *
 * Launches a FreeRTOS task that continuously reads NMEA sentences
 * from the LC76G over I2C and parses them.
 *
 * @return ESP_OK on success.
 */
esp_err_t gps_handler_start(void);

/**
 * @brief Get the latest GPS data (thread-safe copy).
 *
 * @param[out] data  Pointer to GPS data structure to populate.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if data is NULL.
 */
esp_err_t gps_handler_get_data(gps_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* GPS_HANDLER_H */
