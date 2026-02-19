/**
 * @file i2c_node.c
 * @brief OpenDash GPS Unit — I2C Slave Node Handler
 *
 * Implements the OpenDash I2C slave interface at address 0x12 on the
 * inter-node bus. The Center display (I2C master) can:
 *   - Request GPS data (position, speed, heading, satellites)
 *   - Request IMU data (g-forces, gyro rates)
 *   - Send display configuration commands
 *   - Send brightness/system commands
 *
 * The inter-node I2C bus uses OPENDASH_I2C_SDA_PIN / OPENDASH_I2C_SCL_PIN,
 * which are SEPARATE from the BSP's onboard I2C bus (GPIO15/14).
 *
 * @see opendash_i2c_protocol.h for message format and command IDs.
 * @see ESP32 I2C Slave API:
 *      https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html
 */

#include "i2c_node.h"
#include "gps_handler.h"
#include "imu_handler.h"
#include "display_init.h"
#include "ui_manager.h"
#include "opendash_common.h"
#include "opendash_i2c_protocol.h"
#include "opendash_data_model.h"
#include "esp_log.h"
#include "driver/i2c_slave.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <math.h>

static const char *TAG = "i2c_node";

/* I2C slave configuration */
/*
 * The onboard sensor bus (touch, IMU, GPS) uses I2C_NUM_1 on GPIO15/14
 * (initialized by display_init.c). The inter-node bus MUST use a different
 * port AND different SDA pin. GPIO15 is taken, so we override SDA to GPIO17.
 * SCL remains GPIO16 per the common protocol header.
 */
#define I2C_NODE_PORT           I2C_NUM_0
#define I2C_NODE_SDA_PIN        GPIO_NUM_17   /* Override: GPIO15 used by onboard I2C */
#define I2C_NODE_SCL_PIN        OPENDASH_I2C_SCL_PIN  /* GPIO16 — free on this board */
#define I2C_NODE_RX_BUF_SIZE    OPENDASH_MSG_MAX_SIZE
#define I2C_NODE_TX_BUF_SIZE    OPENDASH_MSG_MAX_SIZE

/* Task handles */
static TaskHandle_t node_task_handle = NULL;
static i2c_slave_dev_handle_t slave_handle = NULL;

/* TX buffer (RX comes via ISR callback) */
static uint8_t tx_buffer[I2C_NODE_TX_BUF_SIZE];

/* Queue for passing received data from ISR to task */
typedef struct {
    uint8_t data[I2C_NODE_RX_BUF_SIZE];
    uint32_t length;
} i2c_rx_event_t;

static QueueHandle_t rx_queue = NULL;

/* ──────────────────────────────────────────────────────────────────────────
 * ISR Callbacks (run in interrupt context — keep minimal)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Called when master writes data to this slave.
 */
static bool IRAM_ATTR i2c_slave_on_receive(i2c_slave_dev_handle_t i2c_slave,
                                            const i2c_slave_rx_done_event_data_t *evt_data,
                                            void *arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    i2c_rx_event_t event;

    uint32_t copy_len = evt_data->length;
    if (copy_len > I2C_NODE_RX_BUF_SIZE) {
        copy_len = I2C_NODE_RX_BUF_SIZE;
    }
    memcpy(event.data, evt_data->buffer, copy_len);
    event.length = copy_len;

    xQueueSendFromISR(rx_queue, &event, &xHigherPriorityTaskWoken);
    return (xHigherPriorityTaskWoken == pdTRUE);
}

/**
 * @brief Called when master requests a read but TX FIFO is empty.
 *
 * We pre-load responses in process_message(), so this is just a fallback.
 */
static bool IRAM_ATTR i2c_slave_on_request(i2c_slave_dev_handle_t i2c_slave,
                                            const i2c_slave_request_event_data_t *evt_data,
                                            void *arg)
{
    /* Nothing to do — responses are queued proactively after processing */
    return false;
}

/* ──────────────────────────────────────────────────────────────────────────
 * Data Response Builders
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Build a GPS data response message.
 *
 * Packs current GPS data into a DATA_RESPONSE message.
 * Payload format: [dp_id:2][value:4][timestamp:4]
 */
