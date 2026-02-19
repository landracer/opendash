/**
 * @file gps_handler.c
 * @brief OpenDash GPS Handler — LC76G via I2C
 *
 * Interfaces with the LC76G GNSS module over I2C to read NMEA sentences
 * and parse position, speed, heading, and satellite data.
 *
 * LC76G I2C Protocol (Waveshare board):
 *   Write address: 0x50
 *   Read address:  0x54
 *
 * Communication steps:
 *   1. Send query command to request available data length
 *   2. Read 4 bytes → little-endian uint32 data length
 *   3. Send read command with the data length
 *   4. Read N bytes of ASCII NMEA data
 *
 * Supported NMEA sentences:
 *   $GPRMC — Position, speed, heading, date/time
 *   $GPGGA — Fix quality, altitude, satellites, HDOP
 *
 * @see ESP32 I2C API:
 *      https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2c.html
 */

#include "gps_handler.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "display_init.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "gps_handler";

/* LC76G I2C addresses (7-bit) */
#define LC76G_I2C_ADDR_WRITE    0x28    /* 0x50 >> 1 = 0x28 (7-bit) */
#define LC76G_I2C_ADDR_READ     0x2A    /* 0x54 >> 1 = 0x2A (7-bit) */

/* LC76G I2C command bytes */
static const uint8_t LC76G_CMD_QUERY_LEN[] = {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
static const uint8_t LC76G_CMD_READ_HDR[]  = {0x00, 0x20, 0x51, 0xAA};

/* Maximum NMEA buffer size */
#define GPS_NMEA_BUF_SIZE       2048
#define GPS_READ_INTERVAL_MS    200     /* 5 Hz polling */

/* GPS state */
static gps_data_t current_gps_data = {0};
static SemaphoreHandle_t gps_mutex = NULL;
static TaskHandle_t gps_task_handle = NULL;
static i2c_master_dev_handle_t lc76g_write_handle = NULL;
static i2c_master_dev_handle_t lc76g_read_handle = NULL;

/* NMEA line parsing buffer */
static char nmea_line[256];
static int nmea_line_pos = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * NMEA Parsing Helpers
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Convert NMEA latitude/longitude (DDmm.mmmm) to decimal degrees.
 */
static double nmea_to_degrees(const char *val, const char *dir)
{
    if (val == NULL || val[0] == '\0') return 0.0;

    double raw = atof(val);
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double result = degrees + (minutes / 60.0);

    if (dir && (dir[0] == 'S' || dir[0] == 'W')) {
        result = -result;
    }
    return result;
}

/**
 * @brief Get the nth comma-separated field from an NMEA sentence.
 * @param sentence  Full NMEA sentence string.
 * @param field     Field index (0-based, 0 = sentence type e.g. "$GPRMC").
 * @param out       Output buffer.
 * @param out_len   Output buffer size.
 * @return true if field was found.
 */
static bool nmea_get_field(const char *sentence, int field, char *out, size_t out_len)
{
    int current_field = 0;
    const char *p = sentence;

    while (*p && current_field < field) {
        if (*p == ',') current_field++;
        p++;
    }
    if (current_field != field) {
        out[0] = '\0';
        return false;
    }

    size_t i = 0;
    while (*p && *p != ',' && *p != '*' && *p != '\r' && *p != '\n' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

/**
 * @brief Parse $GPRMC sentence (or $GNRMC).
 *
 * Fields: $GPRMC,time,status,lat,N/S,lon,E/W,speed_knots,heading,date,...
 */
static void parse_rmc(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Field 1: UTC time (hhmmss.ss) */
    if (nmea_get_field(sentence, 1, field, sizeof(field)) && strlen(field) >= 6) {
        data->hour   = (field[0] - '0') * 10 + (field[1] - '0');
        data->minute = (field[2] - '0') * 10 + (field[3] - '0');
        data->second = (field[4] - '0') * 10 + (field[5] - '0');
    }

    /* Field 2: Status (A=active/valid, V=void) */
    if (nmea_get_field(sentence, 2, field, sizeof(field))) {
        data->fix_valid = (field[0] == 'A');
    }

    /* Fields 3-4: Latitude + N/S */
    char lat_val[20], lat_dir[4];
    nmea_get_field(sentence, 3, lat_val, sizeof(lat_val));
    nmea_get_field(sentence, 4, lat_dir, sizeof(lat_dir));
    if (lat_val[0]) {
        data->latitude = nmea_to_degrees(lat_val, lat_dir);
    }

    /* Fields 5-6: Longitude + E/W */
    char lon_val[20], lon_dir[4];
    nmea_get_field(sentence, 5, lon_val, sizeof(lon_val));
    nmea_get_field(sentence, 6, lon_dir, sizeof(lon_dir));
    if (lon_val[0]) {
        data->longitude = nmea_to_degrees(lon_val, lon_dir);
    }

    /* Field 7: Speed over ground (knots → km/h) */
    if (nmea_get_field(sentence, 7, field, sizeof(field)) && field[0]) {
        data->speed = atof(field) * 1.852f;  /* knots to km/h */
    }

    /* Field 8: Track angle / heading (degrees true) */
    if (nmea_get_field(sentence, 8, field, sizeof(field)) && field[0]) {
        data->heading = atof(field);
    }
}

/**
 * @brief Parse $GPGGA sentence (or $GNGGA).
 *
 * Fields: $GPGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,...
 */
static void parse_gga(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Field 6: Fix quality (0=invalid, 1=GPS, 2=DGPS, 6=estimated) */
    if (nmea_get_field(sentence, 6, field, sizeof(field)) && field[0]) {
        data->fix_quality = atoi(field);
        if (data->fix_quality > 0) {
            data->fix_valid = true;
        }
    }

    /* Field 7: Satellites used */
    if (nmea_get_field(sentence, 7, field, sizeof(field)) && field[0]) {
        data->satellites = atoi(field);
    }

    /* Field 8: HDOP */
    if (nmea_get_field(sentence, 8, field, sizeof(field)) && field[0]) {
        data->hdop = atof(field);
        /* Rough accuracy estimate: HDOP * 5 meters (typical GPS) */
        data->accuracy = data->hdop * 5.0f;
    }

    /* Field 9: Altitude above MSL */
    if (nmea_get_field(sentence, 9, field, sizeof(field)) && field[0]) {
        data->altitude = atof(field);
    }
}

/**
 * @brief Process a complete NMEA line.
 */
static void process_nmea_line(const char *line, gps_data_t *data)
{
    if (line[0] != '$') return;

    /* Match sentence types (supports GP, GN, GL, GA prefixes) */
    const char *type = line + 3;  /* Skip "$GP", "$GN", etc. */

    if (strncmp(type, "RMC", 3) == 0) {
        parse_rmc(line, data);
    } else if (strncmp(type, "GGA", 3) == 0) {
        parse_gga(line, data);
    }
    /* Other sentences (GSA, GSV, VTG) can be added here as needed */
}

/* ──────────────────────────────────────────────────────────────────────────
 * LC76G I2C Communication
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Query available data length from LC76G.
 * @return Number of bytes available, 0 on error or no data.
 */
static uint32_t lc76g_query_data_length(void)
{
    esp_err_t ret;

    /* Send query command */
    ret = i2c_master_transmit(lc76g_write_handle, LC76G_CMD_QUERY_LEN,
                              sizeof(LC76G_CMD_QUERY_LEN), 100);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "LC76G query command failed: %s", esp_err_to_name(ret));
        return 0;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    /* Read 4 bytes: little-endian uint32 data length */
    uint8_t len_buf[4] = {0};
    ret = i2c_master_receive(lc76g_read_handle, len_buf, 4, 100);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "LC76G length read failed: %s", esp_err_to_name(ret));
        return 0;
    }

    uint32_t data_len = len_buf[0] | (len_buf[1] << 8) |
                        (len_buf[2] << 16) | (len_buf[3] << 24);

    if (data_len > GPS_NMEA_BUF_SIZE) {
        ESP_LOGW(TAG, "LC76G reported %lu bytes (capped to %d)", data_len, GPS_NMEA_BUF_SIZE);
        data_len = GPS_NMEA_BUF_SIZE;
    }

    return data_len;
}

