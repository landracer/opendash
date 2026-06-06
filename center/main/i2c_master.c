/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file i2c_master.c
 * @brief OpenDash Center Display — I2C Master Controller
 *
 * Implements the I2C master that polls all slave nodes on the inter-node bus.
 * Uses the new driver/i2c_master.h API (ESP-IDF v5.3+).
 *
 * Polling loop (runs at ~2 Hz):
 *   1. PING each known slave → mark online/offline
 *   2. Push any pending data point updates to left/right gauges
 *   3. Request GPS/IMU data from GPS slave (if online)
 *   4. Log status periodically
 *
 * Pin Assignment (Waveshare ESP32-S3-Touch-LCD-4.3):
 *   Inter-node bus: SDA = GPIO15, SCL = GPIO16, Port = I2C_NUM_1
 *
 * @see opendash_i2c_protocol.h for message format.
 * @see gps/main/i2c_node.c for the slave-side reference implementation.
 */

#include "i2c_master.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"
#include "opendash_common.h"

#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <string.h>
#include <math.h>

static const char *TAG = "i2c_master";

/** @brief Human-readable node name for logging. */
static const char *node_name(opendash_node_t node)
{
    switch (node) {
        case OPENDASH_NODE_LEFT:   return "LEFT";
        case OPENDASH_NODE_RIGHT:  return "RIGHT";
        case OPENDASH_NODE_GPS:    return "GPS";
        case OPENDASH_NODE_CENTER: return "CENTER";
        case OPENDASH_NODE_BMS:    return "BMS";
        default:                   return "UNKNOWN";
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Configuration Constants
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief I2C master clock speed. */
#define I2C_MASTER_FREQ_HZ     100000   /* 100 kHz — conservative for long wires */

/** @brief Timeout for a single I2C write+read transaction (ms). */
#define I2C_XFER_TIMEOUT_MS    50

/** @brief Time between full polling cycles (ms). */
#define POLL_INTERVAL_MS       500

/** @brief Consecutive failed PINGs before marking a node offline. */
#define PING_FAIL_THRESHOLD    3

/** @brief How often to log node status (in poll cycles). */
#define STATUS_LOG_INTERVAL    20       /* Every ~10 seconds at 500ms cycle */

/** @brief Maximum number of slave devices we track. */
#define MAX_SLAVES             3

/* ────────────────────────────────────────────────────────────────────────────
 * Slave Node Tracking
 * ──────────────────────────────────────────────────────────────────────────── */

typedef struct {
    opendash_node_t  node;              /**< Node type (LEFT, RIGHT, GPS) */
    uint8_t          addr;              /**< 7-bit I2C address */
    bool             online;            /**< Currently responding? */
    uint8_t          fail_count;        /**< Consecutive PING failures */
    uint32_t         last_seen_ms;      /**< Timestamp of last successful reply */
    i2c_master_dev_handle_t dev_handle; /**< ESP-IDF device handle on the bus */
} slave_info_t;

static slave_info_t s_slaves[MAX_SLAVES] = {
    { .node = OPENDASH_NODE_LEFT,  .addr = OPENDASH_I2C_ADDR_LEFT,  .online = false },
    { .node = OPENDASH_NODE_RIGHT, .addr = OPENDASH_I2C_ADDR_RIGHT, .online = false },
    { .node = OPENDASH_NODE_GPS,   .addr = OPENDASH_I2C_ADDR_GPS,   .online = false },
};

/* ────────────────────────────────────────────────────────────────────────────
 * I2C Bus & Task State
 * ──────────────────────────────────────────────────────────────────────────── */

static i2c_master_bus_handle_t s_bus_handle = NULL;
static TaskHandle_t            s_task_handle = NULL;

/* Node status — protected by simple atomic reads (single writer task) */
static i2c_master_node_status_t s_node_status = {0};

/* Scratch buffers for protocol serialization (used only in master task) */
static uint8_t s_tx_buf[OPENDASH_MSG_MAX_SIZE];
static uint8_t s_rx_buf[OPENDASH_MSG_MAX_SIZE];

/* ────────────────────────────────────────────────────────────────────────────
 * Low-Level I2C Helpers
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Write a protocol message to a slave.
 *
 * Serializes the message and writes it over I2C to the slave device handle.
 *
 * @param[in] slave  Pointer to slave info (contains dev_handle).
 * @param[in] msg    Protocol message to send.
 *
 * @return ESP_OK on success, or error from I2C driver.
 */
static esp_err_t master_write_msg(slave_info_t *slave, const opendash_i2c_msg_t *msg)
{
    uint16_t tx_len = 0;
    opendash_err_t od_ret = opendash_i2c_serialize(msg, s_tx_buf, &tx_len);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGE(TAG, "Serialize failed for cmd 0x%02X", msg->cmd);
        return ESP_FAIL;
    }

    esp_err_t ret = i2c_master_transmit(slave->dev_handle, s_tx_buf, tx_len,
                                         I2C_XFER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Write to 0x%02X failed: %s", slave->addr, esp_err_to_name(ret));
    }
    return ret;
}

/**
 * @brief Write a message then read a response from a slave (split transaction).
 *
 * The slave processes the command in its task context and queues a response
 * via i2c_slave_write(). We write first, wait a short delay for the slave
 * to process and load its TX FIFO, then perform a separate read.
 *
 * @param[in]  slave    Pointer to slave info.
 * @param[in]  msg      Protocol message to send.
 * @param[out] resp     Parsed response message.
 * @param[in]  rx_len   Expected response length in bytes.
 *
 * @return ESP_OK if write+read+deserialize all succeed.
 */
static esp_err_t master_write_read(slave_info_t *slave,
                                    const opendash_i2c_msg_t *msg,
                                    opendash_i2c_msg_t *resp,
                                    uint16_t rx_len)
{
    /* Phase 1: Write the command to the slave */
    esp_err_t ret = master_write_msg(slave, msg);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Phase 2: Wait for slave to process and queue its response.
     * The slave ISR fires, queues to FreeRTOS queue, the slave task
     * dequeues, builds a response, and calls i2c_slave_write().
     * 10ms is generous for this on ESP32-S3 at 240MHz.               */
    vTaskDelay(pdMS_TO_TICKS(10));

    /* Phase 3: Read the response from the slave */
    memset(s_rx_buf, 0, sizeof(s_rx_buf));
    ret = i2c_master_receive(slave->dev_handle, s_rx_buf, rx_len,
                              I2C_XFER_TIMEOUT_MS);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "Read from 0x%02X failed: %s",
                 slave->addr, esp_err_to_name(ret));
        return ret;
    }

    /* Phase 4: Deserialize the response */
    opendash_err_t od_ret = opendash_i2c_deserialize(s_rx_buf, rx_len, resp);
    if (od_ret != OPENDASH_OK) {
        ESP_LOGD(TAG, "Response from 0x%02X invalid (err=%d)", slave->addr, od_ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Slave Operations
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief PING a slave and check for STATUS_REPORT response.
 *
 * Sends SYSTEM(PING) and expects a STATUS_REPORT reply.
 * Updates online/offline status based on response.
 */
static void ping_slave(slave_info_t *slave)
{
    /* Build PING message: SYSTEM command with PING sub-command */
    opendash_i2c_msg_t ping_msg;
    uint8_t subcmd = OPENDASH_SUBCMD_PING;
    opendash_i2c_build_msg(&ping_msg, OPENDASH_CMD_SYSTEM, &subcmd, 1);

    /* Expected response: STATUS_REPORT = SYNC(1) + CMD(1) + LEN(1) + payload(3) + CHKSUM(1) = 7 bytes */
    opendash_i2c_msg_t resp;
    esp_err_t ret = master_write_read(slave, &ping_msg, &resp, 7);

    if (ret == ESP_OK && resp.cmd == OPENDASH_CMD_STATUS_REPORT) {
        /* Slave is alive */
        if (!slave->online) {
            ESP_LOGI(TAG, "Node %s (0x%02X) is ONLINE",
                     node_name(slave->node), slave->addr);
        }
        slave->online = true;
        slave->fail_count = 0;
        slave->last_seen_ms = (uint32_t)(esp_timer_get_time() / 1000);
    } else {
        /* PING failed */
        slave->fail_count++;
        if (slave->fail_count >= PING_FAIL_THRESHOLD && slave->online) {
            ESP_LOGW(TAG, "Node %s (0x%02X) went OFFLINE (%d failed pings)",
                     node_name(slave->node), slave->addr, slave->fail_count);
            slave->online = false;
        }
    }
}

/**
 * @brief Push a SET_DATA_POINT message to a slave.
 *
 * Payload: [dp_id:2 (big-endian)] [value:4 (float, memcpy)]
 */
static esp_err_t push_data_point(slave_info_t *slave, uint16_t dp_id, float value)
{
    if (!slave->online) return ESP_ERR_NOT_FOUND;

    uint8_t payload[6];
    payload[0] = (dp_id >> 8) & 0xFF;   /* dp_id high byte */
    payload[1] = dp_id & 0xFF;          /* dp_id low byte  */
    memcpy(&payload[2], &value, sizeof(float));

    opendash_i2c_msg_t msg;
    opendash_i2c_build_msg(&msg, OPENDASH_CMD_SET_DATA_POINT, payload, sizeof(payload));

    return master_write_msg(slave, &msg);
}

/**
 * @brief Request a data point from a slave.
 *
 * Sends REQUEST_DATA and reads back DATA_RESPONSE.
 * On success, returns the float value via *out_value.
 */
static esp_err_t request_data_point(slave_info_t *slave, uint16_t dp_id, float *out_value)
{
    if (!slave->online) return ESP_ERR_NOT_FOUND;

    uint8_t payload[2];
    payload[0] = dp_id & 0xFF;
    payload[1] = (dp_id >> 8) & 0xFF;

    opendash_i2c_msg_t req;
    opendash_i2c_build_msg(&req, OPENDASH_CMD_REQUEST_DATA, payload, sizeof(payload));

    /* DATA_RESPONSE: SYNC(1)+CMD(1)+LEN(1)+payload(10)+CHKSUM(1) = 14 bytes */
    opendash_i2c_msg_t resp;
    esp_err_t ret = master_write_read(slave, &req, &resp, 14);

    if (ret == ESP_OK && resp.cmd == OPENDASH_CMD_DATA_RESPONSE && resp.length >= 6) {
        /* Extract float value from payload bytes 2-5 */
        memcpy(out_value, &resp.payload[2], sizeof(float));
        return ESP_OK;
    }

    return ESP_FAIL;
}

/* ────────────────────────────────────────────────────────────────────────────
 * Demo Data Generator
 *
 * Until real OBD2/CAN data is available, generate sweeping demo values
 * to push to the gauge pods. This proves the communication works.
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Generate a sinusoidal sweep value for demo/testing. */
static float demo_sweep(float min, float max, float period_s, float phase)
{
    float t = (float)(esp_timer_get_time() / 1000) / 1000.0f;
    float norm = (sinf(2.0f * M_PI * t / period_s + phase) + 1.0f) / 2.0f;
    return min + norm * (max - min);
}

/* ────────────────────────────────────────────────────────────────────────────
 * I2C Master Polling Task
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Main polling task.
 *
 * Runs on core 0, polls slaves every POLL_INTERVAL_MS.
 */
static void i2c_master_task(void *pvParameters)
{
    ESP_LOGI(TAG, "I2C master task started");
    uint32_t cycle = 0;

    while (1) {
        /* ── Phase 1: PING all slaves ────────────────────────────── */
        for (int i = 0; i < MAX_SLAVES; i++) {
            ping_slave(&s_slaves[i]);
            vTaskDelay(pdMS_TO_TICKS(5));   /* Small gap between transactions */
        }

        /* Update global status snapshot */
        s_node_status.left_online  = s_slaves[0].online;
        s_node_status.right_online = s_slaves[1].online;
        s_node_status.gps_online   = s_slaves[2].online;

        /* ── Phase 2: Push demo data to online gauge pods ────────── */
        for (int i = 0; i < 2; i++) {   /* Slots 0=LEFT, 1=RIGHT */
            if (!s_slaves[i].online) continue;

            /* RPM: 800–7000 RPM sweep */
            float rpm = demo_sweep(800.0f, 7000.0f, 8.0f, 0.0f);
            push_data_point(&s_slaves[i], OPENDASH_DP_RPM, rpm);
            vTaskDelay(pdMS_TO_TICKS(2));

            /* Coolant temp: 70–110 °C slow sweep */
            float coolant = demo_sweep(70.0f, 110.0f, 30.0f, 1.0f);
            push_data_point(&s_slaves[i], OPENDASH_DP_COOLANT_TEMP, coolant);
            vTaskDelay(pdMS_TO_TICKS(2));

            /* Oil temp: 80–130 °C */
            float oil_temp = demo_sweep(80.0f, 130.0f, 25.0f, 2.0f);
            push_data_point(&s_slaves[i], OPENDASH_DP_OIL_TEMP, oil_temp);
            vTaskDelay(pdMS_TO_TICKS(2));

            /* Battery voltage: 12.0–14.8 V */
            float batt = demo_sweep(12.0f, 14.8f, 15.0f, 3.0f);
            push_data_point(&s_slaves[i], OPENDASH_DP_BATTERY_VOLTAGE, batt);
            vTaskDelay(pdMS_TO_TICKS(2));

            /* Boost pressure: 0–200 kPa */
            float boost = demo_sweep(0.0f, 200.0f, 6.0f, 0.5f);
            push_data_point(&s_slaves[i], OPENDASH_DP_BOOST_PRESSURE, boost);
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        /* ── Phase 3: Request GPS data (if GPS node is online) ──── */
        if (s_slaves[2].online) {
            float gps_speed = 0.0f;
            if (request_data_point(&s_slaves[2], OPENDASH_DP_GPS_SPEED, &gps_speed) == ESP_OK) {
                /* Push GPS speed to left and right gauge pods */
                for (int i = 0; i < 2; i++) {
                    if (s_slaves[i].online) {
                        push_data_point(&s_slaves[i], OPENDASH_DP_GPS_SPEED, gps_speed);
                    }
                }
                ESP_LOGD(TAG, "GPS speed: %.1f km/h", gps_speed);
            }
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        /* ── Phase 4: Periodic status log ────────────────────────── */
        if ((cycle % STATUS_LOG_INTERVAL) == 0) {
            ESP_LOGI(TAG, "Status: Left=%s  Right=%s  GPS=%s",
                     s_slaves[0].online ? "ONLINE" : "offline",
                     s_slaves[1].online ? "ONLINE" : "offline",
                     s_slaves[2].online ? "ONLINE" : "offline");
        }

        cycle++;
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

/* ────────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────────── */

esp_err_t i2c_master_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C master bus");
    ESP_LOGI(TAG, "  SDA: GPIO%d, SCL: GPIO%d, Port: %d",
             I2C_MASTER_SDA_PIN, I2C_MASTER_SCL_PIN, I2C_MASTER_PORT);
    ESP_LOGI(TAG, "  Clock: %d Hz", I2C_MASTER_FREQ_HZ);

    /* Configure the I2C master bus */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_MASTER_PORT,
        .sda_io_num = I2C_MASTER_SDA_PIN,
        .scl_io_num = I2C_MASTER_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C master bus: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add device handles for each slave */
    for (int i = 0; i < MAX_SLAVES; i++) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = s_slaves[i].addr,
            .scl_speed_hz = I2C_MASTER_FREQ_HZ,
        };

        ret = i2c_master_bus_add_device(s_bus_handle, &dev_cfg,
                                         &s_slaves[i].dev_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add slave 0x%02X: %s",
                     s_slaves[i].addr, esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "  Added slave: %s @ 0x%02X",
                 node_name(s_slaves[i].node), s_slaves[i].addr);
    }

    ESP_LOGI(TAG, "I2C master bus initialized");
    return ESP_OK;
}

esp_err_t i2c_master_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        i2c_master_task,
        "i2c_master",
        4096,
        NULL,
        4,                  /* Medium priority */
        &s_task_handle,
        0                   /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2C master task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2C master task started on core 0");
    return ESP_OK;
}

void i2c_master_get_status(i2c_master_node_status_t *status)
{
    if (status) {
        *status = s_node_status;
    }
}

esp_err_t i2c_master_send_data_point(opendash_node_t node,
                                      uint16_t dp_id, float value)
{
    for (int i = 0; i < MAX_SLAVES; i++) {
        if (s_slaves[i].node == node) {
            return push_data_point(&s_slaves[i], dp_id, value);
        }
    }
    return ESP_ERR_INVALID_ARG;
}

esp_err_t i2c_master_ping(opendash_node_t node)
{
    for (int i = 0; i < MAX_SLAVES; i++) {
        if (s_slaves[i].node == node) {
            ping_slave(&s_slaves[i]);
            return s_slaves[i].online ? ESP_OK : ESP_FAIL;
        }
    }
    return ESP_ERR_INVALID_ARG;
}
