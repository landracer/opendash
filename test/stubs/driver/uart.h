/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file stubs/driver/uart.h (host test stub)
 * @brief UART driver surface used by opendash_uart.c on the host.
 *        uart_read_bytes always reports "no data" — host tests drive the
 *        pure payload decoder directly, never the RX task.
 */
#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

typedef int uart_port_t;
#define UART_NUM_0 0
#define UART_NUM_1 1
#define UART_NUM_2 2

typedef enum { UART_DATA_8_BITS = 8 } uart_word_bit_t;
typedef enum { UART_PARITY_DISABLE = 0 } uart_parity_t;
typedef enum { UART_STOP_BITS_1 = 1 } uart_stop_bits_t;
typedef enum { UART_HW_FLOWCTRL_DISABLE = 0 } uart_hw_flowcontrol_t;
typedef enum { UART_SCLK_DEFAULT = 0 } uart_sclk_t;
typedef int  uart_mode_t;
typedef int  uart_pin_t;
#define UART_PIN_NO_CHANGE (-1)

typedef struct {
    int baud_rate;
    uart_word_bit_t     data_bits;
    uart_parity_t       parity;
    uart_stop_bits_t    stop_bits;
    uart_hw_flowcontrol_t flow_ctrl;
    uart_sclk_t         source_clk;
} uart_config_t;

esp_err_t uart_param_config(uart_port_t uart_num, const uart_config_t *uart_config);
esp_err_t uart_set_pin(uart_port_t uart_num, int tx, int rx, int rts, int cts);
esp_err_t uart_set_baudrate(uart_port_t uart_num, uint32_t baudrate);
esp_err_t uart_driver_install(uart_port_t uart_num, int rx_buffer_size,
                             int tx_buffer_size, int queue_size, void *uart_queue, int intr_alloc_flags);
esp_err_t uart_driver_delete(uart_port_t uart_num);
esp_err_t uart_flush_input(uart_port_t uart_num);
int uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);
int uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, uint32_t ticks_to_wait);