static void build_gps_response(opendash_i2c_msg_t *msg, uint16_t dp_id)
{
    gps_data_t gps = {0};
    gps_handler_get_data(&gps);

    float value = 0.0f;
    switch (dp_id) {
        case 0x0100: value = (float)gps.speed;       break;  /* GPS Speed */
        case 0x0101: value = (float)gps.latitude;     break;  /* Latitude */
        case 0x0102: value = (float)gps.longitude;    break;  /* Longitude */
        case 0x0103: value = gps.altitude;            break;  /* Altitude */
        case 0x0104: value = gps.heading;             break;  /* Heading */
        case 0x0105: value = (float)gps.satellites;   break;  /* Sat count */
        case 0x0106: value = gps.hdop;                break;  /* HDOP */
        case 0x0107: value = gps.accuracy;            break;  /* Accuracy */
        default:     value = 0.0f;                    break;
    }

    /* Build payload: dp_id (2 bytes LE) + value (4 bytes, float) + timestamp (4 bytes) */
    uint8_t payload[10];
    payload[0] = dp_id & 0xFF;
    payload[1] = (dp_id >> 8) & 0xFF;
    memcpy(&payload[2], &value, sizeof(float));
    uint32_t timestamp = (uint32_t)(esp_log_timestamp());
    memcpy(&payload[6], &timestamp, sizeof(uint32_t));

    opendash_i2c_build_msg(msg, OPENDASH_CMD_DATA_RESPONSE, payload, sizeof(payload));
}

/**
 * @brief Build an IMU data response message.
 */
static void build_imu_response(opendash_i2c_msg_t *msg, uint16_t dp_id)
{
    imu_data_t imu = {0};
    imu_handler_get_data(&imu);

    float value = 0.0f;
    switch (dp_id) {
        case 0x0200: value = imu.g_lateral;       break;  /* G lateral */
        case 0x0201: value = imu.g_longitudinal;   break;  /* G longitudinal */
        case 0x0202: value = imu.g_vertical;       break;  /* G vertical */
        case 0x0203: value = imu.total_g;          break;  /* Total G */
        case 0x0204: value = imu.gyro_x;           break;  /* Gyro X */
        case 0x0205: value = imu.gyro_y;           break;  /* Gyro Y */
        case 0x0206: value = imu.gyro_z;           break;  /* Gyro Z */
        case 0x0207: value = imu.pitch;            break;  /* Pitch */
        case 0x0208: value = imu.roll;             break;  /* Roll */
        default:     value = 0.0f;                 break;
    }

    uint8_t payload[10];
    payload[0] = dp_id & 0xFF;
    payload[1] = (dp_id >> 8) & 0xFF;
    memcpy(&payload[2], &value, sizeof(float));
    uint32_t timestamp = (uint32_t)(esp_log_timestamp());
    memcpy(&payload[6], &timestamp, sizeof(uint32_t));

    opendash_i2c_build_msg(msg, OPENDASH_CMD_DATA_RESPONSE, payload, sizeof(payload));
}

/* ──────────────────────────────────────────────────────────────────────────
 * Message Processing
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Process a received message from the Center display.
 */
