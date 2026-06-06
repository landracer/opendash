/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_rtc.c
 * @brief PCF85063 RTC driver — new I2C master API only
 *
 * New API (driver/i2c_master.h): used by all boards (left, right, GPS)
 *
 * PCF85063 register map (relevant subset):
 *   0x00  Control_1      — oscillator, correction, 12/24h, interrupt
 *   0x01  Control_2      — alarm, timer
 *   0x02  Offset         — frequency offset
 *   0x03  RAM_byte       — 1 byte general-purpose RAM
 *   0x04  Seconds        — BCD, bit 7 = OS (oscillator stopped = invalid)
 *   0x05  Minutes        — BCD
 *   0x06  Hours          — BCD (24h mode)
 *   0x07  Days           — BCD
 *   0x08  Weekdays       — 0=Sunday
 *   0x09  Months         — BCD
 *   0x0A  Years          — BCD (0–99, century base = 2000)
 */

#include "opendash_rtc.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include <sys/time.h>
#include <string.h>

static const char *TAG = "rtc_pcf85063";

#define PCF85063_ADDR       0x51
#define PCF85063_REG_CTRL1  0x00
#define PCF85063_REG_SEC    0x04
#define PCF85063_REG_MIN    0x05
#define PCF85063_REG_HOUR   0x06
#define PCF85063_REG_DAY    0x07
#define PCF85063_REG_WDAY   0x08
#define PCF85063_REG_MON    0x09
#define PCF85063_REG_YEAR   0x0A

#define I2C_TIMEOUT_MS      100

static i2c_master_dev_handle_t s_dev_handle = NULL;
static bool      s_available = false;

/* BCD helpers */
static inline uint8_t bcd_to_dec(uint8_t bcd) { return (bcd >> 4) * 10 + (bcd & 0x0F); }
static inline uint8_t dec_to_bcd(uint8_t dec) { return ((dec / 10) << 4) | (dec % 10); }

/* ── Low-level I2C: new master API ──────────────────────────────────────── */

static esp_err_t rtc_write_reg(uint8_t reg, const uint8_t *data, size_t len)
{
    uint8_t buf[1 + 7];  /* reg + up to 7 data bytes */
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(s_dev_handle, buf, 1 + len, I2C_TIMEOUT_MS);
}

static esp_err_t rtc_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, len,
                                        I2C_TIMEOUT_MS);
}

/* ── Public API ─────────────────────────────────────────────────────────── */

esp_err_t opendash_rtc_init_master(i2c_master_bus_handle_t bus_handle)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = PCF85063_ADDR,
        .scl_speed_hz    = 400000,
    };

    esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 not found on I2C bus (0x%02X): %s",
                 PCF85063_ADDR, esp_err_to_name(ret));
        s_available = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* Probe: read Control_1 register */
    uint8_t ctrl1 = 0;
    ret = rtc_read_reg(PCF85063_REG_CTRL1, &ctrl1, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 probe failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(s_dev_handle);
        s_dev_handle = NULL;
        s_available = false;
        return ESP_ERR_NOT_FOUND;
    }

    /* Ensure 24-hour mode */
    ctrl1 &= ~(1 << 5);
    uint8_t new_ctrl1 = ctrl1;
    ret = rtc_write_reg(PCF85063_REG_CTRL1, &new_ctrl1, 1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure PCF85063");
        return ret;
    }

    s_available = true;
    ESP_LOGI(TAG, "PCF85063 RTC initialized (new master API)");
    return ESP_OK;
}

esp_err_t opendash_rtc_get_time(struct tm *tm)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;

    uint8_t regs[7];  /* seconds through years */
    esp_err_t ret = rtc_read_reg(PCF85063_REG_SEC, regs, 7);
    if (ret != ESP_OK) return ret;

    /* Check OS bit (oscillator stopped = time invalid) */
    if (regs[0] & 0x80) {
        ESP_LOGW(TAG, "RTC oscillator stopped — time invalid");
        return ESP_ERR_INVALID_STATE;
    }

    tm->tm_sec  = bcd_to_dec(regs[0] & 0x7F);
    tm->tm_min  = bcd_to_dec(regs[1] & 0x7F);
    tm->tm_hour = bcd_to_dec(regs[2] & 0x3F);
    tm->tm_mday = bcd_to_dec(regs[3] & 0x3F);
    tm->tm_wday = regs[4] & 0x07;
    tm->tm_mon  = bcd_to_dec(regs[5] & 0x1F) - 1;  /* struct tm: 0-11 */
    tm->tm_year = bcd_to_dec(regs[6]) + 100;        /* struct tm: years since 1900, RTC base 2000 */
    tm->tm_isdst = -1;

    ESP_LOGD(TAG, "RTC read: %04d-%02d-%02d %02d:%02d:%02d",
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    return ESP_OK;
}

esp_err_t opendash_rtc_set_time(const struct tm *tm)
{
    if (!s_available) return ESP_ERR_INVALID_STATE;

    uint8_t regs[7];
    regs[0] = dec_to_bcd(tm->tm_sec) & 0x7F;   /* clear OS bit */
    regs[1] = dec_to_bcd(tm->tm_min);
    regs[2] = dec_to_bcd(tm->tm_hour);
    regs[3] = dec_to_bcd(tm->tm_mday);
    regs[4] = tm->tm_wday & 0x07;
    regs[5] = dec_to_bcd(tm->tm_mon + 1);
    regs[6] = dec_to_bcd(tm->tm_year - 100);    /* 2026 → 126 - 100 = 26 */

    esp_err_t ret = rtc_write_reg(PCF85063_REG_SEC, regs, 7);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "RTC set: %04d-%02d-%02d %02d:%02d:%02d",
                 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    return ret;
}

esp_err_t opendash_rtc_sync_system_clock(void)
{
    struct tm tm;
    esp_err_t ret = opendash_rtc_get_time(&tm);
    if (ret != ESP_OK) return ret;

    /* Sanity check: year must be ≥ 2024 */
    if (tm.tm_year + 1900 < 2024) {
        ESP_LOGW(TAG, "RTC year %d implausible — skipping system clock sync",
                 tm.tm_year + 1900);
        return ESP_ERR_INVALID_STATE;
    }

    time_t t = mktime(&tm);
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    ESP_LOGI(TAG, "System clock synced from RTC: %04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    return ESP_OK;
}

bool opendash_rtc_is_available(void)
{
    return s_available;
}
