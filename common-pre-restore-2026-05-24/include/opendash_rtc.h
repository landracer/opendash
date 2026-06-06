/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_rtc.h
 * @brief PCF85063 Real-Time Clock Driver for OpenDash
 *
 * Provides time get/set for the PCF85063 battery-backed RTC found on
 * Waveshare ESP32-S3-LCD-2.8C (left/right) and ESP32-S3-Touch-AMOLED-1.75
 * (GPS). I2C address 0x51.
 *
 * Two init paths:
 *   - opendash_rtc_init()         — legacy driver/i2c.h (left, right)
 *   - opendash_rtc_init_master()  — new driver/i2c_master.h (GPS, center)
 *
 * Usage:
 *   1. Call the appropriate init after I2C bus is ready
 *   2. On GPS fix: opendash_rtc_set_time() to sync RTC from GPS
 *   3. On boot (before GPS lock): opendash_rtc_sync_system_clock()
 *      to load RTC time into ESP32 system clock
 */

#ifndef OPENDASH_RTC_H
#define OPENDASH_RTC_H

#include "esp_err.h"
#include "driver/i2c_master.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the PCF85063 RTC using legacy I2C API.
 *
 * Probes the RTC at I2C address 0x51 and configures it for normal mode.
 * Use this on boards using driver/i2c.h (left, right).
 *
 * @param i2c_port  I2C port number (typically 0)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if RTC not detected
 */
esp_err_t opendash_rtc_init(int i2c_port);

/**
 * @brief Initialize the PCF85063 RTC using new I2C master API.
 *
 * Adds the PCF85063 as a device on an existing i2c_master bus.
 * Use this on boards using driver/i2c_master.h (GPS).
 *
 * @param bus_handle  Existing I2C master bus handle
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if RTC not detected
 */
esp_err_t opendash_rtc_init_master(i2c_master_bus_handle_t bus_handle);

/**
 * @brief Read the current time from the PCF85063.
 *
 * @param[out] tm  Pointer to struct tm to fill with current RTC time
 * @return ESP_OK on success
 */
esp_err_t opendash_rtc_get_time(struct tm *tm);

/**
 * @brief Set the PCF85063 time.
 *
 * Typically called when GPS provides a valid time fix.
 *
 * @param tm  Pointer to struct tm with the time to set
 * @return ESP_OK on success
 */
esp_err_t opendash_rtc_set_time(const struct tm *tm);

/**
 * @brief Sync ESP32 system clock from the RTC.
 *
 * Reads the RTC and calls settimeofday() so that gettimeofday(), time(),
 * and strftime() all return correct wall-clock time even before GPS fix.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if RTC time is invalid
 */
esp_err_t opendash_rtc_sync_system_clock(void);

/**
 * @brief Check if the RTC was detected during init.
 *
 * @return true if RTC is available
 */
bool opendash_rtc_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_RTC_H */