/**
 * @brief Read NMEA data from LC76G.
 * @param buf       Output buffer.
 * @param buf_size  Buffer capacity.
 * @param out_len   Actual bytes read.
 * @return ESP_OK on success.
 */
static esp_err_t lc76g_read_nmea_data(uint8_t *buf, size_t buf_size, uint32_t *out_len)
{
    uint32_t data_len = lc76g_query_data_length();
    if (data_len == 0) {
        *out_len = 0;
        return ESP_OK;  /* No data available — not an error */
    }

    if (data_len > buf_size) data_len = buf_size;

    /* Build read command: header + length bytes */
    uint8_t cmd[8];
    memcpy(cmd, LC76G_CMD_READ_HDR, 4);
    cmd[4] = (data_len >>  0) & 0xFF;
    cmd[5] = (data_len >>  8) & 0xFF;
    cmd[6] = (data_len >> 16) & 0xFF;
    cmd[7] = (data_len >> 24) & 0xFF;

    esp_err_t ret = i2c_master_transmit(lc76g_write_handle, cmd, sizeof(cmd), 100);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "LC76G read cmd failed: %s", esp_err_to_name(ret));
        *out_len = 0;
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    ret = i2c_master_receive(lc76g_read_handle, buf, data_len, 200);
    if (ret != ESP_OK) {
        ESP_LOGD(TAG, "LC76G data read failed: %s", esp_err_to_name(ret));
        *out_len = 0;
        return ret;
    }

    *out_len = data_len;
    return ESP_OK;
}

