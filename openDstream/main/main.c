/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief openDstream — ESP-NOW to UART Bridge for rAtTrax BMS
 *
 * Receives ESP-NOW packets from rAtTrax BMS and outputs DP:0xXXXX:XX.XX lines.
 *
 * Protocol (from BMS):
 *   SYNC(0xAA) + CMD + LEN + PAYLOAD + CHECKSUM
 *   Payload: [dp_id_hi][dp_id_lo][float:4B] repeated N times
 *
 * Output: DP:0xXXXX:%.2f\n via UART at 921600 baud
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "nvs_flash.h"

#include "driver/uart.h"
#include "driver/gpio.h"

/* OpenDash protocol constants (matching BMS source) */
#define OD_MSG_SYNC        0xAA
#define OD_CMD_DATA_RESPONSE 0x81   // Sensor data response

/* UART configuration for USB-to-UART bridge (CP2102/CH340) */
#define UART_PORT_NUM    (UART_NUM_0)
#define UART_BAUD_RATE   (921600)
#define UART_TX_PIN      (GPIO_NUM_1)

/**
 * @brief Calculate checksum (XOR of all bytes)
 */
static uint8_t calc_checksum(const uint8_t *data, int len) {
    uint8_t chk = 0;
    for (int i = 0; i < len; i++) {
        chk ^= data[i];
    }
    return chk;
}

/**
 * @brief Initialize UART for high-speed output to USB-to-UART bridge
 */
static void uart_init_high_speed(void) {
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        #ifdef CONFIG_UART_CLKSRC_DEFAULT
        .source_clk = UART_SCLK_DEFAULT,
        #else
        .source_clk = UART_SCLK_APB,
        #endif
    };

    // Install driver with large buffer
    uart_driver_install(UART_PORT_NUM, 4096, 0, 0, NULL, 0);
    uart_param_config(UART_PORT_NUM, &uart_config);
    uart_set_pin(UART_PORT_NUM, UART_TX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // Flush the port
    uart_flush(UART_PORT_NUM);

    ESP_LOGI("uart", "Initialized @ %d baud (TX=%d)", UART_BAUD_RATE, UART_TX_PIN);
}

/**
 * @brief Send formatted data point via UART
 */
static void uart_send_dp(uint16_t dp_id, float value) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "DP:0x%04X:%.2f\n", dp_id, value);
    uart_write_bytes(UART_PORT_NUM, buf, len);
}

/**
 * @brief Parse a single OpenDash frame and extract data points
 *
 * Frame format:
 *   [0] = SYNC (0xAA)
 *   [1] = CMD
 *   [2] = LEN
 *   [3..3+LEN-1] = PAYLOAD
 *   [3+LEN] = CHECKSUM
 *
 * Payload: repeated [dp_id_hi][dp_id_lo][float:4B]
 *   Total payload per DP = 6 bytes
 */
static void parse_opendash_frame(const uint8_t *data, int len) {
    // Minimum frame: SYNC + CMD + LEN + CHK = 4 bytes
    if (len < 4) return;

    // Validate sync byte
    if (data[0] != OD_MSG_SYNC) return;

    // Get payload length
    uint8_t payload_len = data[2];

    // Validate total frame length: 3 header + payload + 1 checksum
    int expected_len = 3 + payload_len + 1;
    if (expected_len > len) return;

    // Verify checksum
    if (calc_checksum(data, expected_len - 1) != data[expected_len - 1]) {
        ESP_LOGW("parse", "Checksum mismatch");
        return;
    }

    // Parse payload - each DP is 6 bytes: [id_hi][id_lo][float:4B]
    const uint8_t *payload = &data[3];
    int dp_count = payload_len / 6;  // Each data point is 6 bytes

    for (int i = 0; i < dp_count; i++) {
        uint16_t dp_id = ((uint16_t)payload[0] << 8) | payload[1];
        float value;
        memcpy(&value, &payload[2], sizeof(float));

        // Send via UART
        uart_send_dp(dp_id, value);

        payload += 6;  // Move to next data point
    }
}

/**
 * @brief ESP-NOW receive callback
 */
static void espnow_recv_callback(const esp_now_recv_info_t *recv_info,
                                  const uint8_t *data, int len) {
    if (!data || len <= 0) return;

    // Parse and output all data points in the frame
    parse_opendash_frame(data, len);
}

/**
 * @brief Application entry point
 */
void app_main(void) {
    ESP_LOGI("opendstream", "rAtTrax BMS to OpenDash Bridge v1.1");
    ESP_LOGI("opendstream", "Listening for ESPNOW packets from rAtTrax BMS");

    /* Initialize UART first (for early boot messages) */
    uart_init_high_speed();

    /* Send boot message */
    const char *boot_msg = "CFG:BOOT:rAtTrax-BMS-v1.1\n";
    uart_write_bytes(UART_PORT_NUM, boot_msg, strlen(boot_msg));

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* Initialize WiFi in STA mode for ESP-NOW */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    // Lock to channel 1 (matching BMS)
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    esp_wifi_set_mode(WIFI_MODE_STA);

    esp_wifi_start();

    /* Initialize ESP-NOW */
    ret = esp_now_init();
    if (ret != ESP_OK) {
        ESP_LOGE("opendstream", "ESP-NOW init failed: %s", esp_err_to_name(ret));
        return;
    }

    /* Register receive callback */
    esp_now_register_recv_cb(espnow_recv_callback);

    /* Create broadcast peer (FF:FF:FF:FF:FF:FF) */
    esp_now_peer_info_t peer = {
        .peer_addr = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF},
        .channel = 1,
        .ifidx = WIFI_IF_STA,
        .encrypt = false,
    };

    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK) {
        ESP_LOGE("opendstream", "Failed to add peer: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI("opendstream", "Ready - listening on channel 1");
    ESP_LOGI("opendstream", "Output format: DP:0xXXXX:XX.XX at %d baud", UART_BAUD_RATE);
}