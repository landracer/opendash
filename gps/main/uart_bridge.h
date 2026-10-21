/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file uart_bridge.h
 * @brief LC76G native-UART command path — ACTIVE (iteration-8, 2026-10-19)
 *
 * Rationale: the LC76G module's native UART was never connected to the ESP32
 * by the module socket (schematic "NC/GPS RX|TX" = No Connect — later proven by
 * measurement AND by owner surgery: the owner hand-wired module TXD→GPIO18 and
 * RXD→GPIO17). With the wires in place, this module enables UART1 and runs the
 * spec'd command ladder at both documented bauds (115200 default per PAIR864,
 * 9600 fallback) until the module answers. Bench-proven behavior:
 *   $PAIR051*3E (query fix interval) → module echoes $PAIR051,<ms>  (no PAIR001
 *   ACK sentence is emitted by this module — the echo IS the response)
 *   → uart_bridge sends $PAIR050,100*22 → measured ≈10 Hz output.
 * The measured GGA sentence rate is logged so the bench verdict is explicit.
 */

#ifndef GPS_UART_BRIDGE_H
#define GPS_UART_BRIDGE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Start the UART command-path task (called once from app_main). */
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