/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file gps_handler.h
 * @brief OpenDash GPS Handler
 *
 * Manages the LC76G GNSS module for GPS positioning, speed, and heading.
 *
 * On the Waveshare ESP32-S3-Touch-AMOLED-1.75, the LC76G is connected
 * via I2C using the CASIC protocol on the shared bus (GPIO15/14):
 *   Write address: 0x50 (7-bit) — for CASIC commands
 *   Read address:  0x54 (7-bit) — for NMEA data
 *   Clock: 100 kHz
 *
 * The module outputs standard NMEA 0183 sentences over I2C which are parsed
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
    uint16_t visible_sats;  /**< Total visible satellites from GSV */
    float hdop;             /**< Horizontal dilution of precision */
    float accuracy;         /**< Estimated horizontal accuracy in meters */
    bool fix_valid;         /**< True if GPS fix is valid (3D preferred) */
    uint8_t fix_quality;    /**< 0=invalid, 1=GPS, 2=DGPS, 6=estimated */
    uint8_t hour;           /**< UTC hour (0-23) */
    uint8_t minute;         /**< UTC minute (0-59) */
    uint8_t second;         /**< UTC second (0-59) */

    /* ── 14.1 Enhancement: expanded NMEA fields ── */
    float pdop;             /**< Position DOP from GSA */
    float vdop;             /**< Vertical DOP from GSA */
    bool fix_3d;            /**< true if GSA reports 3D fix (mode=3) */
    uint16_t visible_gps;   /**< GPS satellites in view (from GPGSV) */
    uint16_t visible_glo;   /**< GLONASS in view (from GLGSV) */
    uint16_t visible_gal;   /**< Galileo in view (from GAGSV) */
    uint16_t visible_bds;   /**< BeiDou in view (from GBGSV) */
    uint16_t year;          /**< UTC year from RMC (e.g. 2025) */
    uint8_t month;          /**< UTC month from RMC (1-12) */
    uint8_t day;            /**< UTC day from RMC (1-31) */
} gps_data_t;

/**
 * @brief GPS debug/diagnostic data for on-screen display.
 */
typedef struct {
    uint32_t total_bytes;       /**< Total NMEA bytes received */
    uint32_t total_sentences;   /**< Total NMEA sentences parsed */
    uint32_t cycle;             /**< Current polling cycle */
    uint32_t consecutive_fails; /**< Consecutive I2C failures */
    uint32_t warm_ups;          /**< Number of warm-up sequences performed */
    uint32_t cmds_sent;         /**< Commands sent to LC76G */
    uint32_t cmds_ok;           /**< Commands acknowledged OK */
    /* Sentence type counters */
    uint32_t cnt_gga;           /**< GGA sentence count */
    uint32_t cnt_rmc;           /**< RMC sentence count */
    uint32_t cnt_gsv;           /**< GSV sentence count */
    uint32_t cnt_gsa;           /**< GSA sentence count */
    uint32_t cnt_gll;           /**< GLL sentence count */
    uint32_t cnt_vtg;           /**< VTG sentence count */
    uint32_t cnt_txt;           /**< TXT sentence count */
    uint32_t cnt_pqtm;         /**< PQTM proprietary count */
    uint32_t cnt_other;         /**< Other/unknown sentences */
    /* Last raw GGA for debug display */
    char last_gga[128];         /**< Last raw $xxGGA sentence */
    char last_rmc[128];         /**< Last raw $xxRMC sentence */

    /* ── 14.3 Enhancement: recovery diagnostics ── */
    uint32_t total_wakes;           /**< Number of WAKE sequences performed */
    uint32_t total_activations;     /**< Activation transmit_receive count */
    uint32_t total_power_cycles;    /**< Mid-run PMIC power cycle count */
    uint32_t total_nacks;           /**< Total I2C NACKs on 0x54 */
    uint32_t max_consecutive_fails; /**< High-water mark for fail streaks */
} gps_debug_t;

/**
 * @brief Initialize the GPS handler.
 *
 * Configures I2C CASIC communication with the LC76G module.
 * The I2C bus must already be initialized via display_init().
 * GPIO17 = RX (from LC76G), GPIO18 = TX (to LC76G).
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

/**
 * @brief Get GPS debug/diagnostic data (thread-safe copy).
 *
 * @param[out] debug  Pointer to debug structure to populate.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if debug is NULL.
 */
esp_err_t gps_handler_get_debug(gps_debug_t *debug);

/**
 * @brief Send a cold start command to the LC76G.
 *
 * Forces the module to clear all stored data and re-acquire satellites.
 *
 * @return ESP_OK on success.
 */
esp_err_t gps_handler_send_cold_start(void);

/**
 * @brief Send a warm start command to the LC76G ($PQTMWARM).
 *
 * Restarts satellite acquisition while keeping stored ephemeris.
 *
 * @return ESP_OK on success.
 */
esp_err_t gps_handler_send_warm_start(void);

/**
 * @brief Set GNSS fix output rate (1-10 Hz).
 *
 * Sends $PAIR050 via CASIC write protocol.
 *
 * @param hz  Fix rate in Hz (1-10).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if hz out of range.
 */
esp_err_t gps_handler_set_rate_hz(uint8_t hz);

/**
 * @brief Enable/disable GNSS constellations.
 *
 * Sends $PAIR066 via CASIC write protocol.
 *
 * @param gps      Enable GPS (L1 C/A)
 * @param glonass  Enable GLONASS (L1OF)
 * @param galileo  Enable Galileo (E1)
 * @param beidou   Enable BeiDou (B1I)
 * @return ESP_OK on success.
 */
esp_err_t gps_handler_set_constellations(bool gps, bool glonass, bool galileo, bool beidou);

#ifdef __cplusplus
}
#endif

#endif /* GPS_HANDLER_H */
