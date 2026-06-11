/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief openDstream — ESP-NOW to UART Relay Node
 *
 * Hardware: ESP32-WROOM-32 (classic) with USB-to-UART bridge
 * Role: Pure relay — receives ESP-NOW frames, pipes them to UART @ 115200 baud
 *
 * Headless device — no display, no LVGL. Just:
 *   1. ESP-NOW slave listening on channel 6
 *   2. Pipes every frame to UART0
 *   3. Done.
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "opendstream";

/* UART Config */
#define UART_NUM            UART_NUM_0
#define UART_BAUD           115200
#define UART_TX             GPIO_NUM_1
#define UART_RX             GPIO_NUM_3

/* ESP-NOW */
#define ESPNOW_CHANNEL      6

/* Status LED (GPIO2, active-low on most boards) */
#define LED_GPIO            GPIO_NUM_2

/* ════════════════════════════════════════════════════════════════════════════
 * UART — Standard driver
 * ════════════════════════════════════════════════════════════════════════════ */

static void uart_init(void)
{
    uart_config_t uc = {
        .baud_rate = UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    uart_driver_install(UART_NUM, 4096, 4096, 0, NULL, 0);
    uart_param_config(UART_NUM, &uc);
    uart_set_pin(UART_NUM, UART_TX, UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    ESP_LOGI(TAG, "UART0 @ %d baud (TX:GPIO%d)", UART_BAUD, UART_TX);
}

static void uart_send(const uint8_t *data, size_t len)
{
    if (data && len > 0) {
        uart_write_bytes(UART_NUM, data, len);
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Receive Callback — pipe everything to UART
 * ════════════════════════════════════════════════════════════════════════════ */

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    uart_send(data, len);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Init
 * ════════════════════════════════════════════════════════════════════════════ */

static void espnow_init(void)
{
    esp_now_init();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    esp_now_register_recv_cb(espnow_recv_cb);
    ESP_LOGI(TAG, "ESP-NOW recv cb registered (ch %d)", ESPNOW_CHANNEL);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Status LED — double-blink alive
 * ════════════════════════════════════════════════════════════════════════════ */

static void led_task(void *arg)
{
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

    while (1) {
        gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(LED_GPIO, 0); vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(LED_GPIO, 1); vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Entry
 * ════════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "openDstream relay starting");

    /* NVS */
    nvs_flash_init();

    uart_init();
    espnow_init();

    xTaskCreate(led_task, "led", 1024, NULL, 3, NULL);

    ESP_LOGI(TAG, "Relay ready — piping ESP-NOW to UART0");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
