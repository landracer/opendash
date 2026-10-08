/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file stubs/driver/gpio.h (host test stub)
 * @brief GPIO type + function surface used by opendash_uart.c on the host.
 */
#pragma once
#include <stdint.h>
#include "esp_err.h"

typedef int gpio_num_t;
typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT } gpio_mode_t;
typedef enum { GPIO_PULLUP_DISABLE = 0, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE = 0, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE = 0, GPIO_INTR_ANYEDGE } gpio_int_type_t;

typedef struct {
    uint64_t        pin_bit_mask;
    gpio_mode_t     mode;
    gpio_pullup_t   pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;

esp_err_t gpio_reset_pin(gpio_num_t gpio_num);
esp_err_t gpio_config(const gpio_config_t *pGPIOConfig);
esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level);
esp_err_t gpio_pullup_en(gpio_num_t gpio_num);
int       gpio_get_level(gpio_num_t gpio_num);

/* esp_rom_delay_us arrives transitively via driver headers on the target. */
void esp_rom_delay_us(uint32_t us);