/**
 * @file opendash_obd2.h
 * @brief OpenDash OBD2 PID Definitions and Interface
 *
 * Provides OBD2 (On-Board Diagnostics II) PID definitions, request/response
 * parsing, and a high-level interface for reading vehicle data.
 *
 * Supports both CAN bus (direct, via the Center unit's CAN transceiver) and
 * ELM327-compatible adapters (UART/Bluetooth).
 *
 * @par OBD2 Reference
 * Standard PIDs are defined in SAE J1979. Common mode 01 PIDs are listed below.
 *
 * @see ESP-IDF TWAI (CAN) Driver:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/twai.html
 */

#ifndef OPENDASH_OBD2_H
#define OPENDASH_OBD2_H

#include <stdint.h>
#include <stdbool.h>
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * OBD2 Mode 01 PIDs (Standard Diagnostic)
 *
 * These are the most commonly used PIDs for real-time engine/vehicle data.
 * Format: Request  = [Mode 0x01][PID]
 *         Response = [Mode 0x41][PID][Data bytes A, B, C, D]
 *
 * @see https://en.wikipedia.org/wiki/OBD-II_PIDs#Mode_01
 * ──────────────────────────────────────────────────────────────────────────── */

#define OBD2_PID_ENGINE_LOAD        0x04  /**< Calculated engine load (A*100/255) % */
#define OBD2_PID_COOLANT_TEMP       0x05  /**< Coolant temp (A-40) °C */
#define OBD2_PID_FUEL_PRESSURE      0x0A  /**< Fuel pressure (A*3) kPa */
#define OBD2_PID_INTAKE_MAP         0x0B  /**< Intake manifold pressure (A) kPa */
#define OBD2_PID_RPM                0x0C  /**< Engine RPM ((A*256+B)/4) rpm */
#define OBD2_PID_SPEED              0x0D  /**< Vehicle speed (A) km/h */
#define OBD2_PID_TIMING_ADVANCE     0x0E  /**< Timing advance (A/2-64) degrees */
#define OBD2_PID_INTAKE_TEMP        0x0F  /**< Intake air temp (A-40) °C */
#define OBD2_PID_MAF_RATE           0x10  /**< MAF air flow ((A*256+B)/100) g/s */
#define OBD2_PID_THROTTLE_POS       0x11  /**< Throttle position (A*100/255) % */
#define OBD2_PID_O2_VOLTAGE_B1S1    0x14  /**< O2 sensor B1S1 voltage (A/200) V */
#define OBD2_PID_OBD_STANDARD       0x1C  /**< OBD standard this vehicle conforms to */
#define OBD2_PID_RUNTIME            0x1F  /**< Run time since start (A*256+B) s */
#define OBD2_PID_FUEL_LEVEL         0x2F  /**< Fuel tank level (A*100/255) % */
#define OBD2_PID_BARO_PRESSURE      0x33  /**< Barometric pressure (A) kPa */
#define OBD2_PID_CONTROL_MODULE_V   0x42  /**< Control module voltage ((A*256+B)/1000) V */
#define OBD2_PID_OIL_TEMP           0x5C  /**< Engine oil temperature (A-40) °C */

/* ────────────────────────────────────────────────────────────────────────────
 * OBD2 Interface Configuration
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief OBD2 connection method.
 */
typedef enum {
    OBD2_CONN_CAN    = 0,  /**< Direct CAN bus (TWAI driver) — Center unit only */
    OBD2_CONN_ELM327 = 1,  /**< ELM327 adapter via UART or Bluetooth */
} obd2_connection_t;

/**
 * @brief OBD2 interface configuration.
 */
typedef struct {
    obd2_connection_t conn_type;    /**< Connection method */
    int               tx_pin;       /**< CAN TX GPIO (CAN mode) or UART TX (ELM327) */
    int               rx_pin;       /**< CAN RX GPIO (CAN mode) or UART RX (ELM327) */
    uint32_t          baud_rate;    /**< CAN baud rate (typically 500000) or UART baud */
} obd2_config_t;

/**
 * @brief Raw OBD2 response structure.
 */
typedef struct {
    uint8_t  pid;           /**< PID that was queried */
    uint8_t  data[4];       /**< Raw response bytes (A, B, C, D) */
    uint8_t  data_len;      /**< Number of valid data bytes */
    bool     valid;         /**< true if response was received successfully */
} obd2_response_t;

/* ────────────────────────────────────────────────────────────────────────────
 * OBD2 API
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Initialize the OBD2 interface.
 *
 * Sets up either the CAN (TWAI) driver or UART for ELM327 communication
 * based on the provided configuration.
 *
 * @param[in] config  Pointer to OBD2 configuration.
 *
 * @return OPENDASH_OK on success, error code on failure.
 *
 * @note For CAN mode, this uses the ESP-IDF TWAI driver:
 *       https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/twai.html
 */
opendash_err_t opendash_obd2_init(const obd2_config_t *config);

/**
 * @brief Request a single OBD2 PID value.
 *
 * Sends a mode 01 request for the specified PID and waits for the response.
 *
 * @param[in]  pid       OBD2 PID to request (e.g., OBD2_PID_RPM).
 * @param[out] response  Pointer to response structure.
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_TIMEOUT if no response.
 */
opendash_err_t opendash_obd2_request(uint8_t pid, obd2_response_t *response);

/**
 * @brief Decode a raw OBD2 response into a float value.
 *
 * Applies the standard OBD2 formula for the given PID to convert raw
 * bytes into a human-readable value.
 *
 * @param[in]  response   Pointer to raw OBD2 response.
 * @param[out] out_value  Decoded float value.
 *
 * @return OPENDASH_OK on success, OPENDASH_ERR_NOT_FOUND if PID formula unknown.
 */
opendash_err_t opendash_obd2_decode(const obd2_response_t *response, float *out_value);

/**
 * @brief Deinitialize the OBD2 interface and free resources.
 */
void opendash_obd2_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_OBD2_H */
