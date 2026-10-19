/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file uart_bridge.h
 * @brief LC76G UART command-path probe — ITERATION-7 DEBUG (2026-10-19)
 *
 * Rationale: the owner's schematic review of the AMOLED-1.75 shows the LC76G
 * module UART is routed to the ESP32 (RXD net on GPIO17, TXD net on GPIO18 —
 * the same nets the POD3 conversion plan uses). All prior firmware treated
 * UART as "disabled — do NOT enable" folklore and never initialized UART1,
 * so every $PAIR command ever sent landed on the I2C-only read surface.
 *
 * This module enables UART1, auto-probes TX/RX direction and baud, and — if
 * the line is live — runs the spec'd command ladder:
 *   $PAIR051*3E (query fix interval) → expect $PAIR001,051 ACK + $PAIR051 echo
 *   $PAIR050,100*22 (set fix interval 100 ms) → expect $PAIR001,050 ACK
 * The measured GGA sentence rate is logged so the bench verdict is explicit.
 */

#ifndef GPS_UART_BRIDGE_H
#define GPS_UART_BRIDGE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Start the iteration-7 UART probe task (called once from app_main). */
esp_err_t esp_uart_bridge_probe_start(void);

/** True once the probe confirmed live bytes on the module UART. */
bool esp_uart_bridge_is_live(void);

/** Total bytes received on the module UART since boot. */
uint32_t esp_uart_bridge_rx_bytes(void);

/** Copy the most recent complete UART line (NUL-terminated) into out. */
void esp_uart_bridge_get_last_line(char *out, size_t out_sz);

/** Write one command line (CRLF appended) to the module UART. No-op before
 *  the probe confirms liveness; ESP_ERR_INVALID_STATE then. */
esp_err_t esp_uart_bridge_write_line(const char *line);

#endif /* GPS_UART_BRIDGE_H */