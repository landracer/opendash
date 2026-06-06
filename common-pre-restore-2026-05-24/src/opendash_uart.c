/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_uart.c
 * @brief UART receiver for multidisplay serial data (SERIALOUT_BINARY)
 *
 * Parses the multidisplay binary serial output and pushes values into the
 * shared opendash data store.  Has NO display-specific dependencies —
 * UI/buzzer actions are triggered from main.c via status helpers.
 *
 * Wire format (SERIALOUT_BINARY):
 *   STX(0x02) TAG(0x5F) payload[93] ETX(0x03)
 *   Total: 95 bytes.  All multi-byte fields are little-endian (AVR).
 *   NO byte-stuffing — 0x02/0x03 CAN appear in data.
 *   Parser uses fixed-size reads after detecting STX + TAG.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "hal/usb_serial_jtag_ll.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "opendash_uart.h"

static const char *TAG = "opendash_uart";

/* ── UART hardware config ─────────────────────────────────────────────── */
#define MD_UART_PORT       UART_NUM_1
#define MD_UART_BAUD       115200
#define MD_UART_BUF_SIZE   512

/* Timeout: if no valid frame arrives for this many ms, status → TIMEOUT */
#define MD_DATA_TIMEOUT_MS 3000

/* ── Multidisplay binary frame constants ───────────────────────────────── */
#define MD_STX            0x02
#define MD_ETX            0x03
#define MD_BINARY_TAG     0x5F   /* SERIALOUT_BINARY_TAG = 95 */
#define MD_FRAME_SIZE     95     /* Total bytes: STX + 93 payload + ETX */
#define MD_PAYLOAD_SIZE   93     /* Bytes between STX and ETX */

/* Binary frame field offsets (relative to payload start, after TAG byte)
 * TAG is payload[0], so data starts at payload[1].
 * Offsets below are from the start of the full payload (after STX).      */
#define MD_OFF_TAG        0      /* uint8:  0x5F */
#define MD_OFF_TIME       1      /* uint32: millis() */
#define MD_OFF_RPM        5      /* int16:  RPM */
#define MD_OFF_BOOST      7      /* uint16: boost × 100 (bar absolute) */
#define MD_OFF_THROTTLE   9      /* uint8:  throttle 0-100% */
#define MD_OFF_LAMBDA     10     /* uint16: lambda × 100 */
#define MD_OFF_LMM        12     /* uint16: LMM × 100 */
#define MD_OFF_CASETEMP   14     /* uint16: case temp × 100 */
#define MD_OFF_EGT        16     /* int16[8]: EGT 0-7 in °C */
#define MD_OFF_BATVOLT    32     /* uint16: battery × 100 */
#define MD_OFF_VDOPRES1   34     /* int16: VDO pressure 1 */
#define MD_OFF_VDOPRES2   36     /* int16: VDO pressure 2 */
#define MD_OFF_VDOPRES3   38     /* int16: VDO pressure 3 */
#define MD_OFF_VDOTEMP1   40     /* int16: VDO temp 1 */
#define MD_OFF_VDOTEMP2   42     /* int16: VDO temp 2 */
#define MD_OFF_VDOTEMP3   44     /* int16: VDO temp 3 */
#define MD_OFF_SPEED      46     /* uint16: speed × 100 */
#define MD_OFF_GEAR       48     /* uint8:  gear 0-6 */
#define MD_OFF_N75        49     /* uint8:  N75 duty cycle */
#define MD_OFF_REQBOOST   50     /* uint16: requested boost × 100 */
#define MD_OFF_EFR        54     /* uint16: EFR turbo speed */
#define MD_OFF_KNOCK      56     /* uint16: knock sensor */

/* ── Module state ─────────────────────────────────────────────────────── */
static opendash_uart_status_t s_status = OPENDASH_UART_DISABLED;
static int64_t                s_last_valid_frame_us = 0;
static bool                   s_connected_event = false;  /* one-shot flag */
static opendash_md_data_t     s_latest_data;              /* latest parse result */
static portMUX_TYPE           s_data_lock = portMUX_INITIALIZER_UNLOCKED;

/* ── Frame parser ─────────────────────────────────────────────────────── */

