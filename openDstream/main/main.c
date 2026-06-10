/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file main.c
 * @brief openDstream — Headless ESP-NOW to USB Serial/JTAG Relay
 *
 * Hardware: ESP32-S3 (any variant with native USB-OTG)
 * Role: Pure relay node — receives all ESP-NOW frames from the network
 *       and streams them out via USB Serial/JTAG to multidisplay-app on PC
 *
 * This is a HEADLESS device — no display, no LVGL. Just:
 *   1. ESP-NOW slave (listens on channel 6)
 *   2. Pipes every frame to USB Serial/JTAG at 115200 baud
 *   3. That's it.
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
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"

static const char *TAG = "opendstream";

/* ESP-NOW */
#define ESPNOW_CHANNEL      6

/* Status LED — active-low on most devkits */
#define STATUS_LED_GPIO     GPIO_NUM_8

/* ════════════════════════════════════════════════════════════════════════════
 * USB Serial/JTAG — High-level driver API (ESP-IDF v6.x)
 * ════════════════════════════════════════════════════════════════════════════ */

static void usb_init(void)
{
    usb_serial_jtag_driver_config_t usb_config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t ret = usb_serial_jtag_driver_install(&usb_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG driver install failed: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "USB Serial/JTAG initialized");
}

static void usb_pipe(const uint8_t *data, int len)
{
    if (data == NULL || len <= 0) return;
    
    /* Write all bytes to USB — blocks until complete or timeout */
    size_t written = 0;
    while (written < len) {
        int to_write = len - written;
        int ret = usb_serial_jtag_write_bytes(data + written, to_write, pdMS_TO_TICKS(100));
        if (ret <= 0) {
            ESP_LOGW(TAG, "USB write failed");
            break;
        }
        written += ret;
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * ESP-NOW Receive Callback — the ONLY job of this node
 * ════════════════════════════════════════════════════════════════════════════ */

static void espnow_recv_cb(const esp_now_recv_info_t *info,
                           const uint8_t *data, int len)
{
    /* Pipe the raw frame to USB — that's all */
    usb_pipe(data, len);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Init
 * ════════════════════════════════════════════════════════════════════════════ */

static void espnow_init(void)
{
    if (esp_now_init() != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed");
        return;
    }

    wifi_config_t wf = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .channel = ESPNOW_CHANNEL,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wf);
    esp_wifi_start();

    esp_now_register_recv_cb(espnow_recv_cb);
    ESP_LOGI(TAG, "ESP-NOW recv cb registered (ch %d)", ESPNOW_CHANNEL);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Status LED — double-blink alive
 * ════════════════════════════════════════════════════════════════════════════ */

static void led_task(void *arg)
{
    gpio_config_t gc = {
        .pin_bit_mask = (1ULL << STATUS_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gc);

    while (1) {
        gpio_set_level(STATUS_LED_GPIO, 0);  // ON
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(STATUS_LED_GPIO, 1);  // OFF
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(STATUS_LED_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(80));
        gpio_set_level(STATUS_LED_GPIO, 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

/* ════════════════════════════════════════════════════════════════════════════
 * Entry
 * ════════════════════════════════════════════════════════════════════════════ */

void app_main(void)
{
    ESP_LOGI(TAG, "openDstream relay starting");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    usb_init();
    espnow_init();

    /* Status LED */
    xTaskCreate(led_task, "led", 1024, NULL, 3, NULL);

    ESP_LOGI(TAG, "Relay ready — piping ESP-NOW to USB Serial/JTAG");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
