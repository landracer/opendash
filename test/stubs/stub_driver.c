/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file stubs/stub_driver.c (host test stub)
 * @brief Driver-side no-op implementations (GPIO/UART) for host builds.
 *        Nothing here is ever executed by the fuzz/unit tests — these exist
 *        so opendash_uart.c compiles and links on the host; only the pure
 *        payload decoder is called directly.
 */
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/task.h"
#include <stddef.h>

BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t depth,
                       void *arg, UBaseType_t prio, TaskHandle_t *handle)
{ (void)fn; (void)name; (void)depth; (void)arg; (void)prio;
  if (handle) *handle = (void *)1; /* "created" — never scheduled on host */
  return pdPASS; }
void vTaskDelay(TickType_t ticks) { (void)ticks; }
void vTaskSuspend(TaskHandle_t t) { (void)t; }

esp_err_t gpio_reset_pin(gpio_num_t gpio_num) { (void)gpio_num; return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *pGPIOConfig) { (void)pGPIOConfig; return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode) { (void)gpio_num; (void)mode; return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level) { (void)gpio_num; (void)level; return ESP_OK; }
esp_err_t gpio_pullup_en(gpio_num_t gpio_num) { (void)gpio_num; return ESP_OK; }
int       gpio_get_level(gpio_num_t gpio_num) { (void)gpio_num; return 1; /* idle HIGH */ }

void esp_rom_delay_us(uint32_t us) { (void)us; }

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config)
{ (void)uart_num; (void)uart_config; return ESP_OK; }
esp_err_t uart_set_pin(uart_port_t uart_num, int tx, int rx, int rts, int cts)
{ (void)uart_num; (void)tx; (void)rx; (void)rts; (void)cts; return ESP_OK; }
esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baudrate)
{ (void)uart_num; (void)baudrate; return ESP_OK; }
esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size,
                             int tx_buffer_size, int queue_size, void *uart_queue,
                             int intr_alloc_flags)
{ (void)uart_num; (void)rx_buffer_size; (void)tx_buffer_size;
  (void)queue_size; (void)uart_queue; (void)intr_alloc_flags; return ESP_OK; }
esp_err_t uart_driver_delete(uart_port_t uart_num) { (void)uart_num; return ESP_OK; }
esp_err_t uart_flush_input(uart_port_t uart_num) { (void)uart_num; return ESP_OK; }
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{ (void)uart_num; (void)src; return (int)size; }
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, uint32_t ticks_to_wait)
{ (void)uart_num; (void)buf; (void)length; (void)ticks_to_wait; return 0; }