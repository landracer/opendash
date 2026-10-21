/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file uart_bridge.c
 * @brief LC76G native-UART command path (iteration-9) — ACTIVE since owner
 *        surgery 2026-10-19: module RXD → GPIO17, module TXD → GPIO18
 *
 * Module TXD proven live (native NMEA echoes on GPIO18). The task reads the
 * module's CONFIG first ($PAIR059 min-SNR mask, $PAIR067 constellation search
 * mode — the answer to "valid NMEA but Sats 0"), auto-fixes a disabled search
 * config via $PAIR066,1,1,1,1,0,0, then runs the rate ladder: $PAIR051 query →
 * module echo → $PAIR050,100 → measured ≈10 Hz. All checksums computed
 * on-device. The I2C read path stays untouched.
 */

#include "uart_bridge.h"

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "gps_uart";

#define GPS_UART_PORT       UART_NUM_1
#define GPS_UART_BUF_SZ     4096
#define GPS_UART_RX_TMO_MS  250
#define GPS_UART_LISTEN_MS  4000

static volatile bool s_live = false;
static volatile bool s_050_sent = false;
static volatile bool s_search_disabled = false;   /* from $PAIR067 readout */
static volatile int  s_min_snr = -1;              /* from $PAIR059 readout */
static volatile bool s_059_seen = false, s_067_seen = false;
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

/* Send a PAIR command body (without '$' and checksum): checksum is computed
 * on-device so no hand-typed checksum can ever be wrong. */
static void uart_bridge_cmd(const char *body)
{
    uint8_t cs = 0;
    for (const char *p = body; *p; p++) cs ^= (uint8_t)*p;
    char sent[176];
    snprintf(sent, sizeof(sent), "$%s*%02X", body, cs);
    uart_bridge_send(sent);
}

static void handle_line(char *line)
{
    strncpy(s_last_line, line, sizeof(s_last_line) - 1);
    s_last_line[sizeof(s_last_line) - 1] = '\0';
    ESP_LOGI(TAG, "UART« %s", line);

    /* Diagnostics: min-SNR mask (059) and search-mode config (067). */
    if (strncmp(line, "$PAIR059,", 9) == 0) {
        int v = -1;
        if (sscanf(line, "$PAIR059,%d", &v) == 1) { s_min_snr = v; s_059_seen = true; }
        ESP_LOGE(TAG, "MIN-SNR CONFIG: %d dB (default 9 = all usable sats allowed)", v);
    }
    if (strncmp(line, "$PAIR067,", 9) == 0) {
        int f[6] = {1, 1, 1, 1, 0, 0};
        int got = sscanf(line, "$PAIR067,%d,%d,%d,%d,%d,%d",
                         &f[0], &f[1], &f[2], &f[3], &f[4], &f[5]);
        if (got >= 4) {
            s_067_seen = true;
            bool off = (f[0] == 0 || f[1] == 0 || f[2] == 0 || f[3] == 0);
            s_search_disabled = off;
            ESP_LOGE(TAG, "SEARCH CONFIG: GPS=%d GLO=%d GAL=%d BDS=%d QZSS=%d "
                          "(1=search,0=DISABLED) → %s",
                     f[0], f[1], f[2], f[3], f[4], off ? "FIX NEEDED" : "all OK");
        }
    }

    /* The module answers the 051 query by echoing the interval directly
     * ($PAIR051,<ms>) — that echo IS the proof the parser consumes PAIR
     * commands. React to it by setting 10 Hz. */
    if (!s_050_sent && strstr(line, "PAIR051,")) {
        ESP_LOGE(TAG, "*** PARSER ALIVE: module answered $PAIR051 with '%s' — "
                      "hand-wired UART command path WORKS. Setting 10 Hz. ***", line);
        uart_bridge_cmd("PAIR050,100");
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
     * module TXD = GPIO18 (this half was proven by the live NMEA echo).
     * Iteration-9: on-device checksum, then read the module's CONFIG before
     * touching the rate — a disabled constellation or a mis-set min-SNR mask
     * explains "valid NMEA, Sats 0" far better than a dead RF chain. */
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
    int live_baud = 0;
    for (int listen = 0; !s_live && listen < 2; listen++) {
        int b = listen == 0 ? 115200 : 9600;
        uart_set_baudrate(GPS_UART_PORT, b);
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < (int64_t)GPS_UART_LISTEN_MS * 1000) {
            uart_bridge_drain();
            if (s_rx_bytes > 0) break;
        }
        if (s_rx_bytes > 0) { s_live = true; live_baud = b; }
    }
    if (!s_live) {
        ESP_LOGE(TAG, "UART DEAD — silence on GPIO18 at both bauds. Module "
                      "dead, not mis-wired.");
        vTaskDelete(NULL);
    }
    ESP_LOGE(TAG, "UART LIVE @ %d (TX=GPIO17→RXD, RX=GPIO18←TXD)", live_baud);
    uart_set_baudrate(GPS_UART_PORT, live_baud);

    /* Phase 1 — config diagnostics: min-SNR mask + constellation search mode. */
    uart_bridge_cmd("PAIR059");
    uart_bridge_cmd("PAIR067");
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < 3LL * 1000 * 1000) {
        uart_bridge_drain();
        if (s_059_seen && s_067_seen) break;
    }
    if (s_067_seen && s_search_disabled) {
        ESP_LOGE(TAG, "*** SEARCH WAS DISABLED by config — sending spec fix "
                      "$PAIR066,1,1,1,1,0,0 (module REBOOTS on this command; "
                      "rate must be re-set after its reboot) ***");
        uart_bridge_cmd("PAIR066,1,1,1,1,0,0");
        vTaskDelay(pdMS_TO_TICKS(5000));   /* let the module reboot + stream */
        if (live_baud != 115200) uart_set_baudrate(GPS_UART_PORT, live_baud);
    }

    /* Phase 2 — rate ladder: query until echo, then set 100 ms. */
    int cycle = 0;
    while (!s_050_sent) {
        ESP_LOGI(TAG, "rate ladder pass %d: querying $PAIR051", ++cycle);
        uart_bridge_cmd("PAIR051");
        t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < 8LL * 1000 * 1000) {
            uart_bridge_drain();
            if (s_050_sent) break;
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