/**
 * @file opendash_data_model.h
 * @brief OpenDash Shared Data Model
 *
 * Defines the central data store that holds all telemetry values. Every node
 * maintains a local copy of this data model, synchronized via I2C.
 *
 * Data points are identified by 16-bit IDs (see docs/data-points.md).
 * Values are stored as 32-bit floats for uniformity.
 *
 * @see docs/data-points.md for the full data point legend.
 * @see ESP-IDF NVS:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/storage/nvs_flash.html
 */

#ifndef OPENDASH_DATA_MODEL_H
#define OPENDASH_DATA_MODEL_H

#include <stdint.h>
#include <stdbool.h>
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Data Point ID Ranges (see docs/data-points.md for full list)
 * ──────────────────────────────────────────────────────────────────────────── */

/* Engine / OBD2 data points: 0x0100 – 0x01FF */
#define OPENDASH_DP_RPM              0x0100  /**< Engine RPM */
#define OPENDASH_DP_VEHICLE_SPEED    0x0101  /**< Vehicle speed (km/h) from ECU */
#define OPENDASH_DP_COOLANT_TEMP     0x0102  /**< Engine coolant temperature (°C) */
#define OPENDASH_DP_INTAKE_TEMP      0x0103  /**< Intake air temperature (°C) */
#define OPENDASH_DP_ENGINE_LOAD      0x0104  /**< Calculated engine load (%) */
#define OPENDASH_DP_THROTTLE_POS     0x0105  /**< Throttle position (%) */
#define OPENDASH_DP_BOOST_PRESSURE   0x0106  /**< Boost / MAP pressure (kPa) */
#define OPENDASH_DP_OIL_TEMP         0x0107  /**< Engine oil temperature (°C) */
#define OPENDASH_DP_OIL_PRESSURE     0x0108  /**< Engine oil pressure (kPa) */
#define OPENDASH_DP_FUEL_PRESSURE    0x0109  /**< Fuel system pressure (kPa) */
#define OPENDASH_DP_AFR              0x010A  /**< Air-fuel ratio */
#define OPENDASH_DP_LAMBDA           0x010B  /**< Lambda value */
#define OPENDASH_DP_EGT              0x010C  /**< Exhaust gas temperature (°C) */
#define OPENDASH_DP_BATTERY_VOLTAGE  0x010D  /**< System battery voltage (V) */
#define OPENDASH_DP_TIMING_ADVANCE   0x010E  /**< Ignition timing advance (°) */
#define OPENDASH_DP_MAF_RATE         0x010F  /**< Mass air flow rate (g/s) */
#define OPENDASH_DP_FUEL_LEVEL       0x0110  /**< Fuel tank level (%) */
#define OPENDASH_DP_TRANS_TEMP       0x0111  /**< Transmission fluid temp (°C) */

/* GPS / Navigation data points: 0x0200 – 0x02FF */
#define OPENDASH_DP_GPS_SPEED        0x0200  /**< Speed from GNSS (km/h) */
#define OPENDASH_DP_GPS_HEADING      0x0201  /**< Heading in degrees (true north) */
#define OPENDASH_DP_LATITUDE         0x0202  /**< GPS latitude (degrees) */
#define OPENDASH_DP_LONGITUDE        0x0203  /**< GPS longitude (degrees) */
#define OPENDASH_DP_ALTITUDE         0x0204  /**< GPS altitude (meters) */
#define OPENDASH_DP_SAT_COUNT        0x0205  /**< Number of satellites locked */
#define OPENDASH_DP_HDOP             0x0206  /**< Horizontal dilution of precision */
#define OPENDASH_DP_LAP_NUMBER       0x0207  /**< Current lap number */
#define OPENDASH_DP_LAP_TIME         0x0208  /**< Current lap time (ms) */
#define OPENDASH_DP_BEST_LAP_TIME    0x0209  /**< Best lap time this session (ms) */
#define OPENDASH_DP_LAP_DELTA        0x020A  /**< Delta vs. best lap (ms, +/-) */
#define OPENDASH_DP_SECTOR_TIME      0x020B  /**< Current sector time (ms) */
#define OPENDASH_DP_PREDICTIVE_LAP   0x020C  /**< Predicted current lap time (ms) */

/* IMU / Motion data points: 0x0300 – 0x03FF */
#define OPENDASH_DP_GFORCE_LAT       0x0300  /**< Lateral g-force */
#define OPENDASH_DP_GFORCE_LONG      0x0301  /**< Longitudinal g-force */
#define OPENDASH_DP_GFORCE_VERT      0x0302  /**< Vertical g-force */
#define OPENDASH_DP_YAW_RATE         0x0303  /**< Yaw rate (°/s) */
#define OPENDASH_DP_PITCH_RATE       0x0304  /**< Pitch rate (°/s) */
#define OPENDASH_DP_ROLL_RATE        0x0305  /**< Roll rate (°/s) */
#define OPENDASH_DP_PITCH_ANGLE      0x0306  /**< Pitch angle (°) */
#define OPENDASH_DP_ROLL_ANGLE       0x0307  /**< Roll angle (°) */

