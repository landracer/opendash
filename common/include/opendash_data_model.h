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
 * Licensed under Sovereign Individual License v1.0 — see LICENSE file
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
#define OPENDASH_DP_EGT              0x010C  /**< Exhaust gas temperature — max of all (°C) */
#define OPENDASH_DP_BATTERY_VOLTAGE  0x010D  /**< System battery voltage (V) */
#define OPENDASH_DP_TIMING_ADVANCE   0x010E  /**< Ignition timing advance (°) */
#define OPENDASH_DP_MAF_RATE         0x010F  /**< Mass air flow rate / LMM (g/s) */
#define OPENDASH_DP_FUEL_LEVEL       0x0110  /**< Fuel tank level (%) */
#define OPENDASH_DP_TRANS_TEMP       0x0111  /**< Transmission fluid temp (°C) */
#define OPENDASH_DP_EGT1             0x0112  /**< EGT cylinder 1 (°C) */
#define OPENDASH_DP_EGT2             0x0113  /**< EGT cylinder 2 (°C) */
#define OPENDASH_DP_EGT3             0x0114  /**< EGT cylinder 3 (°C) */
#define OPENDASH_DP_EGT4             0x0115  /**< EGT cylinder 4 (°C) */
#define OPENDASH_DP_EGT5             0x0118  /**< EGT channel 5 (°C) */
#define OPENDASH_DP_EGT6             0x0119  /**< EGT channel 6 (°C) */
#define OPENDASH_DP_EGT7             0x011A  /**< EGT channel 7 (°C) */
#define OPENDASH_DP_EGT8             0x011B  /**< EGT channel 8 (°C) */
#define OPENDASH_DP_O2_LAMBDA        0x0116  /**< O2 / Lambda sensor (ratio) */
#define OPENDASH_DP_MD_RPM           0x0117  /**< RPM from multidisplay source */
#define OPENDASH_DP_OBD2_FLAGS       0x011C  /**< OBD2 status flags (bit0=ready, bit1=DTCs, bit2=MIL) */
#define OPENDASH_DP_MIL_ON           0x011D  /**< MIL lamp on (1.0=on, 0.0=off) */
#define OPENDASH_DP_DTC_COUNT        0x011E  /**< Number of stored DTCs */

/* ── RPM source preference ────────────────────────────────────────────────
 * Which RPM source to display on center arc when both OBD/demo and
 * multidisplay provide RPM. 0 = prefer OBD/demo, 1 = prefer multidisplay */
#ifndef OPENDASH_RPM_SOURCE_MD
#define OPENDASH_RPM_SOURCE_MD  0
#endif

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
#define OPENDASH_DP_GPS_FIX          0x020D  /**< GPS fix status (0=no fix, 1=valid fix) */

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

/* Battery / BMS extended: 0x0409 – 0x040F */
#define OPENDASH_DP_SOH              0x0409  /**< State of health (%) */
#define OPENDASH_DP_BMS_TEMP_IC      0x040A  /**< BMS IC temperature (°C) */
#define OPENDASH_DP_BMS_BALANCE      0x040B  /**< Balancing active (0/1) */
#define OPENDASH_DP_BMS_CHARGING     0x040C  /**< Charging active (0/1) */
#define OPENDASH_DP_ENERGY_CHARGED   0x040D  /**< Energy charged this session (Wh) */

/* System data points: 0x0500 – 0x05FF */
#define OPENDASH_DP_CPU_TEMP         0x0500  /**< ESP32 die temperature (°C) */
#define OPENDASH_DP_FREE_HEAP        0x0501  /**< Free heap memory (KB) */
#define OPENDASH_DP_WIFI_RSSI        0x0502  /**< WiFi signal strength (dBm) */
#define OPENDASH_DP_UPTIME           0x0503  /**< Seconds since boot */
#define OPENDASH_DP_SD_FREE          0x0504  /**< SD card free space (MB) */
#define OPENDASH_DP_LOG_SESSION      0x0505  /**< Logging session number */