/* ──────────────────────────────────────────────────────────────────────────
 * GPS Task
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief GPS reading task.
 *
 * Continuously reads NMEA data from the LC76G via I2C and updates GPS state.
 */
static void gps_task(void *pvParameters)
{
    ESP_LOGI(TAG, "GPS task started (I2C polling at %d Hz)", 1000 / GPS_READ_INTERVAL_MS);

    static uint8_t nmea_buf[GPS_NMEA_BUF_SIZE];
    gps_data_t local_data = {0};

    while (1) {
        uint32_t bytes_read = 0;
        esp_err_t ret = lc76g_read_nmea_data(nmea_buf, sizeof(nmea_buf) - 1, &bytes_read);

        if (ret == ESP_OK && bytes_read > 0) {
            nmea_buf[bytes_read] = '\0';

            /* Feed bytes into line parser */
            for (uint32_t i = 0; i < bytes_read; i++) {
                char c = (char)nmea_buf[i];

                if (c == '\n' || c == '\r') {
                    if (nmea_line_pos > 0) {
                        nmea_line[nmea_line_pos] = '\0';
                        process_nmea_line(nmea_line, &local_data);
                        nmea_line_pos = 0;
                    }
                } else if (nmea_line_pos < (int)(sizeof(nmea_line) - 1)) {
                    nmea_line[nmea_line_pos++] = c;
                }
            }

            /* Thread-safe update */
            if (gps_mutex && xSemaphoreTake(gps_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                memcpy(&current_gps_data, &local_data, sizeof(gps_data_t));
                xSemaphoreGive(gps_mutex);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(GPS_READ_INTERVAL_MS));
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────── */

esp_err_t gps_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing GPS handler (LC76G via I2C)");
    ESP_LOGI(TAG, "  Write addr: 0x%02X, Read addr: 0x%02X",
             LC76G_I2C_ADDR_WRITE, LC76G_I2C_ADDR_READ);

    /* Create mutex for thread-safe GPS data access */
    gps_mutex = xSemaphoreCreateMutex();
    if (gps_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create GPS mutex");
        return ESP_FAIL;
    }

    /* Get the shared I2C bus handle from display_init */
    i2c_master_bus_handle_t i2c_bus = display_get_i2c_handle();
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2C bus handle is NULL — call display_init() first");
        return ESP_FAIL;
    }

    /* Add LC76G write device (0x28 = 0x50 >> 1) */
    i2c_device_config_t write_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LC76G_I2C_ADDR_WRITE,
        .scl_speed_hz = 100000,  /* LC76G requires 100 kHz */
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &write_cfg, &lc76g_write_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add LC76G write device: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Add LC76G read device (0x2A = 0x54 >> 1) */
    i2c_device_config_t read_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LC76G_I2C_ADDR_READ,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(i2c_bus, &read_cfg, &lc76g_read_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add LC76G read device: %s", esp_err_to_name(ret));
        return ret;
    }

    memset(&current_gps_data, 0, sizeof(gps_data_t));

    ESP_LOGI(TAG, "GPS handler initialized — LC76G registered on I2C bus");
    return ESP_OK;
}

esp_err_t gps_handler_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        gps_task, "gps_task", 8192, NULL,
        5,   /* High priority for real-time GPS data */
        &gps_task_handle,
        0    /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GPS task started on core 0");
    return ESP_OK;
}

esp_err_t gps_handler_get_data(gps_data_t *data)
{
    if (data == NULL) return ESP_ERR_INVALID_ARG;

    if (gps_mutex && xSemaphoreTake(gps_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(data, &current_gps_data, sizeof(gps_data_t));
        xSemaphoreGive(gps_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}
