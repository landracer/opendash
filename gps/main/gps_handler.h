/**
 * @file gps_handler.h
 * @brief OpenDash GPS Handler
 *
 * Manages the LC76G GNSS module for GPS positioning, speed, and heading.
 */

#ifndef GPS_HANDLER_H
#define GPS_HANDLER_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief GPS data structure.
 */
typedef struct {
    double latitude;        /**< Latitude in degrees */
    double longitude;       /**< Longitude in degrees */
    float altitude;         /**< Altitude in meters */
    float speed;            /**< Speed in km/h */
    float heading;          /**< Heading in degrees (0-360) */
    uint8_t satellites;     /**< Number of satellites in view */
    float hdop;             /**< Horizontal dilution of precision */
    bool fix_valid;         /**< True if GPS fix is valid */
} gps_data_t;

/**
 * @brief Initialize the GPS handler.
 *
 * @return ESP_OK on success.
 */
esp_err_t gps_handler_init(void);

/**
 * @brief Start the GPS reading task.
 *
 * @return ESP_OK on success.
 */
esp_err_t gps_handler_start(void);

/**
 * @brief Get the latest GPS data.
 *
 * @param[out] data  Pointer to GPS data structure to populate.
 *
 * @return ESP_OK on success.
 */
esp_err_t gps_handler_get_data(gps_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* GPS_HANDLER_H */
