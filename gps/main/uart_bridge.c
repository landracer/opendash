/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file uart_bridge.c
 * @brief Iteration-7 debug: LC76G UART command-path probe (see uart_bridge.h)
 *
 * Auto-probe order (dir, baud): (TX=17,RX=18)@(115200 → 9600), then
 * (TX=18,RX=17)@(115200 → 9600). 115200 first: vendor spec §2.3.43 PAIR864
 * lists UART default baud = 115200. 4 s listen window per combo.
 * If live: logs every complete line, sends $PAIR051*3E, and on a
 * $PAIR001,051 ACK sends $PAIR050,100*22 (10 Hz). If dead: logs the verdict
 * and deletes itself — I2C read path stays untouched either way.
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

typedef struct { int8_t tx; int8_t rx; int baud; } gps_uart_combo_t;
static const gps_uart_combo_t k_combo_list[] = {
    { .tx = 17, .rx = 18, .baud = 115200 },
    { .tx = 17, .rx = 18, .baud = 9600 },
    { .tx = 18, .rx = 17, .baud = 115200 },
    { .tx = 18, .rx = 17, .baud = 9600 },
};

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

    if (strstr(line, "PAIR051") && strstr(line, "001")) {
        ESP_LOGE(TAG, "*** PARSER ALIVE: $PAIR001 ACK for 051 — the command "
                      "path EXISTS (UART). The 'command channel is not wired' "
                      "conclusion was FOLKLORE. Setting fix rate to 100 ms. ***");
        if (!s_050_sent) { uart_bridge_send("$PAIR050,100*22"); s_050_sent = true; }
    }
    if (strstr(line, "PAIR001,050"))
        ESP_LOGE(TAG, "*** RATE COMMAND ACKED (PAIR001,050) — node UNFROZEN ***");
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
    for (size_t c = 0; c < sizeof(k_combo_list) / sizeof(k_combo_list[0]); c++) {
        const gps_uart_combo_t combo = k_combo_list[c];
        uart_config_t cfg = {
            .baud_rate  = combo.baud,
            .data_bits  = UART_DATA_8_BITS,
            .parity     = UART_PARITY_DISABLE,
            .stop_bits  = UART_STOP_BITS_1,
            .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        };
        ESP_ERROR_CHECK(uart_param_config(GPS_UART_PORT, &cfg));
        ESP_ERROR_CHECK(uart_set_pin(GPS_UART_PORT, combo.tx, combo.rx,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(GPS_UART_PORT, GPS_UART_BUF_SZ,
                                            GPS_UART_BUF_SZ, 8, NULL, 0));
        ESP_LOGI(TAG, "UART probe %u/4: TX=GPIO%d RX=GPIO%d @ %d",
                 (unsigned)(c + 1), combo.tx, combo.rx, combo.baud);
        int64_t t0 = esp_timer_get_time();
        while (esp_timer_get_time() - t0 < (int64_t)GPS_UART_LISTEN_MS * 1000) {
            uart_bridge_drain();
            if (s_rx_bytes > 0) break;
        }
        if (s_rx_bytes > 0) {
            s_live = true;
            ESP_LOGE(TAG, "UART LIVE on TX=GPIO%d RX=GPIO%d @ %d — module "
                          "UART is wired exactly as the schematic says. "
                          "Querying fix interval ($PAIR051).",
                     combo.tx, combo.rx, combo.baud);
            uart_bridge_send("$PAIR051*3E");
            break;
        }
        uart_driver_delete(GPS_UART_PORT);
    }
    if (!s_live) {
        ESP_LOGE(TAG, "UART DEAD — silence on GPIO17/18 in BOTH directions "
                      "at 115200 AND 9600. I2C-only read-out reality stands; "
                      "module command path stays unreachable. Node stays 1 Hz.");
        vTaskDelete(NULL);
    }
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