static void process_message(const opendash_i2c_msg_t *msg)
{
    switch (msg->cmd) {
        case OPENDASH_CMD_SET_DATA_POINT: {
            /* Master is pushing a data point value to us */
            if (msg->length >= 6) {
                uint16_t dp_id = msg->payload[0] | (msg->payload[1] << 8);
                float value;
                memcpy(&value, &msg->payload[2], sizeof(float));
                ui_manager_update_value(dp_id, value);
                ESP_LOGD(TAG, "Received data point 0x%04X = %.2f", dp_id, value);
            }
            break;
        }

        case OPENDASH_CMD_SET_BRIGHTNESS: {
            /* Master is setting our brightness */
            if (msg->length >= 1) {
                uint8_t brightness = msg->payload[0];
                display_set_brightness((brightness * 100) / 255);
                ESP_LOGI(TAG, "Brightness set to %d", brightness);
            }
            break;
        }

        case OPENDASH_CMD_REQUEST_DATA: {
            /* Master is requesting a data point from us */
            if (msg->length >= 2) {
                uint16_t dp_id = msg->payload[0] | (msg->payload[1] << 8);
                opendash_i2c_msg_t response;

                if (dp_id >= 0x0100 && dp_id <= 0x01FF) {
                    build_gps_response(&response, dp_id);
                } else if (dp_id >= 0x0200 && dp_id <= 0x02FF) {
                    build_imu_response(&response, dp_id);
                } else {
                    /* Unknown data point — send NAK */
                    uint8_t err_code = 0x01;  /* Unknown DP */
                    opendash_i2c_build_msg(&response, OPENDASH_CMD_NAK, &err_code, 1);
                }

                /* Serialize and queue for transmission */
                uint16_t tx_len = 0;
                if (opendash_i2c_serialize(&response, tx_buffer, &tx_len) == OPENDASH_OK) {
                    /* Write response to slave TX FIFO */
                    uint32_t written = 0;
                    i2c_slave_write(slave_handle, tx_buffer, tx_len, &written, 100);
                    ESP_LOGD(TAG, "Sent response for DP 0x%04X (%lu/%d bytes)", dp_id, written, tx_len);
                }
            }
            break;
        }

        case OPENDASH_CMD_SYSTEM: {
            if (msg->length >= 1) {
                switch (msg->payload[0]) {
                    case OPENDASH_SUBCMD_PING:
                        ESP_LOGI(TAG, "Ping received from master");
                        /* Respond with status report */
                        {
                            uint8_t status_payload[3] = {
                                OPENDASH_NODE_GPS,  /* Node ID */
                                0x01, 0x00          /* Flags: running, no errors */
                            };
                            opendash_i2c_msg_t response;
                            opendash_i2c_build_msg(&response, OPENDASH_CMD_STATUS_REPORT,
                                                    status_payload, sizeof(status_payload));
                            uint16_t tx_len = 0;
                            if (opendash_i2c_serialize(&response, tx_buffer, &tx_len) == OPENDASH_OK) {
                                uint32_t written = 0;
                                i2c_slave_write(slave_handle, tx_buffer, tx_len, &written, 100);
                            }
                        }
                        break;

                    case OPENDASH_SUBCMD_REBOOT:
                        ESP_LOGW(TAG, "Reboot command received — restarting...");
                        esp_restart();
                        break;

                    default:
                        ESP_LOGW(TAG, "Unknown system sub-command: 0x%02X", msg->payload[0]);
                        break;
                }
            }
            break;
        }

        default:
            ESP_LOGW(TAG, "Unhandled command: 0x%02X", msg->cmd);
            break;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * I2C Node Task
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief I2C slave communication task.
 *
 * Waits for messages from the ISR receive callback via a FreeRTOS queue,
 * then deserializes and processes them.
 */
static void i2c_node_task(void *pvParameters)
{
    ESP_LOGI(TAG, "I2C node task started (addr=0x%02X)", OPENDASH_I2C_ADDR_GPS);
    i2c_rx_event_t event;

    while (1) {
        /* Block until data arrives from ISR callback */
        if (xQueueReceive(rx_queue, &event, pdMS_TO_TICKS(500)) == pdTRUE) {
            /* Deserialize and validate the message */
            opendash_i2c_msg_t msg;
            opendash_err_t ret = opendash_i2c_deserialize(event.data, (uint16_t)event.length, &msg);

            if (ret == OPENDASH_OK) {
                process_message(&msg);
            } else {
                ESP_LOGW(TAG, "Invalid message received (err=%d, len=%lu)", ret, event.length);
            }
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────── */

esp_err_t i2c_node_init(void)
{
    ESP_LOGI(TAG, "Initializing I2C slave node");
    ESP_LOGI(TAG, "  Address: 0x%02X", OPENDASH_I2C_ADDR_GPS);
    ESP_LOGI(TAG, "  SDA: GPIO%d, SCL: GPIO%d", I2C_NODE_SDA_PIN, I2C_NODE_SCL_PIN);
    ESP_LOGI(TAG, "  Port: I2C_NUM_%d (onboard sensors on I2C_NUM_1)", I2C_NODE_PORT);

    /* Configure I2C slave */
    i2c_slave_config_t slave_cfg = {
        .i2c_port = I2C_NODE_PORT,
        .sda_io_num = I2C_NODE_SDA_PIN,
        .scl_io_num = I2C_NODE_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .send_buf_depth = I2C_NODE_TX_BUF_SIZE,
        .receive_buf_depth = I2C_NODE_RX_BUF_SIZE,
        .slave_addr = OPENDASH_I2C_ADDR_GPS,
        .addr_bit_len = I2C_ADDR_BIT_LEN_7,
    };

    esp_err_t ret = i2c_new_slave_device(&slave_cfg, &slave_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C slave: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Create RX event queue */
    rx_queue = xQueueCreate(8, sizeof(i2c_rx_event_t));
    if (rx_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create RX queue");
        return ESP_ERR_NO_MEM;
    }

    /* Register ISR callbacks for receive and request events */
    i2c_slave_event_callbacks_t cbs = {
        .on_receive = i2c_slave_on_receive,
        .on_request = i2c_slave_on_request,
    };
    ret = i2c_slave_register_event_callbacks(slave_handle, &cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register I2C callbacks: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "I2C slave node initialized");
    return ESP_OK;
}

esp_err_t i2c_node_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        i2c_node_task, "i2c_node", 4096, NULL,
        4,   /* Medium priority */
        &node_task_handle,
        0    /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create I2C node task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2C node task started on core 0");
    return ESP_OK;
}
