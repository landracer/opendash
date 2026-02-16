/**
 * @file gps_handler.c
 * @brief OpenDash GPS Handler Implementation
 *
 * Interfaces with the LC76G GNSS module via UART to read GPS data.
 * Parses NMEA sentences and provides position, speed, and heading data.
 *
 * @see ESP32 UART API:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/peripherals/uart.html
 */

#include "gps_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "gps_handler";

/* Latest GPS data */
static gps_data_t current_gps_data = {0};
static TaskHandle_t gps_task_handle = NULL;

/**
 * @brief GPS reading task.
 *
 * Continuously reads from the GPS module UART and parses NMEA sentences.
 */
static void gps_task(void *pvParameters)
{
    ESP_LOGI(TAG, "GPS task started");
    
    while (1) {
        /* Future implementation:
         * 1. Read NMEA sentences from UART
         * 2. Parse GGA/RMC sentences for position, speed, heading
         * 3. Update current_gps_data structure
         * 4. Handle fix status and satellite count
         */
        
        /* For baseline, set dummy data to indicate GPS is "acquiring" */
        current_gps_data.fix_valid = false;
        current_gps_data.satellites = 0;
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t gps_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing GPS handler for LC76G module");
    
    /* Future implementation:
     * 1. Configure UART for GPS module (9600 baud, 8N1)
     * 2. Initialize NMEA parser
     * 3. Configure GPS module settings if needed
     */
    
    /* Initialize GPS data structure */
    memset(&current_gps_data, 0, sizeof(gps_data_t));
    
    ESP_LOGI(TAG, "GPS handler initialized");
    return ESP_OK;
}

esp_err_t gps_handler_start(void)
{
    /* Create GPS reading task */
    BaseType_t ret = xTaskCreatePinnedToCore(
        gps_task,
        "gps_task",
        4096,
        NULL,
        5,  /* High priority for real-time GPS data */
        &gps_task_handle,
        0   /* Core 0 */
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "GPS task created on core 0");
    return ESP_OK;
}

esp_err_t gps_handler_get_data(gps_data_t *data)
{
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    /* Copy current GPS data */
    memcpy(data, &current_gps_data, sizeof(gps_data_t));
    
    return ESP_OK;
}
