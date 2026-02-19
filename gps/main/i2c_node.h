/**
 * @file i2c_node.h
 * @brief OpenDash GPS Unit — I2C Slave Node Handler
 *
 * Implements the I2C slave interface for receiving commands from the
 * Center display (I2C master) and responding with GPS/IMU data.
 *
 * NOTE: The Waveshare board's shared I2C bus (GPIO15/14) is used by the
 * BSP for onboard peripherals (touch, IMU, GPS, PMU). The OpenDash
 * inter-node I2C bus uses **separate pins** defined in opendash_i2c_protocol.h
 * so there is no bus contention.
 *
 * OpenDash I2C Node:
 *   Address: OPENDASH_I2C_ADDR_GPS (0x12)
 *   SDA:     OPENDASH_I2C_SDA_PIN  (GPIO varies per wiring)
 *   SCL:     OPENDASH_I2C_SCL_PIN  (GPIO varies per wiring)
 *   Role:    Slave — responds to data requests from Center unit
 *
 * @see opendash_i2c_protocol.h for message format and command IDs.
 */

#ifndef I2C_NODE_H
#define I2C_NODE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the OpenDash I2C slave node.
 *
 * Sets up I2C slave on the inter-node bus with address 0x12.
 * Registers the receive callback for handling master commands.
 *
 * @return ESP_OK on success.
 */
esp_err_t i2c_node_init(void);

/**
 * @brief Start the I2C node communication task.
 *
 * Launches a FreeRTOS task that processes received I2C messages
 * and prepares GPS/IMU data responses.
 *
 * @return ESP_OK on success.
 */
esp_err_t i2c_node_start(void);

#ifdef __cplusplus
}
#endif

#endif /* I2C_NODE_H */