/** @brief Read a little-endian uint16 from a byte buffer. */
static inline uint16_t rd_u16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
/** @brief Read a little-endian int16 from a byte buffer. */
static inline int16_t  rd_s16(const uint8_t *p) { return (int16_t)rd_u16(p); }
/** @brief Read a little-endian uint32 from a byte buffer. */
static inline uint32_t rd_u32(const uint8_t *p) {
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * Parse a complete binary payload (93 bytes after STX, before ETX).
 * Returns true if the frame is valid and data was stored.
 */
static bool parse_binary_frame(const uint8_t *payload, int len)
{
    if (len != MD_PAYLOAD_SIZE) return false;
    if (payload[MD_OFF_TAG] != MD_BINARY_TAG) {
        ESP_LOGD(TAG, "Bad TAG: 0x%02X (expected 0x%02X)", payload[0], MD_BINARY_TAG);
        return false;
    }

    /* Decode all fields from the binary payload */
    int16_t  raw_rpm      = rd_s16(&payload[MD_OFF_RPM]);
    uint16_t raw_boost    = rd_u16(&payload[MD_OFF_BOOST]);
    uint8_t  raw_throttle = payload[MD_OFF_THROTTLE];
    uint16_t raw_lambda   = rd_u16(&payload[MD_OFF_LAMBDA]);
    uint16_t raw_lmm      = rd_u16(&payload[MD_OFF_LMM]);
    uint16_t raw_casetemp = rd_u16(&payload[MD_OFF_CASETEMP]);
    uint16_t raw_batvolt  = rd_u16(&payload[MD_OFF_BATVOLT]);
    int16_t  raw_vdop1    = rd_s16(&payload[MD_OFF_VDOPRES1]);
    int16_t  raw_vdop2    = rd_s16(&payload[MD_OFF_VDOPRES2]);
    int16_t  raw_vdop3    = rd_s16(&payload[MD_OFF_VDOPRES3]);
    int16_t  raw_vdot1    = rd_s16(&payload[MD_OFF_VDOTEMP1]);
    int16_t  raw_vdot2    = rd_s16(&payload[MD_OFF_VDOTEMP2]);
    int16_t  raw_vdot3    = rd_s16(&payload[MD_OFF_VDOTEMP3]);
    uint16_t raw_speed    = rd_u16(&payload[MD_OFF_SPEED]);
    uint8_t  raw_gear     = payload[MD_OFF_GEAR];
    uint8_t  raw_n75      = payload[MD_OFF_N75];
    uint16_t raw_reqboost = rd_u16(&payload[MD_OFF_REQBOOST]);
    uint16_t raw_efr      = rd_u16(&payload[MD_OFF_EFR]);
    uint16_t raw_knock    = rd_u16(&payload[MD_OFF_KNOCK]);

    /* Store parsed values (thread-safe) */
    portENTER_CRITICAL(&s_data_lock);
    s_latest_data.rpm       = (float)raw_rpm;
    s_latest_data.boost     = (float)raw_boost / 100.0f;
    s_latest_data.throttle  = (float)raw_throttle;
    s_latest_data.lambda    = (float)raw_lambda / 100.0f;
    s_latest_data.lmm       = (float)raw_lmm / 100.0f;
    s_latest_data.case_temp = (float)raw_casetemp / 100.0f;
    for (int i = 0; i < 8; i++) {
        s_latest_data.egt[i] = (float)rd_s16(&payload[MD_OFF_EGT + i * 2]);
    }
    s_latest_data.bat_volt  = (float)raw_batvolt / 100.0f;
    s_latest_data.vdo_pres1 = (float)raw_vdop1;
    s_latest_data.vdo_pres2 = (float)raw_vdop2;
    s_latest_data.vdo_pres3 = (float)raw_vdop3;
    s_latest_data.vdo_temp1 = (float)raw_vdot1;
    s_latest_data.vdo_temp2 = (float)raw_vdot2;
    s_latest_data.vdo_temp3 = (float)raw_vdot3;
    s_latest_data.speed     = (float)raw_speed / 100.0f;
    s_latest_data.gear      = raw_gear;
    s_latest_data.n75_duty  = raw_n75;
    s_latest_data.req_boost = (float)raw_reqboost / 100.0f;
    s_latest_data.efr_speed = (float)raw_efr;
    s_latest_data.knock     = (float)raw_knock;
    s_latest_data.frame_count++;
    portEXIT_CRITICAL(&s_data_lock);

#if OPENDASH_UART_DEBUG
    ESP_LOGI(TAG, "[MD] RPM=%d BOOST=%.2f THR=%d LAM=%.2f LMM=%.2f BAT=%.2fV"
                  " EGT[%d,%d,%d,%d,%d,%d,%d,%d]",
             raw_rpm, (float)raw_boost / 100.0f, raw_throttle,
             (float)raw_lambda / 100.0f, (float)raw_lmm / 100.0f,
             (float)raw_batvolt / 100.0f,
             rd_s16(&payload[MD_OFF_EGT + 0]),
             rd_s16(&payload[MD_OFF_EGT + 2]),
             rd_s16(&payload[MD_OFF_EGT + 4]),
             rd_s16(&payload[MD_OFF_EGT + 6]),
             rd_s16(&payload[MD_OFF_EGT + 8]),
             rd_s16(&payload[MD_OFF_EGT + 10]),
             rd_s16(&payload[MD_OFF_EGT + 12]),
             rd_s16(&payload[MD_OFF_EGT + 14]));
#endif

    return true;
}

/* ── HC-05 AT Command Layer ──────────────────────────────────────────── */
/* HC-05 in AT mode uses 38400 baud. Normal data mode uses 115200.
 * KEY pin HIGH = AT command mode, KEY pin LOW = data mode.
 *
 * Target Bluetooth names (multidisplay HC-06): "MD02", "rAtTrax", "multidisplay"
 *
 * AT command sequence for auto-connect:
 *   1. Pull KEY HIGH → enter AT mode (38400 baud)
 *   2. AT               → verify module responds "OK"
 *   3. AT+ROLE=1        → set master mode
 *   4. AT+CMODE=1       → connect to any address
 *   5. AT+INIT          → initialize SPP profile
 *   6. AT+INQ           → inquire/scan for nearby devices
 *   7. AT+RNAME?<addr>  → get remote name, match against targets
 *   8. AT+LINK=<addr>   → connect to matching device
 *   9. Pull KEY LOW → exit AT mode (115200 baud)
 *  10. Data starts flowing as transparent serial bridge
 */

#define HC05_AT_BAUD       38400
#define HC05_AT_TIMEOUT_MS 2000
#define HC05_INQ_TIMEOUT_S 10

/* Bluetooth target name — configurable in opendash_uart.h */
static const char *s_bt_target_name = OPENDASH_MD_BT_NAME;

/**
 * Send an AT command and wait for a response line.
 * Returns number of bytes read into resp_buf, or 0 on timeout.
 * Caller must hold UART at 38400 baud.
 */
static int hc05_send_at(const char *cmd, char *resp_buf, int resp_size, int timeout_ms)
{
    /* Flush RX buffer first */
    uart_flush_input(MD_UART_PORT);

    /* Send command with CR+LF */
    uart_write_bytes(MD_UART_PORT, cmd, strlen(cmd));
    uart_write_bytes(MD_UART_PORT, "\r\n", 2);

    /* Read response */
    int total = 0;
    int64_t start = esp_timer_get_time();
    while (total < resp_size - 1) {
        int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
        if (elapsed_ms > timeout_ms) break;

        uint8_t byte;
        int n = uart_read_bytes(MD_UART_PORT, &byte, 1, pdMS_TO_TICKS(50));
        if (n == 1) {
            resp_buf[total++] = (char)byte;
            /* Stop on newline — response complete */
            if (byte == '\n' && total >= 2) break;
        }
    }
    resp_buf[total] = '\0';
    return total;
}

/**
 * Check if HC-05 is in AT mode by sending "AT" and looking for "OK".
 */
static bool hc05_check_at_mode(void)
{
    char resp[64];
    int n = hc05_send_at("AT", resp, sizeof(resp), HC05_AT_TIMEOUT_MS);
    return (n > 0 && strstr(resp, "OK") != NULL);
}

/**
 * Enter AT mode: pull KEY HIGH, switch UART to 38400.
 * Returns true if AT mode confirmed.
 */
static bool hc05_enter_at_mode(void)
{
    int key_pin = OPENDASH_HC05_KEY_PIN;
    if (key_pin < 0) {
        ESP_LOGW(TAG, "HC-05 KEY pin not configured — cannot enter AT mode");
        return false;
    }

    /* Configure KEY pin as output and pull HIGH */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << key_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(key_pin, 1);
    vTaskDelay(pdMS_TO_TICKS(500));  /* Wait for HC-05 to enter AT mode */

    /* Switch UART to AT mode baud rate */
    uart_set_baudrate(MD_UART_PORT, HC05_AT_BAUD);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Verify AT mode */
    if (!hc05_check_at_mode()) {
        ESP_LOGW(TAG, "HC-05 not responding to AT after KEY HIGH");
        gpio_set_level(key_pin, 0);
        uart_set_baudrate(MD_UART_PORT, MD_UART_BAUD);
        return false;
    }

    ESP_LOGI(TAG, "HC-05 AT mode entered");
    return true;
}

/**
 * Exit AT mode: pull KEY LOW, switch UART back to 115200.
 */
static void hc05_exit_at_mode(void)
{
    int key_pin = OPENDASH_HC05_KEY_PIN;
    if (key_pin >= 0) {
        gpio_set_level(key_pin, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
    uart_set_baudrate(MD_UART_PORT, MD_UART_BAUD);
    ESP_LOGI(TAG, "HC-05 data mode (115200 baud)");
}

/**
 * Configure HC-05 as master and attempt to connect to multidisplay.
 * Must be called while in AT mode (38400 baud, KEY HIGH).
 *
 * Returns true if a connection was established.
 */
static bool hc05_configure_and_connect(void)
{
    char resp[256];

    /* Set master role */
    hc05_send_at("AT+ROLE=1", resp, sizeof(resp), HC05_AT_TIMEOUT_MS);
    if (!strstr(resp, "OK")) {
        ESP_LOGW(TAG, "AT+ROLE=1 failed: %s", resp);
        /* Continue anyway — might already be master */
    }

    /* Allow connection to any address */
    hc05_send_at("AT+CMODE=1", resp, sizeof(resp), HC05_AT_TIMEOUT_MS);

    /* Initialize SPP profile (may fail if already initialized) */
    hc05_send_at("AT+INIT", resp, sizeof(resp), HC05_AT_TIMEOUT_MS);

    /* Start inquiry — scan for nearby BT devices
     * AT+INQ returns lines like: +INQ:<addr>,<class>,<rssi>
     * Example: +INQ:1234:56:789ABC,1F00,FFC1 */
    ESP_LOGI(TAG, "Scanning for multidisplay Bluetooth...");

    /* Set inquiry mode: RSSI, max 10 devices, timeout in 1.28s units */
    hc05_send_at("AT+INQM=1,10,8", resp, sizeof(resp), HC05_AT_TIMEOUT_MS);

    /* Start inquiry */
    uart_flush_input(MD_UART_PORT);
    uart_write_bytes(MD_UART_PORT, "AT+INQ\r\n", 8);

    /* Collect inquiry results for up to 10 seconds */
    char inq_results[1024];
    int inq_len = 0;
    int64_t inq_start = esp_timer_get_time();

    while ((esp_timer_get_time() - inq_start) / 1000 < (HC05_INQ_TIMEOUT_S * 1000)) {
        uint8_t byte;
        int n = uart_read_bytes(MD_UART_PORT, &byte, 1, pdMS_TO_TICKS(100));
        if (n == 1 && inq_len < (int)sizeof(inq_results) - 1) {
            inq_results[inq_len++] = (char)byte;
        }
    }
    inq_results[inq_len] = '\0';

    /* Cancel inquiry */
    hc05_send_at("AT+INQC", resp, sizeof(resp), HC05_AT_TIMEOUT_MS);

    if (inq_len == 0) {
        ESP_LOGW(TAG, "No Bluetooth devices found");
        return false;
    }

    ESP_LOGI(TAG, "Inquiry results (%d bytes): %.100s...", inq_len, inq_results);

    /* Parse inquiry results — extract addresses and check names */
    char *line = inq_results;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = '\0';

        /* Look for +INQ: prefix */
        char *inq = strstr(line, "+INQ:");
        if (inq) {
            /* Extract address: format is XXXX:XX:XXXXXX */
            char *addr_start = inq + 5;
            char *addr_end = strchr(addr_start, ',');
            if (addr_end) {
                char addr[32];
                int addr_len = addr_end - addr_start;
                if (addr_len < (int)sizeof(addr)) {
                    memcpy(addr, addr_start, addr_len);
                    addr[addr_len] = '\0';

                    /* Query remote name */
                    char name_cmd[64];
                    snprintf(name_cmd, sizeof(name_cmd), "AT+RNAME?%s", addr);
                    hc05_send_at(name_cmd, resp, sizeof(resp), 5000);

                    ESP_LOGI(TAG, "Device %s name: %s", addr, resp);

                    /* Check if name matches target */
                    if (strstr(resp, s_bt_target_name)) {
                        ESP_LOGI(TAG, "Found target '%s' at %s — connecting...",
                                 s_bt_target_name, addr);

                        /* Attempt to connect */
                        char link_cmd[64];
                        snprintf(link_cmd, sizeof(link_cmd), "AT+LINK=%s", addr);
                        hc05_send_at(link_cmd, resp, sizeof(resp), 5000);

                        if (strstr(resp, "OK")) {
                            ESP_LOGI(TAG, "Connected to %s!", s_bt_target_name);
                            return true;
                        }
                        ESP_LOGW(TAG, "AT+LINK failed: %s", resp);
                    }
                }
            }
        }

        line = next;
    }

    ESP_LOGW(TAG, "No matching multidisplay device found");
    return false;
}

/**
 * Full HC-05 auto-connect sequence:
 *   Enter AT mode → configure → scan → connect → exit AT mode
 */
static bool hc05_auto_connect(void)
{
    if (OPENDASH_HC05_KEY_PIN < 0 || OPENDASH_UART_TX_PIN < 0) {
        ESP_LOGD(TAG, "HC-05 auto-connect skipped (KEY or TX pin not configured)");
        return false;
    }

    if (!hc05_enter_at_mode()) return false;

    bool connected = hc05_configure_and_connect();

    hc05_exit_at_mode();

    return connected;
}

/* ── UART receiver task ───────────────────────────────────────────────── */

/**
 * Continuously reads UART bytes, extracts frames between STX/ETX,
 * and parses each complete frame.
 */
/**
 * Binary frame receiver task.
 *
 * Multidisplay SERIALOUT_BINARY sends fixed 95-byte frames:
 *   [STX] [TAG=0x5F] [91 data bytes] [ETX]
 *
 * Because binary data can contain 0x02/0x03 values, we cannot rely on
 * simple STX/ETX scanning.  Instead:
 *   1. Scan for STX (0x02)
 *   2. Read next byte — must be TAG (0x5F)
 *   3. Bulk-read remaining 92 bytes (91 data + ETX)
 *   4. Verify last byte is ETX (0x03)
 *   5. Parse the 93-byte payload (TAG + 91 data + ETX excluded)
 *
 * If validation fails at any step, the candidate STX was just a data byte
 * and we continue scanning from the next byte.
 */
static void uart_rx_task(void *arg)
{
    uint8_t frame_buf[MD_FRAME_SIZE];   /* 95 bytes max */
    uint8_t rx_byte;

    /* ── HC-05 auto-connect on startup ────────────────────────────────── */
    if (OPENDASH_HC05_KEY_PIN >= 0 && OPENDASH_UART_TX_PIN >= 0) {
        ESP_LOGI(TAG, "Attempting HC-05 auto-connect to multidisplay...");
        bool bt_ok = hc05_auto_connect();
        if (bt_ok) {
            ESP_LOGI(TAG, "HC-05 Bluetooth connected — listening for data");
        } else {
            ESP_LOGW(TAG, "HC-05 auto-connect failed — will retry on timeout");
        }
    }

    /* ── Main receive loop ────────────────────────────────────────────── */
    uint32_t total_bytes = 0;
    uint32_t stx_count = 0;
    int64_t last_diag_us = esp_timer_get_time();

    while (1) {
        /* Periodic diagnostic every 5 seconds */
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_diag_us > 5000000) {
            ESP_LOGI(TAG, "UART diag: %lu bytes, %lu STX, status=%s",
                     (unsigned long)total_bytes, (unsigned long)stx_count,
                     opendash_uart_status_str());
            last_diag_us = now_us;
        }

        /* Step 1: Scan for STX byte */
        int n = uart_read_bytes(MD_UART_PORT, &rx_byte, 1, pdMS_TO_TICKS(100));
        if (n != 1) {
            /* No data — check for timeout */
            goto check_timeout;
        }
        total_bytes++;
        if (rx_byte != MD_STX) {
            continue;   /* Not STX, keep scanning */
        }
        stx_count++;

        /* Step 2: Read TAG byte (should be 0x5F) */
        n = uart_read_bytes(MD_UART_PORT, &rx_byte, 1, pdMS_TO_TICKS(50));
        if (n != 1 || rx_byte != MD_BINARY_TAG) {
            if (n == 1) {
                ESP_LOGW(TAG, "STX found but TAG=0x%02X (expected 0x5F)", rx_byte);
            }
            continue;   /* Not a valid frame start */
        }
        ESP_LOGD(TAG, "STX+TAG detected, reading %d more bytes...",
                 MD_FRAME_SIZE - 2);

        /* We have STX + TAG — bulk-read remaining 93 bytes (92 payload + ETX) */
        frame_buf[0] = MD_BINARY_TAG;
        int remaining = MD_FRAME_SIZE - 2;   /* 95 - 2 (STX+TAG consumed) = 93 */
        n = uart_read_bytes(MD_UART_PORT, &frame_buf[1], remaining,
                            pdMS_TO_TICKS(200));
        if (n != remaining) {
            ESP_LOGW(TAG, "Incomplete frame: got %d of %d bytes", n, remaining);
            continue;
        }

        /* Step 4: Verify ETX — sits right after the 93-byte payload
         * frame_buf[0..92] = payload (TAG + 92 data bytes)
         * frame_buf[93]    = ETX expected */
        if (frame_buf[MD_PAYLOAD_SIZE] != MD_ETX) {
            ESP_LOGW(TAG, "Bad ETX: 0x%02X at pos %d (got[0..3]: %02X %02X %02X %02X)",
                     frame_buf[MD_PAYLOAD_SIZE], MD_PAYLOAD_SIZE,
                     frame_buf[0], frame_buf[1], frame_buf[2], frame_buf[3]);
            continue;
        }

        /* Step 5: Parse the payload (93 bytes: TAG through byte before ETX) */
        if (parse_binary_frame(frame_buf, MD_PAYLOAD_SIZE)) {
            if (s_status != OPENDASH_UART_RECEIVING) {
#if OPENDASH_UART_DEBUG
                ESP_LOGI(TAG, "[STATUS] %s -> RECEIVING (frame #%lu)",
                         opendash_uart_status_str(),
                         (unsigned long)s_latest_data.frame_count);
#endif
                s_status = OPENDASH_UART_RECEIVING;
                s_connected_event = true;
                ESP_LOGI(TAG, "Multidisplay data stream active");
            }
            s_last_valid_frame_us = esp_timer_get_time();
        }
        continue;

check_timeout:
        /* Check for data timeout → attempt HC-05 reconnect */
        if (s_status == OPENDASH_UART_RECEIVING && s_last_valid_frame_us > 0) {
            int64_t elapsed_ms = (esp_timer_get_time() - s_last_valid_frame_us) / 1000;
            if (elapsed_ms > MD_DATA_TIMEOUT_MS) {
                s_status = OPENDASH_UART_TIMEOUT;
                ESP_LOGW(TAG, "Multidisplay data timeout");

                if (OPENDASH_HC05_KEY_PIN >= 0 && OPENDASH_UART_TX_PIN >= 0) {
                    ESP_LOGI(TAG, "Attempting HC-05 reconnect...");
                    vTaskDelay(pdMS_TO_TICKS(2000));
                    s_status = OPENDASH_UART_WAITING;
                    if (hc05_auto_connect()) {
                        ESP_LOGI(TAG, "HC-05 reconnected");
                    }
                }
            }
        }
    }
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool opendash_uart_init(void)
{
#if !OPENDASH_MULTIDISPLAY_CONNECTION
    ESP_LOGI(TAG, "Multidisplay UART disabled at compile time");
    return true;
#else
    int tx_pin = OPENDASH_UART_TX_PIN;
    int rx_pin = OPENDASH_UART_RX_PIN;

    /* ── Release USB PHY pads if RX pin is on GPIO19 or GPIO20 ─────────
     * GPIO19/20 are the ESP32-S3 internal USB D-/D+ pins.  The USB-Serial/
     * JTAG controller claims them at boot even when console uses UART0.
     * We disable the USB PHY pad to release the GPIO for UART use.
     * Flashing still works — the ROM bootloader re-enables USB.          */
    if (rx_pin == 19 || rx_pin == 20 || tx_pin == 19 || tx_pin == 20) {
        ESP_LOGW(TAG, "Releasing USB-Serial/JTAG PHY pad to free GPIO19/20 for UART");
        usb_serial_jtag_ll_phy_enable_pad(false);
        vTaskDelay(pdMS_TO_TICKS(10));  /* let PHY release settle */
    }

    /* ── GPIO activity diagnostic ─────────────────────────────────────
     * Sample the RX pin as raw GPIO before UART takes over, to verify
     * that the HC-05 TX signal is physically connected.  An idle UART
     * line = HIGH (all samples). Active data = mix of HIGH and LOW.    */
    if (rx_pin >= 0) {
        gpio_reset_pin(rx_pin);
        gpio_set_direction(rx_pin, GPIO_MODE_INPUT);
        gpio_pullup_en(rx_pin);           /* idle UART = HIGH */
        vTaskDelay(pdMS_TO_TICKS(10));     /* settle */
        int high_count = 0;
        for (int i = 0; i < 2000; i++) {
            if (gpio_get_level(rx_pin)) high_count++;
            esp_rom_delay_us(50);          /* sample 2000 × 50µs = 100ms */
        }
        ESP_LOGI(TAG, "GPIO%d activity probe: %d/2000 HIGH"
                      " (2000=idle/nodata, <2000=active)", rx_pin, high_count);
    }

    uart_config_t cfg = {
        .baud_rate  = MD_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_param_config(MD_UART_PORT, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_set_pin(MD_UART_PORT,
                       tx_pin >= 0 ? tx_pin : UART_PIN_NO_CHANGE,
                       rx_pin >= 0 ? rx_pin : UART_PIN_NO_CHANGE,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        return false;
    }

    err = uart_driver_install(MD_UART_PORT, MD_UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return false;
    }

    s_status = OPENDASH_UART_WAITING;

    xTaskCreate(uart_rx_task, "md_uart_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Multidisplay UART ready (RX=GPIO%d, TX=%s, KEY=%s, %d baud)",
             rx_pin,
             tx_pin >= 0 ? "GPIO" : "N/C",
             OPENDASH_HC05_KEY_PIN >= 0 ? "GPIO" : "N/C",
             MD_UART_BAUD);
    if (tx_pin >= 0) ESP_LOGI(TAG, "  TX=GPIO%d", tx_pin);
    if (OPENDASH_HC05_KEY_PIN >= 0) ESP_LOGI(TAG, "  KEY=GPIO%d", OPENDASH_HC05_KEY_PIN);
    return true;
#endif
}

opendash_uart_status_t opendash_uart_get_status(void)
{
    return s_status;
}

const char *opendash_uart_status_str(void)
{
    switch (s_status) {
        case OPENDASH_UART_DISABLED:  return "MD: Disabled";
        case OPENDASH_UART_WAITING:   return "MD: Waiting [" OPENDASH_MD_BT_NAME "]...";
        case OPENDASH_UART_RECEIVING: return "MD: " OPENDASH_MD_BT_NAME " Connected";
        case OPENDASH_UART_TIMEOUT:   return "MD: " OPENDASH_MD_BT_NAME " No Data";
        default:                      return "MD: ???";
    }
}

bool opendash_uart_connected_event(void)
{
    if (s_connected_event) {
        s_connected_event = false;
        return true;
    }
    return false;
}

bool opendash_uart_get_data(opendash_md_data_t *out)
{
    portENTER_CRITICAL(&s_data_lock);
    *out = s_latest_data;
    portEXIT_CRITICAL(&s_data_lock);
    return out->frame_count > 0;
}