/* VESC motor controller data points: 0x0600 – 0x06FF
 * Mapped from VESC CAN STATUS messages 1-6
 * See: https://github.com/vedderb/bldc — datatypes.h CAN_PACKET_STATUS */

/* CAN_PACKET_STATUS (ID=9) — 20 Hz broadcast */
#define OPENDASH_DP_VESC_ERPM        0x0600  /**< Electrical RPM (raw eRPM) */
#define OPENDASH_DP_VESC_CURRENT     0x0601  /**< Motor phase current (A) */
#define OPENDASH_DP_VESC_DUTY        0x0602  /**< Duty cycle (0.0–1.0) */

/* CAN_PACKET_STATUS_2 (ID=14) */
#define OPENDASH_DP_VESC_AH          0x0603  /**< Amp-hours consumed (Ah) */
#define OPENDASH_DP_VESC_AH_CHARGED  0x0604  /**< Amp-hours charged (Ah) */

/* CAN_PACKET_STATUS_3 (ID=15) */
#define OPENDASH_DP_VESC_WH          0x0605  /**< Watt-hours consumed (Wh) */
#define OPENDASH_DP_VESC_WH_CHARGED  0x0606  /**< Watt-hours charged (Wh) */

/* CAN_PACKET_STATUS_4 (ID=16) */
#define OPENDASH_DP_VESC_TEMP_FET    0x0607  /**< MOSFET temperature (°C) */
#define OPENDASH_DP_VESC_TEMP_MOTOR  0x0608  /**< Motor temperature (°C) */
#define OPENDASH_DP_VESC_CURRENT_IN  0x0609  /**< Total input current (A) */
#define OPENDASH_DP_VESC_PID_POS     0x060A  /**< PID position (°) */

/* CAN_PACKET_STATUS_5 (ID=27) */
#define OPENDASH_DP_VESC_TACHO       0x060B  /**< Tachometer value (ERPM ticks) */
#define OPENDASH_DP_VESC_V_IN        0x060C  /**< Input voltage (V) */

/* CAN_PACKET_STATUS_6 (ID=58) */
#define OPENDASH_DP_VESC_ADC1        0x060D  /**< ADC channel 1 (V) */
#define OPENDASH_DP_VESC_ADC2        0x060E  /**< ADC channel 2 (V) */
#define OPENDASH_DP_VESC_ADC3        0x060F  /**< ADC channel 3 (V) */
#define OPENDASH_DP_VESC_PPM         0x0610  /**< PPM input (0.0–1.0) */

/* VESC derived / computed */
#define OPENDASH_DP_VESC_RPM         0x0611  /**< Motor RPM (eRPM / pole_pairs) */
#define OPENDASH_DP_VESC_POWER_IN    0x0612  /**< Input power (V_in × I_in) (W) */
#define OPENDASH_DP_VESC_POWER_MOTOR 0x0613  /**< Motor power (I_motor × V_in × duty) (W) */
#define OPENDASH_DP_VESC_FAULT       0x0614  /**< Fault code (mc_fault_code enum) */

/* VESC wheel speed (4 wheels via separate VESC IDs or hall sensors) */
#define OPENDASH_DP_WHEEL_RPM_FL     0x0620  /**< Front-left wheel RPM */
#define OPENDASH_DP_WHEEL_RPM_FR     0x0621  /**< Front-right wheel RPM */
#define OPENDASH_DP_WHEEL_RPM_RL     0x0622  /**< Rear-left wheel RPM */
#define OPENDASH_DP_WHEEL_RPM_RR     0x0623  /**< Rear-right wheel RPM */
#define OPENDASH_DP_WHEEL_SPEED_AVG  0x0624  /**< Average wheel speed (km/h) */

/* ─── Relay / MOS controller data points: 0x0700 – 0x07FF ───────────────
 * State: 0.0 = OFF, 1.0 = ON.  PWM duty: 0.0–100.0% (MOS modules only).
 * ──────────────────────────────────────────────────────────────────────── */