/* Battery / BMS data points: 0x0400 – 0x04FF */
#define OPENDASH_DP_PACK_VOLTAGE     0x0400  /**< Total pack voltage (V) */
#define OPENDASH_DP_PACK_CURRENT     0x0401  /**< Pack current (A) */
#define OPENDASH_DP_SOC              0x0402  /**< State of charge (%) */
#define OPENDASH_DP_CELL_V_MIN       0x0403  /**< Lowest cell voltage (V) */
#define OPENDASH_DP_CELL_V_MAX       0x0404  /**< Highest cell voltage (V) */
#define OPENDASH_DP_CELL_V_DELTA     0x0405  /**< Cell voltage delta (mV) */
#define OPENDASH_DP_BMS_TEMP_MAX     0x0406  /**< Highest BMS temp (°C) */
#define OPENDASH_DP_PACK_POWER       0x0407  /**< Pack power (W) */
#define OPENDASH_DP_ENERGY_USED      0x0408  /**< Energy used this session (Wh) */
#define OPENDASH_DP_CELL_V_BASE      0x0410  /**< Cell 1 voltage (0x0410–0x041F) */

/* System data points: 0x0500 – 0x05FF */
#define OPENDASH_DP_CPU_TEMP         0x0500  /**< ESP32 die temperature (°C) */
#define OPENDASH_DP_FREE_HEAP        0x0501  /**< Free heap memory (KB) */
#define OPENDASH_DP_WIFI_RSSI        0x0502  /**< WiFi signal strength (dBm) */
#define OPENDASH_DP_UPTIME           0x0503  /**< Seconds since boot */
#define OPENDASH_DP_SD_FREE          0x0504  /**< SD card free space (MB) */
#define OPENDASH_DP_LOG_SESSION      0x0505  /**< Logging session number */

/* ────────────────────────────────────────────────────────────────────────────
 * Data Point Entry
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum number of data points tracked simultaneously. */
#define OPENDASH_MAX_DATA_POINTS    128

/**
 * @brief Single data point entry in the data store.
 *
 * Each entry holds the current value, timestamp, and metadata for one
 * telemetry parameter (e.g., RPM, coolant temp, GPS speed).
 */
typedef struct {
    uint16_t id;            /**< Data point ID (see defines above) */
    float    value;         /**< Current value (all values stored as float) */
    uint32_t timestamp_ms;  /**< Timestamp of last update (ms since boot) */
    bool     valid;         /**< true if value has been received at least once */
} opendash_data_point_t;

/**
 * @brief Data store holding all tracked data points.
 *
 * Each node maintains one instance of this structure. The I2C communication
 * layer updates entries as new data arrives.
 */
typedef struct {
    opendash_data_point_t points[OPENDASH_MAX_DATA_POINTS];  /**< Data point array */
    uint16_t              count;                              /**< Number of active entries */
} opendash_data_store_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Data Store API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the data store.
 *
 * Clears all data points and sets count to 0. Call once at startup.
 *
 * @param[out] store  Pointer to data store to initialize.
 *
 * @return OPENDASH_OK on success.
 */
opendash_err_t opendash_data_init(opendash_data_store_t *store);

/**
 * @brief Update or insert a data point value.
 *
 * If a data point with the given ID already exists, its value and timestamp
 * are updated. Otherwise, a new entry is created if space permits.
 *
 * @param[in,out] store         Pointer to data store.
 * @param[in]     id            Data point ID.
 * @param[in]     value         New value.
 * @param[in]     timestamp_ms  Timestamp of this reading (ms since boot).
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_NO_MEM if store is full.
 */
opendash_err_t opendash_data_set(opendash_data_store_t *store,
                                  uint16_t id,
                                  float value,
                                  uint32_t timestamp_ms);

/**
 * @brief Get a data point value by ID.
 *
 * @param[in]  store      Pointer to data store.
 * @param[in]  id         Data point ID to look up.
 * @param[out] out_value  Pointer to receive the current value.
 *
 * @return OPENDASH_OK if found, OPENDASH_ERR_NOT_FOUND if no such data point.
 */
opendash_err_t opendash_data_get(const opendash_data_store_t *store,
                                  uint16_t id,
                                  float *out_value);

/**
 * @brief Get a data point entry (value + metadata) by ID.
 *
 * @param[in]  store  Pointer to data store.
 * @param[in]  id     Data point ID to look up.
 * @param[out] out    Pointer to receive the full data point entry.
 *
 * @return OPENDASH_OK if found, OPENDASH_ERR_NOT_FOUND if no such data point.
 */
opendash_err_t opendash_data_get_entry(const opendash_data_store_t *store,
                                        uint16_t id,
                                        opendash_data_point_t *out);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_DATA_MODEL_H */
