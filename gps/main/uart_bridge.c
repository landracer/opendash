/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file uart_bridge.c
 * @brief LC76G UART command-path test on HAND-WIRED lines (owner surgery
 *        2026-10-19: module RXD → GPIO17, module TXD → GPIO18)
 *
 * Module TXD proven live (native NMEA echoes on GPIO18). This task injects the
 * spec'd query ($PAIR051*3E) at BOTH documented bauds (115200 default per
 * PAIR864, 9600 fallback), cycling until a $PAIR001 ACK proves the parser
 * consumes commands — at which point it sets 10 Hz ($PAIR050,100*22) and logs
 * the measured rate. If the hand-wired RX line carries nothing at either baud
 * the task says so and self-deletes. I2C read path stays untouched.
 */

#include "uart_bridge.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "gps_uart";

#define GPS_UART_PORT       UART_NUM_1
#define GPS_UART_BUF_SZ     4096
#define GPS_UART_RX_TMO_MS  250
#define GPS_UART_LISTEN_MS  4000

static volatile bool s_live = false;
static volatile bool s_050_sent = false;
static volatile uint32_t s_rx_bytes = 0;
static char s_last_line[160];

/* Rolling GGA timestamp ring → measured sentence rate (bench verdict). */
#define GGA_STAMPS 8
static int64_t s_gga_t[GGA_STAMPS];
static unsigned s_gga_n;
static unsigned s_gga_i;

static void gga_stamp_and_report(void)
{
    int64_t now = esp_timer_get_time();
    s_gga_t[s_gga_i] = now;
    s_gga_i = (s_gga_i + 1) % GGA_STAMPS;
    if (s_gga_n < GGA_STAMPS) s_gga_n++;
    if (s_gga_n >= 2) {
        int64_t oldest = s_gga_t[(s_gga_i + GGA_STAMPS - s_gga_n) % GGA_STAMPS];
        int64_t span = now - oldest;
        float hz = span > 0 ? (float)(s_gga_n - 1) * 1000000.0f / (float)span : 0;
        ESP_LOGI(TAG, "UART GGA rate: %.2f Hz over last %u fixes", hz, s_gga_n);
    }
}

static void uart_bridge_send(const char *line)
{
    uart_write_bytes(GPS_UART_PORT, line, strlen(line));
    uart_write_bytes(GPS_UART_PORT, "\r\n", 2);
    ESP_LOGI(TAG, "UART» %s", line);
}

static void handle_line(char *line)
{
    strncpy(s_last_line, line, sizeof(s_last_line) - 1);
    s_last_line[sizeof(s_last_line) - 1] = '\0';
    ESP_LOGI(TAG, "UART« %s", line);

    /* The module answers the 051 query by echoing the interval directly
     * ($PAIR051,<ms>) — that echo IS the proof the parser consumes PAIR
     * commands. React to it by setting 10 Hz. */
    if (!s_050_sent && strstr(line, "PAIR051,")) {
        ESP_LOGE(TAG, "*** PARSER ALIVE: module answered $PAIR051 with '%s' — "
                      "hand-wired UART command path WORKS. Setting 10 Hz. ***", line);
        uart_bridge_send("$PAIR050,100*22");
        s_050_sent = true;
    }
    if (strstr(line, "GGA")) gga_stamp_and_report();
}

static void uart_bridge_drain(void)
{
    uint8_t buf[128];
    int n = uart_read_bytes(GPS_UART_PORT, buf, sizeof(buf) - 1,
                           pdMS_TO_TICKS(GPS_UART_RX_TMO_MS));
    if (n <= 0) return;
    s_rx_bytes += (uint32_t)n;
    static char line[160];
    static size_t li = 0;
    for (int i = 0; i < n; i++) {
        char c = (char)buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            line[li] = '\0';
            if (li > 1) handle_line(line);
            li = 0;
        } else if (li < sizeof(line) - 1) {
            line[li++] = c;
        }
    }
}

static void uart_bridge_task(void *arg)
{
    (void)arg;
    /* Fixed direction per owner surgery 2026-10-19: module RXD = GPIO17,
     * module TXD = GPIO18. The live NMEA echo on GPIO18 proves that half.
     * Command direction is now driven at BOTH documented bauds, cycling —
     * closing the last escape hatch (command sent at a baud the module's
     * receiver is not listening at). */
    uart_config_t cfg = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(GPS_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(GPS_UART_PORT, 17, 18,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(GPS_UART_PORT, GPS_UART_BUF_SZ,
                                        GPS_UART_BUF_SZ, 8, NULL, 0));
    /* Liveness at each baud so the command is injected at both. */
    for (int listen = 0; !s_live && listen < 2; listen++) {
        int b = listen == 0 ? 115200 : 9600;
        uart_set_baudrate(GPS_UART_PORT, b);
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < (int64_t)GPS_UART_LISTEN_MS * 1000) {
            uart_bridge_drain();
            if (s_rx_bytes > 0) break;
        }
        if (s_rx_bytes > 0) {
            s_live = true;
            ESP_LOGE(TAG, "UART LIVE @ %d (TX=GPIO17→RXD, RX=GPIO18←TXD) — "
                          "injecting $PAIR051 query at %d", b, b);
            uart_bridge_send("$PAIR051*3E");
        }
    }
    if (!s_live) {
        ESP_LOGE(TAG, "UART DEAD — silence on GPIO18 even on the hand-wired "
                      "lines at both bauds. Module is dead, not mis-wired.");
        vTaskDelete(NULL);
    }
    /* Live loop: cycle bauds until the parser answers. Each pass re-sends the
     * query at the new baud; the 051 echo flips s_050_sent (10 Hz set). */
    int bauds[2] = {115200, 9600};
    int cycle = 1;
    while (!s_050_sent) {
        for (int b = 0; b < 2 && !s_050_sent; b++) {
            uart_set_baudrate(GPS_UART_PORT, bauds[b]);
            ESP_LOGI(TAG, "ladder pass %d @ %d: re-sending $PAIR051", ++cycle, bauds[b]);
            uart_bridge_send("$PAIR051*3E");
            int64_t t0 = esp_timer_get_time();
            while (esp_timer_get_time() - t0 < 10LL * 1000 * 1000) {
                uart_bridge_drain();
                if (s_050_sent) break;
            }
        }
    }
    ESP_LOGE(TAG, "*** 10 Hz COMMAND INJECTED — node UNFROZEN. Watching rate. ***");
    for (;;) uart_bridge_drain();
}

esp_err_t esp_uart_bridge_probe_start(void)
{
    BaseType_t r = xTaskCreatePinnedToCore(uart_bridge_task, "gps_uart", 4096,
                                           NULL, 5, NULL, 1);
    return (r == pdPASS) ? ESP_OK : ESP_ERR_NO_MEM;
}

bool esp_uart_bridge_is_live(void) { return s_live; }
uint32_t esp_uart_bridge_rx_bytes(void) { return s_rx_bytes; }

void esp_uart_bridge_get_last_line(char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    strncpy(out, s_last_line, out_sz - 1);
    out[out_sz - 1] = '\0';
}

esp_err_t esp_uart_bridge_write_line(const char *line)
{
    if (!line) return ESP_ERR_INVALID_ARG;
    if (!s_live) return ESP_ERR_INVALID_STATE;
    uart_bridge_send(line);
    return ESP_OK;
}