/* 4-channel HD relay (high-amp: fans, pumps, etc.) */
#define OPENDASH_DP_RELAY4_CH1       0x0700  /**< HD relay ch1 state (0/1) */
#define OPENDASH_DP_RELAY4_CH2       0x0701  /**< HD relay ch2 state (0/1) */
#define OPENDASH_DP_RELAY4_CH3       0x0702  /**< HD relay ch3 state (0/1) */
#define OPENDASH_DP_RELAY4_CH4       0x0703  /**< HD relay ch4 state (0/1) */

/* 8-channel relay module A (low-amp devices) */
#define OPENDASH_DP_RELAY8A_CH1      0x0710  /**< Relay 8A ch1 state (0/1) */
#define OPENDASH_DP_RELAY8A_CH2      0x0711  /**< Relay 8A ch2 state (0/1) */
#define OPENDASH_DP_RELAY8A_CH3      0x0712  /**< Relay 8A ch3 state (0/1) */
#define OPENDASH_DP_RELAY8A_CH4      0x0713  /**< Relay 8A ch4 state (0/1) */
#define OPENDASH_DP_RELAY8A_CH5      0x0714  /**< Relay 8A ch5 state (0/1) */
#define OPENDASH_DP_RELAY8A_CH6      0x0715  /**< Relay 8A ch6 state (0/1) */
#define OPENDASH_DP_RELAY8A_CH7      0x0716  /**< Relay 8A ch7 state (0/1) */
#define OPENDASH_DP_RELAY8A_CH8      0x0717  /**< Relay 8A ch8 state (0/1) */

/* 8-channel relay module B (low-amp devices) */
#define OPENDASH_DP_RELAY8B_CH1      0x0720  /**< Relay 8B ch1 state (0/1) */
#define OPENDASH_DP_RELAY8B_CH2      0x0721  /**< Relay 8B ch2 state (0/1) */
#define OPENDASH_DP_RELAY8B_CH3      0x0722  /**< Relay 8B ch3 state (0/1) */
#define OPENDASH_DP_RELAY8B_CH4      0x0723  /**< Relay 8B ch4 state (0/1) */
#define OPENDASH_DP_RELAY8B_CH5      0x0724  /**< Relay 8B ch5 state (0/1) */
#define OPENDASH_DP_RELAY8B_CH6      0x0725  /**< Relay 8B ch6 state (0/1) */
#define OPENDASH_DP_RELAY8B_CH7      0x0726  /**< Relay 8B ch7 state (0/1) */
#define OPENDASH_DP_RELAY8B_CH8      0x0727  /**< Relay 8B ch8 state (0/1) */

/* 4-channel MOS module A (PWM or on-off) */
#define OPENDASH_DP_MOS4A_CH1        0x0730  /**< MOS 4A ch1 state (0/1 or PWM 0-100%) */
#define OPENDASH_DP_MOS4A_CH2        0x0731  /**< MOS 4A ch2 state */
#define OPENDASH_DP_MOS4A_CH3        0x0732  /**< MOS 4A ch3 state */
#define OPENDASH_DP_MOS4A_CH4        0x0733  /**< MOS 4A ch4 state */

/* 4-channel MOS module B (PWM or on-off) */
#define OPENDASH_DP_MOS4B_CH1        0x0740  /**< MOS 4B ch1 state (0/1 or PWM 0-100%) */
#define OPENDASH_DP_MOS4B_CH2        0x0741  /**< MOS 4B ch2 state */
#define OPENDASH_DP_MOS4B_CH3        0x0742  /**< MOS 4B ch3 state */
#define OPENDASH_DP_MOS4B_CH4        0x0743  /**< MOS 4B ch4 state */

/* ────────────────────────────────────────────────────────────────────────────
 * Data Point Entry
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum number of data points tracked simultaneously. */
#define OPENDASH_MAX_DATA_POINTS    192

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
