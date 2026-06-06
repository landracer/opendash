/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file i2c_master.h
 * @brief OpenDash Center Display — I2C Master Controller
 *
 * The Center display acts as the I2C master on the inter-node bus.
 * It polls all slave nodes (Left, Right, GPS) periodically, sends
 * data point updates, and collects telemetry data.
 *
 * Pin Assignment (Waveshare ESP32-S3-Touch-LCD-4.3):
 *   Inter-node bus: SDA = GPIO15, SCL = GPIO16, Port = I2C_NUM_1
 *   On-board touch:  SDA = GPIO8,  SCL = GPIO9,  Port = I2C_NUM_0
 *
 * @see opendash_i2c_protocol.h for the wire protocol.
 */

#ifndef OPENDASH_I2C_MASTER_H
#define OPENDASH_I2C_MASTER_H

#include "esp_err.h"
#include "opendash_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Pin overrides for the 4.3" center board
 *
 * GPIO4 is used by GT911 interrupt on this board, so we use GPIO15 for SDA.
 * GPIO16 is free on all boards — used for SCL universally.
 * ──────────────────────────────────────────────────────────────────────────── */
#define I2C_MASTER_SDA_PIN      15      /* GPIO15 — free on 4.3" board */
#define I2C_MASTER_SCL_PIN      16      /* GPIO16 — free on all boards */
#define I2C_MASTER_PORT         1       /* Port 1 (port 0 = on-board touch) */

/** @brief Node online status, accessible from other modules. */
typedef struct {
    bool left_online;
    bool right_online;
    bool gps_online;
} i2c_master_node_status_t;

/**
 * @brief Initialize the I2C master bus for inter-node communication.
 *
 * Configures I2C port 1 as master at 400 kHz on GPIO15 (SDA) / GPIO16 (SCL).
 * Must be called after display_init() (which uses port 0 for touch).
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t i2c_master_init(void);

/**
 * @brief Start the I2C master polling task.
 *
 * Spawns a FreeRTOS task that periodically PINGs all slave nodes,
 * pushes data point updates, and requests telemetry from the GPS unit.
 *
 * @return ESP_OK on success, or an error code.
 */
esp_err_t i2c_master_start(void);

/**
 * @brief Get the current online status of all slave nodes.
 *
 * @param[out] status  Pointer to status structure to populate.
 */
void i2c_master_get_status(i2c_master_node_status_t *status);

/**
 * @brief Send a data point value to a specific slave node.
 *
 * @param[in] node   Target slave node.
 * @param[in] dp_id  Data point identifier.
 * @param[in] value  Float value to send.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if node is invalid,
 *         ESP_FAIL if transmission fails.
 */
esp_err_t i2c_master_send_data_point(opendash_node_t node,
                                      uint16_t dp_id, float value);

/**
 * @brief Send a PING to a specific slave and check for STATUS_REPORT.
 *
 * @param[in] node  Target slave node.
 *
 * @return ESP_OK if the slave responds, ESP_FAIL otherwise.
 */
esp_err_t i2c_master_ping(opendash_node_t node);

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_I2C_MASTER_H */
