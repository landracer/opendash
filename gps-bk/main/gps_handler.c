/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file gps_handler.c
 * @brief OpenDash GPS Handler — LC76G via I2C (CASIC protocol)
 *
 * Interfaces with the LC76G GNSS module over I2C to read NMEA sentences
 * and parse position, speed, heading, and satellite data.
 *
 * LC76G I2C Configuration (Waveshare ESP32-S3-Touch-AMOLED-1.75):
 *   I2C bus: GPIO15 (SDA) / GPIO14 (SCL) — shared with touch, IMU, etc.
 *   Write address: 0x50 (7-bit) — for sending CASIC commands
 *   Read address:  0x54 (7-bit) — for receiving NMEA data
 *   Clock: 100 kHz
 *
 * CASIC I2C Protocol (Quectel I2C Application Note v1.0):
 *   Uses SEPARATE I2C slave addresses — NOT repeated-start:
 *     0x50 = command/query write (CR_CMD / CW_CMD)
 *     0x54 = data read (NMEA / length responses)
 *     0x58 = data write (NMEA commands to module)
 *
 *   Read sequence (separate tx + rx transactions):
 *   1. Query data length:
 *      Transmit to 0x50: {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00}
 *        → uint32_t[2] LE: [(CR_CMD<<16)|TX_LEN_OFFSET, 4]
 *      Receive from 0x54: 4 bytes → little-endian uint32 available byte count
 *   2. Request NMEA data:
 *      Transmit to 0x50: {0x00, 0x20, 0x51, 0xAA, <4 bytes length>}
 *        → uint32_t[2] LE: [(CR_CMD<<16)|TX_BUF_OFFSET, length]
 *      Receive from 0x54: N bytes → raw NMEA ASCII
 *
 *   Write sequence:
 *   1. Query RX buffer free space:
 *      Transmit to 0x50: {0x04, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00}
 *      Receive from 0x54: 4 bytes → free space
 *   2. Config write:
 *      Transmit to 0x50: {0x00, 0x10, 0x53, 0xAA, <4 bytes length>}
 *   3. Write data:
 *      Transmit to 0x58: actual NMEA command bytes
 *
 * Supported NMEA sentences:
 *   $GPRMC / $GNRMC — Position, speed, heading, date/time
 *   $GPGGA / $GNGGA — Fix quality, altitude, satellites, HDOP
 *
 * Version: v15L2 (stable base version)
 *   - Extended power-off (5s) and boot wait (5s) for clean LC76G resets
 *   - "Primer" mechanism: TxRx + data_req (CR_CMD offset 0x2000) activates
 *     the module's I2C TX buffer fill — required at boot AND recovery
 *   - CW+0x58 WAKE after every data read for sustained data flow
 *   - Re-prime (data_req + drain read) at TxRx activation maintains output
 *   - 50ms bus_reset delay = optimal for shared bus (tested 20/50/100ms)
 *   - Self-healing recovery: PMIC power cycle → WAKE → primer → probes
 *   - Verified: 55-95K bytes/5min, 100-125s continuous, 3D FIX 9-16 sats
 *   - Position: 39.664xxx, -105.010xxx, Alt: 1621-1635m, HDOP 0.7-1.1
 *
 * @see Waveshare ESP32-S3-LC76G-I2C example
 */

/* ═══════════════════════════════════════════════════════════════════
 * CROSS-REFERENCE INDEX — wiki/LC76G-I2C-GPS-Driver-Guide.md v2.0.0
 * ═══════════════════════════════════════════════════════════════════
 * §2.3  I2C Address Map      → LC76G_I2C_ADDR_* defines
 * §2.4  PMIC Power Rails     → gps_ensure_power()
 * §2.5  GPIO Expander        → TCA9554 FORCE_ON/NRESET in gps_ensure_power()
 * §5.1  PMIC Power Cycle     → gps_ensure_power()
 * §5.2  I2C WAKE Mechanism   → WAKE sequences in warm-up, main loop, recovery
 * §5.3  Activation TxRx      → Tier 1 activation in RX fail path
 * §5.4  Primer Mechanism     → Boot warm-up primer, recovery primer
 * §5.5  Per-Read WAKE        → Post-data CW+0x58 after successful read
 * §5.6  Re-Prime + Drain     → data_req + drain in Tier 1 activation
 * §6    Step-by-Step         → gps_handler_init(), gps_task()
 * §7    NMEA Parsing         → process_nmea_line(), parse_gga(), parse_rmc()
 * §13   Reproduction Checklist → Full implementation verification
 * ═══════════════════════════════════════════════════════════════════
 */

#include "gps_handler.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "display_init.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "gps_handler";

/* LC76G I2C addresses (7-bit) — Waveshare ESP32-S3-Touch-AMOLED-1.75
 * The LC76G uses CASIC protocol which has SEPARATE addresses for
 * write (commands) and read (NMEA data):
 *   0x50 = write endpoint (CASIC commands)
 *   0x54 = read endpoint (NMEA responses)
 * Using the wrong address for reads returns 0 bytes every time. */
#define LC76G_I2C_ADDR_WRITE    0x50    /* CASIC command address */
#define LC76G_I2C_ADDR_READ     0x54    /* CASIC data read address */
#define LC76G_I2C_ADDR_DATA_WR  0x58    /* CASIC data write address */
#define LC76G_I2C_ADDR          LC76G_I2C_ADDR_WRITE  /* For bus scan/probe */
#define LC76G_I2C_CLK_HZ       100000   /* 100 kHz — matches Waveshare example */

/* Maximum NMEA buffer size — large enough to drain the LC76G buffer quickly */
#define GPS_NMEA_BUF_SIZE       4096
#define GPS_STATUS_LOG_CYCLES   5       /* Log status every ~5 seconds (1000ms × 5) */

/* GPS state */
static gps_data_t current_gps_data = {0};
static gps_debug_t current_gps_debug = {0};
static SemaphoreHandle_t gps_mutex = NULL;
static TaskHandle_t gps_task_handle = NULL;
static i2c_master_dev_handle_t lc76g_handle = NULL;       /* Write endpoint (0x50) */
static i2c_master_dev_handle_t lc76g_read_handle = NULL;  /* Read endpoint (0x54) */
static i2c_master_dev_handle_t lc76g_dwr_handle = NULL;   /* Data write endpoint (0x58) */

/* NMEA line parsing buffer */
static char nmea_line[256];
static int nmea_line_pos = 0;

/* ──────────────────────────────────────────────────────────────────────────
 * NMEA Parsing Helpers
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Convert NMEA latitude/longitude (DDmm.mmmm) to decimal degrees.
 */
static double nmea_to_degrees(const char *val, const char *dir)
{
    if (val == NULL || val[0] == '\0') return 0.0;

    double raw = atof(val);
    int degrees = (int)(raw / 100.0);
    double minutes = raw - (degrees * 100.0);
    double result = degrees + (minutes / 60.0);

    if (dir && (dir[0] == 'S' || dir[0] == 'W')) {
        result = -result;
    }
    return result;
}

/**
 * @brief Get the nth comma-separated field from an NMEA sentence.
 * @param sentence  Full NMEA sentence string.
 * @param field     Field index (0-based, 0 = sentence type e.g. "$GPRMC").
 * @param out       Output buffer.
 * @param out_len   Output buffer size.
 * @return true if field was found.
 */
static bool nmea_get_field(const char *sentence, int field, char *out, size_t out_len)
{
    int current_field = 0;
    const char *p = sentence;

    while (*p && current_field < field) {
        if (*p == ',') current_field++;
        p++;
    }
    if (current_field != field) {
        out[0] = '\0';
        return false;
    }

    size_t i = 0;
    while (*p && *p != ',' && *p != '*' && *p != '\r' && *p != '\n' && i < out_len - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0;
}

/* ──────────────────────────────────────────────────────────────────────────
 * NMEA Command Writing via CASIC I2C Protocol
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Build a complete NMEA command with checksum and CR/LF.
 * @param body  The command body without '$', '*', or CR/LF (e.g., "PQTMCOLD")
 * @param out   Output buffer (must be large enough)
 * @param out_sz Output buffer size
 * @return Length of the complete command string, or 0 on error
 */
static size_t nmea_build_command(const char *body, char *out, size_t out_sz)
{
    uint8_t cksum = 0;
    for (const char *p = body; *p; p++) {
        cksum ^= (uint8_t)*p;
    }
    int len = snprintf(out, out_sz, "$%s*%02X\r\n", body, cksum);
    if (len < 0 || (size_t)len >= out_sz) return 0;
    return (size_t)len;
}

/**
 * @brief Send an NMEA command to the LC76G via CASIC I2C write protocol.
 *
 * CASIC Write Protocol (from Quectel I2C Application Note):
 *   1. Query RX buffer free space:
 *      Write {0x04,0x00,0x51,0xAA, 0x04,0x00,0x00,0x00} to 0x50
 *      Read 4 bytes from 0x54 → free space (little-endian uint32)
 *   2. Config write:
 *      Write {0x00,0x10,0x53,0xAA, len[0..3]} to 0x50
 *   3. Write data:
 *      Write actual command bytes to 0x58
 *
 * @param cmd_body  NMEA command body (e.g., "PQTMCOLD")
 * @return ESP_OK on success
 */
static esp_err_t lc76g_send_command(const char *cmd_body)
{
    if (!lc76g_handle || !lc76g_read_handle || !lc76g_dwr_handle) {
        ESP_LOGE(TAG, "send_command: device handles not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    /* Build complete NMEA command with checksum */
    char cmd_buf[128];
    size_t cmd_len = nmea_build_command(cmd_body, cmd_buf, sizeof(cmd_buf));
    if (cmd_len == 0) {
        ESP_LOGE(TAG, "send_command: failed to build command for '%s'", cmd_body);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Sending command to LC76G: %.*s", (int)(cmd_len - 2), cmd_buf);  /* Skip CR/LF */

    /* Step 1: Query RX buffer free space — separate tx(0x50) + rx(0x54)
     * Per Quectel app note: I2C_Master_Transmit to 0x50, I2C_Master_Receive from 0x54.
     * The 8-byte query is a uint32_t[2] on little-endian ARM:
     *   cmd[0] = (CW_CMD << 16) | RX_LEN_OFFSET = 0xAA510004 → {0x04, 0x00, 0x51, 0xAA}
     *   cmd[1] = 4 → {0x04, 0x00, 0x00, 0x00} */
    uint8_t query[] = {0x04, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
    uint8_t free_buf[4] = {0};
    esp_err_t ret = i2c_master_transmit(lc76g_handle, query, sizeof(query), 500);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_command: query tx failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));  /* 100ms — matching Waveshare demo timing */
    ret = i2c_master_receive(lc76g_read_handle, free_buf, sizeof(free_buf), 500);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_command: query rx failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint32_t free_space = (uint32_t)free_buf[0] | ((uint32_t)free_buf[1] << 8)
                        | ((uint32_t)free_buf[2] << 16) | ((uint32_t)free_buf[3] << 24);
    ESP_LOGI(TAG, "  LC76G RX buffer free: %lu bytes (need %u)", (unsigned long)free_space, (unsigned)cmd_len);

    if (free_space < cmd_len && free_space != 0) {
        ESP_LOGW(TAG, "  RX buffer too full (free=%lu, need=%u)", (unsigned long)free_space, (unsigned)cmd_len);
        return ESP_ERR_NO_MEM;
    }

    /* Step 2: Config write command — tell LC76G we're about to write cmd_len bytes */
    uint8_t config_cmd[8];
    config_cmd[0] = 0x00;  /* cmd_id low */
    config_cmd[1] = 0x10;  /* cmd_id high */
    config_cmd[2] = 0x53;  /* tag low */
    config_cmd[3] = 0xAA;  /* tag high */
    config_cmd[4] = (uint8_t)(cmd_len & 0xFF);
    config_cmd[5] = (uint8_t)((cmd_len >> 8) & 0xFF);
    config_cmd[6] = (uint8_t)((cmd_len >> 16) & 0xFF);
    config_cmd[7] = (uint8_t)((cmd_len >> 24) & 0xFF);

    ret = i2c_master_transmit(lc76g_handle, config_cmd, sizeof(config_cmd), 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_command: config tx failed: %s", esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(100));  /* 100ms — matching Waveshare demo timing */

    /* Step 3: Write actual data to 0x58 */
    ret = i2c_master_transmit(lc76g_dwr_handle, (const uint8_t *)cmd_buf, cmd_len, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "send_command: data write to 0x58 failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "  Command sent OK (%u bytes to 0x58)", (unsigned)cmd_len);
    current_gps_debug.cmds_sent++;
    current_gps_debug.cmds_ok++;
    return ESP_OK;
}

/**
 * @brief Parse $GPRMC sentence (or $GNRMC).
 *
 * Fields: $GPRMC,time,status,lat,N/S,lon,E/W,speed_knots,heading,date,...
 */
static void parse_rmc(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Field 1: UTC time (hhmmss.ss) */
    if (nmea_get_field(sentence, 1, field, sizeof(field)) && strlen(field) >= 6) {
        data->hour   = (field[0] - '0') * 10 + (field[1] - '0');
        data->minute = (field[2] - '0') * 10 + (field[3] - '0');
        data->second = (field[4] - '0') * 10 + (field[5] - '0');
    }

    /* Field 2: Status (A=active/valid, V=void) */
    if (nmea_get_field(sentence, 2, field, sizeof(field))) {
        data->fix_valid = (field[0] == 'A');
    }

    /* Fields 3-4: Latitude + N/S */
    char lat_val[20], lat_dir[4];
    nmea_get_field(sentence, 3, lat_val, sizeof(lat_val));
    nmea_get_field(sentence, 4, lat_dir, sizeof(lat_dir));
    if (lat_val[0]) {
        data->latitude = nmea_to_degrees(lat_val, lat_dir);
    }

    /* Fields 5-6: Longitude + E/W */
    char lon_val[20], lon_dir[4];
    nmea_get_field(sentence, 5, lon_val, sizeof(lon_val));
    nmea_get_field(sentence, 6, lon_dir, sizeof(lon_dir));
    if (lon_val[0]) {
        data->longitude = nmea_to_degrees(lon_val, lon_dir);
    }

    /* Field 7: Speed over ground (knots → km/h) */
    if (nmea_get_field(sentence, 7, field, sizeof(field)) && field[0]) {
        data->speed = atof(field) * 1.852f;  /* knots to km/h */
    }

    /* Field 8: Track angle / heading (degrees true) */
    if (nmea_get_field(sentence, 8, field, sizeof(field)) && field[0]) {
        data->heading = atof(field);
    }

    /* Field 9: Date (ddmmyy) */
    if (nmea_get_field(sentence, 9, field, sizeof(field)) && strlen(field) >= 6) {
        data->day   = (field[0] - '0') * 10 + (field[1] - '0');
        data->month = (field[2] - '0') * 10 + (field[3] - '0');
        data->year  = 2000 + (field[4] - '0') * 10 + (field[5] - '0');
    }
}

/**
 * @brief Parse $GPGGA sentence (or $GNGGA).
 *
 * Fields: $GPGGA,time,lat,N/S,lon,E/W,quality,sats,hdop,alt,M,...
 */
static void parse_gga(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Field 6: Fix quality (0=invalid, 1=GPS, 2=DGPS, 6=estimated) */
    if (nmea_get_field(sentence, 6, field, sizeof(field)) && field[0]) {
        data->fix_quality = atoi(field);
        if (data->fix_quality > 0) {
            data->fix_valid = true;
        }
    }

    /* Field 7: Satellites used */
    if (nmea_get_field(sentence, 7, field, sizeof(field)) && field[0]) {
        data->satellites = atoi(field);
    }

    /* Field 8: HDOP */
    if (nmea_get_field(sentence, 8, field, sizeof(field)) && field[0]) {
        data->hdop = atof(field);
        /* Rough accuracy estimate: HDOP * 5 meters (typical GPS) */
        data->accuracy = data->hdop * 5.0f;
    }

    /* Field 9: Altitude above MSL */
    if (nmea_get_field(sentence, 9, field, sizeof(field)) && field[0]) {
        data->altitude = atof(field);
    }
}

/**
 * @brief Parse $GPGSV / $GNGSV / $GLGSV / $GAGSV sentence for visible satellite count.
 *
 * GSV format: $xxGSV,totalMsgs,msgNum,totalSats,PRN,elev,azim,SNR,...*cs
 *   Talker prefix determines constellation:
 *     $GP = GPS,  $GL = GLONASS,  $GA = Galileo,  $GB = BeiDou
 *   Field 3 = total visible satellites for THIS constellation.
 *   Only read from message 1 of N (field 2 == "1").
 */
static void parse_gsv(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Only process message 1 of the GSV sequence (has the total count) */
    char msgnum[8];
    if (!nmea_get_field(sentence, 2, msgnum, sizeof(msgnum)) || msgnum[0] != '1') {
        return;
    }

    /* Field 3: total satellites in view */
    if (!nmea_get_field(sentence, 3, field, sizeof(field)) || !field[0]) {
        return;
    }
    int sats = atoi(field);

    /* Route to correct constellation counter based on talker prefix.
     * sentence[0] = '$', sentence[1..2] = talker (GP/GL/GA/GB/GN) */
    if (strncmp(sentence, "$GP", 3) == 0) {
        data->visible_gps = sats;
        data->visible_sats = sats;  /* Reset total with GPS (first in cycle) */
    } else if (strncmp(sentence, "$GL", 3) == 0) {
        data->visible_glo = sats;
        data->visible_sats += sats;
    } else if (strncmp(sentence, "$GA", 3) == 0) {
        data->visible_gal = sats;
        data->visible_sats += sats;
    } else if (strncmp(sentence, "$GB", 3) == 0) {
        data->visible_bds = sats;
        data->visible_sats += sats;
    }
}

/**
 * @brief Parse $GNGSA / $GPGSA — DOP values and fix mode.
 *
 * GSA format: $xxGSA,mode,fix,sv1..sv12,pdop,hdop,vdop*cs
 *   Field 2:  fix type (1=no fix, 2=2D, 3=3D)
 *   Field 15: PDOP
 *   Field 16: HDOP (may refine GGA's value)
 *   Field 17: VDOP (before checksum)
 */
static void parse_gsa(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Field 2: Fix mode */
    if (nmea_get_field(sentence, 2, field, sizeof(field)) && field[0]) {
        data->fix_3d = (atoi(field) == 3);
    }

    /* Field 15: PDOP */
    if (nmea_get_field(sentence, 15, field, sizeof(field)) && field[0]) {
        data->pdop = atof(field);
    }

    /* Field 16: HDOP (may refine GGA's value) */
    if (nmea_get_field(sentence, 16, field, sizeof(field)) && field[0]) {
        float gsa_hdop = atof(field);
        if (gsa_hdop > 0.0f) {
            data->hdop = gsa_hdop;
            data->accuracy = gsa_hdop * 5.0f;
        }
    }

    /* Field 17: VDOP (atof stops at '*' naturally) */
    if (nmea_get_field(sentence, 17, field, sizeof(field)) && field[0]) {
        data->vdop = atof(field);
    }
}

/**
 * @brief Parse $GNVTG / $GPVTG — Track and ground speed.
 *
 * VTG format: $xxVTG,cog_true,T,cog_mag,M,sog_knots,N,sog_kmh,K,mode*cs
 *   Field 7:  Speed over ground in km/h (direct, no conversion)
 *   Field 1:  Course over ground, true (heading backup)
 */
static void parse_vtg(const char *sentence, gps_data_t *data)
{
    char field[32];

    /* Field 7: Speed in km/h (direct, no conversion needed) */
    if (nmea_get_field(sentence, 7, field, sizeof(field)) && field[0]) {
        float vtg_speed = atof(field);
        if (vtg_speed > 0.0f) {
            data->speed = vtg_speed;  /* VTG km/h is more direct than RMC knots */
        }
    }

    /* Field 1: Course over ground, true north (heading backup) */
    if (nmea_get_field(sentence, 1, field, sizeof(field)) && field[0]) {
        data->heading = atof(field);
    }
}

/* See wiki §7 — NMEA Parsing */
/**
 * @brief Process a complete NMEA line — parse + count sentence types.
 */
static void process_nmea_line(const char *line, gps_data_t *data)
{
    if (line[0] != '$') return;

    /* Match sentence types (supports GP, GN, GL, GA, GB prefixes) */
    const char *type = line + 3;  /* Skip "$GP", "$GN", etc. */

    if (strncmp(type, "RMC", 3) == 0) {
        parse_rmc(line, data);
        current_gps_debug.cnt_rmc++;
        /* Store raw RMC for debug display */
        strncpy(current_gps_debug.last_rmc, line, sizeof(current_gps_debug.last_rmc) - 1);
        current_gps_debug.last_rmc[sizeof(current_gps_debug.last_rmc) - 1] = '\0';
    } else if (strncmp(type, "GGA", 3) == 0) {
        parse_gga(line, data);
        current_gps_debug.cnt_gga++;
        /* Store raw GGA for debug display */
        strncpy(current_gps_debug.last_gga, line, sizeof(current_gps_debug.last_gga) - 1);
        current_gps_debug.last_gga[sizeof(current_gps_debug.last_gga) - 1] = '\0';
        /* Log raw GGA sentence for first 60 cycles (diagnostics) */
        if (current_gps_debug.cycle < 60) {
            ESP_LOGI(TAG, "  [GGA] %s", line);
        }
    } else if (strncmp(type, "GSV", 3) == 0) {
        parse_gsv(line, data);
        current_gps_debug.cnt_gsv++;
        /* Log raw GSV for first 10 cycles to diagnose satellite visibility */
        if (current_gps_debug.cycle < 10) {
            ESP_LOGI(TAG, "  [GSV] %s", line);
        }
    } else if (strncmp(type, "GSA", 3) == 0) {
        parse_gsa(line, data);
        current_gps_debug.cnt_gsa++;
    } else if (strncmp(type, "GLL", 3) == 0) {
        current_gps_debug.cnt_gll++;     /* skip — redundant with RMC */
    } else if (strncmp(type, "VTG", 3) == 0) {
        parse_vtg(line, data);
        current_gps_debug.cnt_vtg++;
    } else if (strncmp(line + 1, "PQTM", 4) == 0) {
        current_gps_debug.cnt_pqtm++;
        /* Always log PQTM responses */
        ESP_LOGI(TAG, "  [PQTM] %s", line);
    } else if (strncmp(line + 1, "PAIR", 4) == 0) {
        current_gps_debug.cnt_pqtm++;  /* reuse counter for proprietary */
        ESP_LOGI(TAG, "  [PAIR ACK] %s", line);
        /* Parse result: field 2 = "0" means success */
        char result[4];
        if (nmea_get_field(line, 2, result, sizeof(result)) && result[0] == '0') {
            current_gps_debug.cmds_ok++;
        }
    } else if (strncmp(type - 1, "TXT", 3) == 0 || strncmp(type, "TXT", 3) == 0) {
        current_gps_debug.cnt_txt++;
        /* Log TXT messages (module status info) */
        ESP_LOGI(TAG, "  [TXT] %s", line);
    } else {
        current_gps_debug.cnt_other++;
        /* Log unknown sentence types for first 30 cycles */
        if (current_gps_debug.cycle < 30) {
            ESP_LOGI(TAG, "  [???] %s", line);
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * AXP2101 PMIC Diagnostic — Read and log power rail status
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Read a single AXP2101 register via I2C.
 * Uses transmit_receive on a temporary device handle at address 0x34.
 */
static esp_err_t axp2101_read_reg(i2c_master_dev_handle_t pmu, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(pmu, &reg, 1, val, 1, 100);
}


/**
 * @brief Write a single AXP2101 register via I2C.
 */
static esp_err_t axp2101_write_reg(i2c_master_dev_handle_t pmu, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(pmu, buf, 2, 100);
}

/**
 * @brief Write a single TCA9554 register via I2C.
 */
static esp_err_t tca9554_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, 2, 100);
}

/**
 * @brief Read a single TCA9554 register via I2C.
 */
static __attribute__((unused)) esp_err_t tca9554_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(dev, &reg, 1, val, 1, 100);
}

/* factory_reset_done: reserved for future use */

/* See wiki §5.1 — PMIC Power Cycle */
/**
 * @brief Full GPS power cycle via PMIC + TCA9554 NRESET.
 * Toggles ALDO3+ALDO4+BLDO2 off for 2s, then back on.
 * Required after software reset (GPS I2C interface stays stale without power cycle).
 */
static void gps_ensure_power(i2c_master_bus_handle_t bus)
{
    ESP_LOGW(TAG, "=== GPS POWER CYCLE ===");

    /* ── v15: Restored mainBAK power cycle exactly ──
     * 1. Assert NRESET LOW via TCA9554 P5
     * 2. Cut GPS rails via AXP2101 — wait 2s
     * 3. Restore GPS rails — wait 500ms
     * 4. Release NRESET (P5 HIGH)
     * 5. Wait 3s for LC76G boot
     * NO boot-window commands. NO probing. Just clean power cycle. */

    /* Step 1: Assert NRESET LOW via TCA9554 P5 before cutting power */
    i2c_master_dev_handle_t tca = NULL;
    i2c_device_config_t tca_cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x20, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &tca_cfg, &tca) == ESP_OK) {
        tca9554_write_reg(tca, 0x03, 0xCF);  /* P4,P5 as outputs */
        tca9554_write_reg(tca, 0x01, 0x10);   /* P4=HIGH(FORCE_ON), P5=LOW(NRESET) */
        ESP_LOGI(TAG, "  NRESET asserted (P5=LOW)");
        i2c_master_bus_rm_device(tca);
    }

    /* Step 2: PMIC — Turn OFF GPS rails, wait, turn ON */
    i2c_master_dev_handle_t pmu = NULL;
    i2c_device_config_t pmu_cfg = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(bus, &pmu_cfg, &pmu) == ESP_OK) {
        uint8_t ldo = 0;
        axp2101_read_reg(pmu, 0x90, &ldo);
        ESP_LOGI(TAG, "  PMIC 0x90 before: 0x%02X", ldo);

        /* Turn OFF ALDO3(bit2)+ALDO4(bit3)+BLDO2(bit5) = mask 0x2C */
        axp2101_write_reg(pmu, 0x90, ldo & ~0x2C);
        /* v15h: Extended power-off to 5s (was 2s) to ensure full discharge
         * and clear any persistent I2C state in the LC76G. */
        ESP_LOGI(TAG, "  GPS rails OFF — waiting 5s (extended v15h)");
        vTaskDelay(pdMS_TO_TICKS(5000));

        /* Turn ON all rails */
        axp2101_write_reg(pmu, 0x90, ldo | 0x2C);
        ESP_LOGI(TAG, "  GPS rails ON");
        vTaskDelay(pdMS_TO_TICKS(500));

        i2c_master_bus_rm_device(pmu);
    }

    /* Step 3: Release NRESET (P5 HIGH) */
    if (i2c_master_bus_add_device(bus, &tca_cfg, &tca) == ESP_OK) {
        tca9554_write_reg(tca, 0x01, 0x30);  /* P4=HIGH, P5=HIGH */
        ESP_LOGI(TAG, "  NRESET released (P5=HIGH)");
        i2c_master_bus_rm_device(tca);
    }

    /* Step 4: Wait for LC76G boot
     * v15h: Extended to 5s (was 3s) — give module more time to start
     * populating its I2C TX buffer with NMEA sentences. */
    ESP_LOGI(TAG, "  Waiting 5s for LC76G boot (extended v15h)...");
    vTaskDelay(pdMS_TO_TICKS(5000));

    ESP_LOGW(TAG, "=== GPS POWER CYCLE COMPLETE ===");
}
/**
 * @brief Log AXP2101 PMIC power rail status for diagnostics.
 * Reads key registers and logs which LDO/DCDC rails are enabled and at what voltage.
 * This helps diagnose whether the GPS module has power.
 *
 * AXP2101 Register Map (key addresses):
 *   0x80: DCDC on/off control (DC1=b0, DC2=b1, DC3=b2, DC4=b3, DC5=b4)
 *   0x90: LDO on/off control 0 (ALDO1=b0, ALDO2=b1, ALDO3=b2, ALDO4=b3, BLDO1=b4, BLDO2=b5)
 *   0x91: LDO on/off control 1 (DLDO1=b0, DLDO2=b1, CPUSLDO=b2)
 *   0x82-0x86: DCDC1-5 voltage setting
 *   0x92-0x95: ALDO1-4 voltage (val * 100 + 500 mV)
 *   0x96-0x97: BLDO1-2 voltage (val * 100 + 500 mV)
 */
static void axp2101_log_status(i2c_master_bus_handle_t bus)
{
    /* Create temporary device handle for AXP2101 at 0x34 */
    i2c_master_dev_handle_t pmu = NULL;
    i2c_device_config_t pmu_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x34,
        .scl_speed_hz = 400000,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &pmu_cfg, &pmu);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PMIC: cannot add device 0x34: %s", esp_err_to_name(ret));
        return;
    }

    uint8_t dcdc_ctrl = 0, ldo_ctrl0 = 0, ldo_ctrl1 = 0;
    uint8_t aldo1_v = 0, aldo2_v = 0, aldo3_v = 0, aldo4_v = 0;
    uint8_t bldo1_v = 0, bldo2_v = 0;

    /* Read control registers */
    bool ok = true;
    ok &= (axp2101_read_reg(pmu, 0x80, &dcdc_ctrl) == ESP_OK);
    ok &= (axp2101_read_reg(pmu, 0x90, &ldo_ctrl0) == ESP_OK);
    ok &= (axp2101_read_reg(pmu, 0x91, &ldo_ctrl1) == ESP_OK);

    if (!ok) {
        ESP_LOGW(TAG, "PMIC: failed to read control registers");
        i2c_master_bus_rm_device(pmu);
        return;
    }

    /* Read voltage registers */
    axp2101_read_reg(pmu, 0x92, &aldo1_v);
    axp2101_read_reg(pmu, 0x93, &aldo2_v);
    axp2101_read_reg(pmu, 0x94, &aldo3_v);
    axp2101_read_reg(pmu, 0x95, &aldo4_v);
    axp2101_read_reg(pmu, 0x96, &bldo1_v);
    axp2101_read_reg(pmu, 0x97, &bldo2_v);

    ESP_LOGI(TAG, "=== AXP2101 PMIC Rail Status ===");
    ESP_LOGI(TAG, "  DCDC ctrl=0x%02X: DC1=%s DC2=%s DC3=%s DC4=%s DC5=%s",
             dcdc_ctrl,
             (dcdc_ctrl & 0x01) ? "ON" : "off",
             (dcdc_ctrl & 0x02) ? "ON" : "off",
             (dcdc_ctrl & 0x04) ? "ON" : "off",
             (dcdc_ctrl & 0x08) ? "ON" : "off",
             (dcdc_ctrl & 0x10) ? "ON" : "off");
    ESP_LOGI(TAG, "  LDO ctrl0=0x%02X: ALDO1=%s(%umV) ALDO2=%s(%umV) ALDO3=%s(%umV) ALDO4=%s(%umV)",
             ldo_ctrl0,
             (ldo_ctrl0 & 0x01) ? "ON" : "off", (unsigned)(aldo1_v * 100 + 500),
             (ldo_ctrl0 & 0x02) ? "ON" : "off", (unsigned)(aldo2_v * 100 + 500),
             (ldo_ctrl0 & 0x04) ? "ON" : "off", (unsigned)(aldo3_v * 100 + 500),
             (ldo_ctrl0 & 0x08) ? "ON" : "off", (unsigned)(aldo4_v * 100 + 500));
    ESP_LOGI(TAG, "  LDO ctrl0=0x%02X: BLDO1=%s(%umV) BLDO2=%s(%umV)",
             ldo_ctrl0,
             (ldo_ctrl0 & 0x10) ? "ON" : "off", (unsigned)(bldo1_v * 100 + 500),
             (ldo_ctrl0 & 0x20) ? "ON" : "off", (unsigned)(bldo2_v * 100 + 500));
    ESP_LOGI(TAG, "  LDO ctrl1=0x%02X: DLDO1=%s DLDO2=%s CPUSLDO=%s",
             ldo_ctrl1,
             (ldo_ctrl1 & 0x01) ? "ON" : "off",
             (ldo_ctrl1 & 0x02) ? "ON" : "off",
             (ldo_ctrl1 & 0x04) ? "ON" : "off");

    i2c_master_bus_rm_device(pmu);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Tier 3 Recovery — Full PMIC power cycle with handle teardown/rebuild
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Full recovery: remove device handles, power cycle, re-add handles.
 *
 * This is the LAST RESORT — only triggered after 50+ consecutive RX failures,
 * which indicates the LC76G I2C state machine is irrecoverably poisoned.
 *
 * CRITICAL: Device handles must be removed before power cycle and re-created
 * after, because the LC76G resets its I2C slave endpoints during power loss.
 */

/* See wiki §5.1 — Full recovery: remove handles → power cycle → re-add → WAKE → primer+drain */
static void gps_full_recovery(i2c_master_bus_handle_t bus)
{
    ESP_LOGE(TAG, "=== FULL RECOVERY: 50+ failures — PMIC power cycle ===");

    /* Step 1: Remove stale device handles */
    esp_err_t ret;
    if (lc76g_handle) {
        ret = i2c_master_bus_rm_device(lc76g_handle);
        ESP_LOGI(TAG, "  rm_device 0x50: %s", esp_err_to_name(ret));
        lc76g_handle = NULL;
    }
    if (lc76g_read_handle) {
        ret = i2c_master_bus_rm_device(lc76g_read_handle);
        ESP_LOGI(TAG, "  rm_device 0x54: %s", esp_err_to_name(ret));
        lc76g_read_handle = NULL;
    }
    if (lc76g_dwr_handle) {
        ret = i2c_master_bus_rm_device(lc76g_dwr_handle);
        ESP_LOGI(TAG, "  rm_device 0x58: %s", esp_err_to_name(ret));
        lc76g_dwr_handle = NULL;
    }

    /* Step 2: Full PMIC power cycle (same as init) */
    ESP_LOGI(TAG, "  Starting power cycle...");
    gps_ensure_power(bus);
    ESP_LOGI(TAG, "  Power cycle done");

    /* Step 3: Re-create device handles */
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LC76G_I2C_ADDR_WRITE,
        .scl_speed_hz = LC76G_I2C_CLK_HZ,
    };
    ret = i2c_master_bus_add_device(bus, &cfg, &lc76g_handle);
    ESP_LOGI(TAG, "  add_device 0x50: %s", esp_err_to_name(ret));

    cfg.device_address = LC76G_I2C_ADDR_READ;
    ret = i2c_master_bus_add_device(bus, &cfg, &lc76g_read_handle);
    ESP_LOGI(TAG, "  add_device 0x54: %s", esp_err_to_name(ret));

    /* v15e: 0x58 — NO disable_ack_check (it causes bus lockup).
     * 0x58 will NACK but that's fine — we skip 0x58 writes in WAKE. */
    cfg.device_address = LC76G_I2C_ADDR_DATA_WR;
    ret = i2c_master_bus_add_device(bus, &cfg, &lc76g_dwr_handle);
    ESP_LOGI(TAG, "  add_device 0x58: %s", esp_err_to_name(ret));

    /* Step 4: WAKE + primer to activate I2C TX buffer (same as boot)
     * v15j: Without this, data doesn't flow after recovery because the
     * module's I2C TX buffer fill mechanism isn't primed. */
    vTaskDelay(pdMS_TO_TICKS(1000));  /* Settle time */

    /* 3 WAKE cycles (CW to 0x50 + dummy to 0x58) */
    for (int w = 0; w < 3; w++) {
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
        i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t wd = 0x00;
        i2c_master_transmit(lc76g_dwr_handle, &wd, 1, 500);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* Primer: TxRx + data request to activate TX buffer fill */
    i2c_master_bus_reset(bus);
    vTaskDelay(pdMS_TO_TICKS(50));
    {
        uint8_t q[] = {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
        uint8_t tr[4] = {0};
        i2c_master_transmit_receive(lc76g_handle, q, sizeof(q), tr, 4, 1000);
    }
    i2c_master_bus_reset(bus);
    vTaskDelay(pdMS_TO_TICKS(50));
    {
        uint8_t data_req[] = {0x00, 0x20, 0x51, 0xAA, 0x00, 0x01, 0x00, 0x00};
        i2c_master_transmit(lc76g_handle, data_req, sizeof(data_req), 1000);
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    /* Post-recovery probe */
    for (int w = 0; w < 5; w++) {
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(200));
        uint8_t q[] = {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
        esp_err_t tx = i2c_master_transmit(lc76g_handle, q, sizeof(q), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t r[4] = {0};
        esp_err_t rx = i2c_master_receive(lc76g_read_handle, r, 4, 1000);
        uint32_t avail = (uint32_t)r[0] | ((uint32_t)r[1] << 8)
                       | ((uint32_t)r[2] << 16) | ((uint32_t)r[3] << 24);
        ESP_LOGI(TAG, "  Post-recovery[%d]: tx=%s rx=%s avail=%lu",
                 w, esp_err_to_name(tx), esp_err_to_name(rx),
                 (unsigned long)avail);
        if (tx == ESP_OK && avail > 0 && avail < 65536) {
            ESP_LOGI(TAG, "  >>> Module alive with %lu bytes!", (unsigned long)avail);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    current_gps_debug.total_power_cycles++;
    ESP_LOGE(TAG, "=== FULL RECOVERY COMPLETE (power_cycles=%lu) ===",
             (unsigned long)current_gps_debug.total_power_cycles);
}

/* ──────────────────────────────────────────────────────────────────────────
 * GPS Task — Multi-approach I2C read with auto-detection
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief GPS reading task — tries multiple I2C approaches to find one that works.
 *
 * The Waveshare demo uses legacy I2C API with tx(0x50)+rx(0x54).
 * On some boards/ESP-IDF versions, 0x54 doesn't respond (NACK).
 * We try these approaches in order:
 *   1. tx(0x50) + rx(0x54) — official CASIC protocol
 *   2. transmit_receive on 0x50 — repeated-START, same address
 *   3. tx(0x50) + rx(0x50) — separate transactions, same address
 *   4. rx(0x54) with disable_ack_check — ignore NACK, read anyway
 *   5. raw rx(0x50) — no query, just read
 *
 * Once a method works, we use it for the main loop.
 */
static void gps_task(void *pvParameters)
{
    ESP_LOGI(TAG, "GPS task started — LC76G CASIC I2C (TX 0x50 / RX 0x54)");

    i2c_master_bus_handle_t bus = display_get_i2c_handle();

    /* ── PMIC diagnostic ── */
    axp2101_log_status(bus);

    /* ── Warm-up: standard WAKE + avail check (mainBAK/wiki pattern) ──
     * 3 WAKE cycles to activate I2C TX buffer, then 5 avail checks. */
    ESP_LOGI(TAG, "  Warm-up: standard WAKE activation...");
    i2c_master_bus_reset(bus);
    vTaskDelay(pdMS_TO_TICKS(100));

    /* Diagnostic: probe 0x50/0x54/0x58 — may NACK initially, NORMAL */
    for (uint8_t addr = 0x50; addr <= 0x58; addr += 4) {
        esp_err_t p = i2c_master_probe(bus, addr, 100);
        ESP_LOGI(TAG, "  Probe 0x%02X: %s", addr, esp_err_to_name(p));
    }

    /* See wiki §5.2 — I2C WAKE Mechanism */
    /* v15g: Standard WAKE = CW to 0x50 + dummy to 0x58.
     * 0x58 NACKs but the bus activity is the actual trigger.
     * NO disable_ack_check — the NACK is harmless and keeps bus healthy. */
    for (int w = 0; w < 3; w++) {
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));

        uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
        esp_err_t cw = i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t dummy = 0x00;
        esp_err_t dw = i2c_master_transmit(lc76g_dwr_handle, &dummy, 1, 500);
        ESP_LOGI(TAG, "  Wake[%d]: cw=%s dw=%s", w, esp_err_to_name(cw), esp_err_to_name(dw));
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* ── Check initial avail ── */
    for (int i = 0; i < 5; i++) {
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t q[] = {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
        esp_err_t tx = i2c_master_transmit(lc76g_handle, q, sizeof(q), 1000);
        vTaskDelay(pdMS_TO_TICKS(10));
        uint8_t r[4] = {0};
        esp_err_t rx = i2c_master_receive(lc76g_read_handle, r, 4, 1000);
        uint32_t avail = (uint32_t)r[0] | ((uint32_t)r[1]<<8) |
                         ((uint32_t)r[2]<<16) | ((uint32_t)r[3]<<24);
        ESP_LOGI(TAG, "  Init[%d]: tx=%s rx=%s avail=%lu [%02X%02X%02X%02X]",
                 i, esp_err_to_name(tx), esp_err_to_name(rx),
                 (unsigned long)avail, r[0], r[1], r[2], r[3]);
        if (rx == ESP_OK && avail > 0 && avail < 65536) {
            ESP_LOGI(TAG, "  >>> LC76G I2C active with %lu bytes!", (unsigned long)avail);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* See wiki §5.4 — Primer Mechanism (REQUIRED at boot) */
    /* ── v15i: Activate I2C TX buffer with TxRx + data request ──
     * The CASIC protocol requires a data request (CR_CMD offset 0x2000) to
     * prime the module's I2C TX buffer fill mechanism. Without this, the
     * module doesn't route NMEA to the I2C TX buffer (avail stays 0).
     * Discovered empirically: v15h got data with this step, v15i without it
     * got zero bytes despite identical main loop code. */
    {
        ESP_LOGI(TAG, "  Priming I2C TX buffer...");

        /* Step 1: TxRx activation on 0x50 — enables 0x54 read endpoint */
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t q[] = {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
        uint8_t tr[4] = {0};
        esp_err_t tre = i2c_master_transmit_receive(lc76g_handle, q, sizeof(q), tr, 4, 1000);
        ESP_LOGI(TAG, "  Prime TxRx: %s [%02X%02X%02X%02X]",
                 esp_err_to_name(tre), tr[0], tr[1], tr[2], tr[3]);

        /* Step 2: Data request (CR_CMD offset 0x2000, 256 bytes) — primes
         * the module to start filling its I2C TX buffer with NMEA data.
         * The RX will likely NACK — that's OK, the TX alone is sufficient. */
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));
        uint8_t data_req[] = {0x00, 0x20, 0x51, 0xAA, 0x00, 0x01, 0x00, 0x00};
        esp_err_t dtx = i2c_master_transmit(lc76g_handle, data_req, sizeof(data_req), 1000);
        ESP_LOGI(TAG, "  Prime data_req: %s", esp_err_to_name(dtx));
        /* Intentionally skip RX — 0x54 likely NACKs here, and the TX alone
         * is what triggers the module to start buffering. */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "Entering main polling loop...");

    static uint8_t nmea_buf[GPS_NMEA_BUF_SIZE];
    gps_data_t local_data = {0};
    uint32_t total_bytes = 0;
    uint32_t total_sentences = 0;
    uint32_t cycle = 0;
    bool ever_received = false;
    uint32_t consecutive_fails = 0;
    uint32_t empty_polls = 0;

    while (1) {
        /* ── Heartbeat: unconditional log every 100 cycles ── */
        if (cycle % 100 == 0) {
            ESP_LOGI(TAG, "[HEARTBEAT] cycle=%lu fails=%lu empty=%lu bytes=%lu sentences=%lu",
                     (unsigned long)cycle, (unsigned long)consecutive_fails,
                     (unsigned long)empty_polls, (unsigned long)total_bytes,
                     (unsigned long)total_sentences);
        }

        /* See wiki §6.4 — 50ms bus reset delay (v15L2 optimal) */
        /* ── Bus reset before each cycle ──
         * Prevents bus state corruption from accumulating across cycles.
         * v15L proved skipping this causes faster degradation.
         * Delay tuning: 20ms→45s flow, 50ms→125s flow, 100ms→83s flow.
         * 50ms is the optimal value on this shared I2C bus. */
        i2c_master_bus_reset(bus);
        vTaskDelay(pdMS_TO_TICKS(50));

        /* ── Step 1: Query available data ──
         * TX CASIC length query to 0x50, RX 4-byte response from 0x54. */
        uint8_t queryData[] = {0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00};
        uint8_t readData[4] = {0};

        esp_err_t tx_ret = i2c_master_transmit(lc76g_handle, queryData, sizeof(queryData), 1000);
        if (tx_ret != ESP_OK) {
            consecutive_fails++;

            /* Track high-water mark */
            if (consecutive_fails > current_gps_debug.max_consecutive_fails) {
                current_gps_debug.max_consecutive_fails = consecutive_fails;
            }

            if (cycle < 10 || (cycle % 50) == 0)
                ESP_LOGW(TAG, "TX failed: %s (c=%lu, f=%lu)",
                         esp_err_to_name(tx_ret),
                         (unsigned long)cycle, (unsigned long)consecutive_fails);

            /* ── Tier 3: Full power cycle (at 100 TX failures) ── */
            if (consecutive_fails >= 100) {
                gps_full_recovery(bus);
                consecutive_fails = 0;
            }

            cycle++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(10));

        esp_err_t rx_ret = i2c_master_receive(lc76g_read_handle, readData, 4, 1000);
        if (rx_ret != ESP_OK) {
            /* v15: Without disable_ack_check, RX failures mean 0x54 NACKed.
             * This is the normal path when the module's I2C slave is inactive.
             * Use transmit_receive(0x50) to re-register 0x54's slave address. */
            consecutive_fails++;
            current_gps_debug.total_nacks++;

            if (consecutive_fails > current_gps_debug.max_consecutive_fails) {
                current_gps_debug.max_consecutive_fails = consecutive_fails;
            }

            if (cycle < 10 || (cycle % 50) == 0)
                ESP_LOGW(TAG, "RX failed: %s (c=%lu, f=%lu)",
                         esp_err_to_name(rx_ret),
                         (unsigned long)cycle, (unsigned long)consecutive_fails);

            /* ── Recovery: transmit_receive(0x50) every 5 failures ──
             * v15g: TxRx activation ONLY (no WAKE here). In v15, this path
             * had only TxRx and successfully got 9206 bytes. Adding WAKE
             * (CW write) here in v15f poisoned the module's response register,
             * causing TxRx to return [4D4D4D4D] instead of avail data.
             * WAKE is handled in empty_polls + post-data paths instead. */
            /* See wiki §5.3, §5.6 — Activation + Re-Prime + Drain */
            /* Re-prime at every 5th consecutive fail.
             * v15L proved == 5 is worse; % 5 at 5,10,15 keeps module primed. */
            if (consecutive_fails > 0 && (consecutive_fails % 5 == 0)) {
                /* TxRx activation — re-registers 0x54's slave address */
                uint8_t act_r[4] = {0};
                esp_err_t act = i2c_master_transmit_receive(lc76g_handle,
                    queryData, sizeof(queryData), act_r, 4, 1000);
                current_gps_debug.total_activations++;

                ESP_LOGW(TAG, "  TxRx activation: %s [%02X%02X%02X%02X]",
                         esp_err_to_name(act), act_r[0], act_r[1], act_r[2], act_r[3]);

                /* v15k-fix2: Re-prime + drain after TxRx activation.
                 * v15h sustained 400+ cycles because its diagnostic code sent
                 * data_req(256) + drain_read(256) at each activation — this
                 * continuously re-primed the module's TX buffer fill mechanism.
                 * CRITICAL: Must drain-read the response after data_req!
                 * Without drain, data_req response stays queued and poisons
                 * subsequent avail queries (the v15k [2C2C2C2C] bug). */
                if (act == ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    i2c_master_bus_reset(bus);
                    vTaskDelay(pdMS_TO_TICKS(20));

                    /* Send data_req for 256 bytes → primes module TX buffer */
                    uint8_t dr[] = {0x00, 0x20, 0x51, 0xAA, 0x00, 0x01, 0x00, 0x00};
                    esp_err_t dr_tx = i2c_master_transmit(lc76g_handle, dr, sizeof(dr), 1000);

                    if (dr_tx == ESP_OK) {
                        vTaskDelay(pdMS_TO_TICKS(20));
                        /* Drain read — MUST consume the data_req response to
                         * prevent TX buffer poisoning of subsequent avail queries.
                         * Read into nmea_buf (safe, it's static and we're in
                         * the RX-fail path so it's not being used for data). */
                        uint8_t drain[256];
                        esp_err_t dr_rx = i2c_master_receive(lc76g_read_handle,
                            drain, 256, 1000);
                        if (dr_rx == ESP_OK) {
                            /* Process any NMEA data in the drain read */
                            uint32_t dlen = 256;
                            while (dlen > 0 && drain[dlen - 1] == 0) dlen--;
                            if (dlen > 0) {
                                for (uint32_t i = 0; i < dlen; i++) {
                                    char c = (char)drain[i];
                                    if (c == '\n' || c == '\r') {
                                        if (nmea_line_pos > 0) {
                                            nmea_line[nmea_line_pos] = '\0';
                                            process_nmea_line(nmea_line, &local_data);
                                            total_sentences++;
                                            nmea_line_pos = 0;
                                        }
                                    } else if (nmea_line_pos < (int)(sizeof(nmea_line) - 1)) {
                                        nmea_line[nmea_line_pos++] = c;
                                    }
                                }
                                total_bytes += dlen;
                            }
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }

            /* ── Tier 3: Full power cycle (at 100 failures) ──
             * v15j: Lowered from 200 to 100 — recovers faster from bus degradation. */
            if (consecutive_fails >= 100) {
                gps_full_recovery(bus);
                consecutive_fails = 0;
            }

            cycle++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* Parse available data length */
        uint32_t dataLength = (uint32_t)readData[0] | ((uint32_t)readData[1] << 8)
                            | ((uint32_t)readData[2] << 16) | ((uint32_t)readData[3] << 24);

        consecutive_fails = 0;

        /* ── Handle: no data available ── */
        if (dataLength == 0) {
            empty_polls++;

            /* See wiki §5.2 — WAKE after 5 empty polls (v15L2: lowered from 30) */
            /* v15k: WAKE after 5 empty polls — CW to 0x50 + dummy to 0x58.
             * The 0x58 write NACKs but the bus activity is essential for the
             * module's WAKE mechanism (v15j proved CW-only is insufficient). */
            if (empty_polls >= 5) {
                uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
                i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);
                vTaskDelay(pdMS_TO_TICKS(10));
                uint8_t dummy = 0x00;
                i2c_master_transmit(lc76g_dwr_handle, &dummy, 1, 500);
                empty_polls = 0;
                current_gps_debug.total_wakes++;
                if (cycle < 20 || cycle % 100 == 0)
                    ESP_LOGW(TAG, "  WAKE after 5 empty polls (c=%lu)",
                             (unsigned long)cycle);
            }
            /* 200ms poll delay when no data */
            vTaskDelay(pdMS_TO_TICKS(200));
            cycle++;
            continue;
        }

        /* ── Sanity check ── */
        if (dataLength > 65536) {
            /* Bogus value — skip */
            if (cycle < 20 || (cycle % 100) == 0)
                ESP_LOGW(TAG, "avail=bogus %lu (c=%lu)",
                         (unsigned long)dataLength, (unsigned long)cycle);
            cycle++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        empty_polls = 0;
        ESP_LOGI(TAG, "avail=%lu (c=%lu)", (unsigned long)dataLength, (unsigned long)cycle);

        /* ── Step 2: Request and read NMEA data ──
         * For the data request, use separate TX(0x50) + RX(0x54) since the
         * CASIC protocol expects the data read on a different address. */
        uint32_t readLen = dataLength;
        if (readLen > (GPS_NMEA_BUF_SIZE - 1)) readLen = GPS_NMEA_BUF_SIZE - 1;

        uint8_t dataReq[8];
        dataReq[0] = 0x00;
        dataReq[1] = 0x20;
        dataReq[2] = 0x51;
        dataReq[3] = 0xAA;
        dataReq[4] = (uint8_t)(readLen & 0xFF);
        dataReq[5] = (uint8_t)((readLen >> 8) & 0xFF);
        dataReq[6] = (uint8_t)((readLen >> 16) & 0xFF);
        dataReq[7] = (uint8_t)((readLen >> 24) & 0xFF);

        /* Data request TX with retry — after a TxRx WAKE, the bus may need
         * a reset before standard TX succeeds. Retry up to 3 times. */
        esp_err_t data_tx = ESP_FAIL;
        for (int retry = 0; retry < 3; retry++) {
            data_tx = i2c_master_transmit(lc76g_handle, dataReq, sizeof(dataReq), 1000);
            if (data_tx == ESP_OK) break;
            i2c_master_bus_reset(bus);
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (data_tx != ESP_OK) {
            ESP_LOGW(TAG, "data req TX failed (3 retries): %s (c=%lu)",
                     esp_err_to_name(data_tx), (unsigned long)cycle);
            cycle++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(10));

        esp_err_t data_rx = i2c_master_receive(lc76g_read_handle, nmea_buf, readLen, 2000);
        if (data_rx != ESP_OK) {
            ESP_LOGW(TAG, "data read(%lu) failed: %s (c=%lu)",
                     (unsigned long)readLen, esp_err_to_name(data_rx), (unsigned long)cycle);
            cycle++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ── Process received NMEA data ── */
        uint32_t got_len = readLen;
        while (got_len > 0 && nmea_buf[got_len - 1] == 0) got_len--;

        bool has_nmea = false;
        for (uint32_t i = 0; i < got_len && i < 256; i++) {
            if (nmea_buf[i] == '$') { has_nmea = true; break; }
        }

        if (!has_nmea || got_len == 0) {
            if (got_len > 0) {
                char preview[65];
                size_t plen = got_len < 64 ? got_len : 64;
                memcpy(preview, nmea_buf, plen);
                for (size_t i = 0; i < plen; i++) {
                    if (preview[i] < 32 || preview[i] > 126) preview[i] = '.';
                }
                preview[plen] = '\0';
                ESP_LOGI(TAG, "  non-NMEA (%lu): %.64s", (unsigned long)got_len, preview);
            }
            cycle++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* ── NMEA data received ── */
        nmea_buf[got_len] = '\0';
        total_bytes += got_len;

        if (!ever_received) {
            ESP_LOGI(TAG, "=== FIRST GPS DATA! (%lu bytes, c=%lu) ===",
                     (unsigned long)got_len, (unsigned long)cycle);
            char preview[161];
            size_t plen = got_len < 160 ? got_len : 160;
            memcpy(preview, nmea_buf, plen);
            preview[plen] = '\0';
            ESP_LOGI(TAG, "  %.160s", preview);
            ever_received = true;

            /* TODO: Enable 10 Hz + all constellations after validating stability
             * The $PAIR050,100 command can destabilize the CASIC I2C interface on
             * some LC76G firmware versions — verify with field testing before enabling.
             *
             * gps_handler_set_rate_hz(10);
             * gps_handler_set_constellations(true, true, true, true);
             */
        }

        /* Feed into line parser */
        for (uint32_t i = 0; i < got_len; i++) {
            char c = (char)nmea_buf[i];
            if (c == '\n' || c == '\r') {
                if (nmea_line_pos > 0) {
                    nmea_line[nmea_line_pos] = '\0';
                    process_nmea_line(nmea_line, &local_data);
                    total_sentences++;
                    nmea_line_pos = 0;
                }
            } else if (nmea_line_pos < (int)(sizeof(nmea_line) - 1)) {
                nmea_line[nmea_line_pos++] = c;
            }
        }

        /* Thread-safe update */
        if (gps_mutex && xSemaphoreTake(gps_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            memcpy(&current_gps_data, &local_data, sizeof(gps_data_t));
            current_gps_debug.total_bytes = total_bytes;
            current_gps_debug.total_sentences = total_sentences;
            current_gps_debug.cycle = cycle;
            current_gps_debug.consecutive_fails = consecutive_fails;
            xSemaphoreGive(gps_mutex);
        }

        /* See wiki §5.5 — Per-Read WAKE (REQUIRED for sustained flow) */
        /* v15k: WAKE after every successful data read — CW to 0x50 + dummy to 0x58.
         * Both components are essential: CW sets up the write, 0x58 bus activity
         * triggers the module's WAKE. v15j proved CW-only is insufficient. */
        {
            uint8_t wake_cw[] = {0x00, 0x10, 0x53, 0xAA, 0x01, 0x00, 0x00, 0x00};
            i2c_master_transmit(lc76g_handle, wake_cw, sizeof(wake_cw), 500);
            vTaskDelay(pdMS_TO_TICKS(10));
            uint8_t wd = 0x00;
            i2c_master_transmit(lc76g_dwr_handle, &wd, 1, 500);
            current_gps_debug.total_wakes++;
            /* v15k-fix2: Extra delay after WAKE gives bus breathing room.
             * Removing this in v15L (along with bus_reset skip) degraded
             * performance from 57,710B to 18,401B. Keep the delay. */
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if ((cycle % GPS_STATUS_LOG_CYCLES) == 0 && cycle > 0) {
            ESP_LOGI(TAG, "GPS: fix=%s sats=%d/%d speed=%.1f hdop=%.1f total=%luB %lus c=%lu",
                     local_data.fix_valid ? "YES" : "no",
                     local_data.satellites, local_data.visible_sats,
                     local_data.speed, local_data.hdop,
                     (unsigned long)total_bytes, (unsigned long)total_sentences,
                     (unsigned long)cycle);
            if (local_data.fix_valid) {
                ESP_LOGI(TAG, "  Pos: %.6f, %.6f  Alt: %.1fm  Hdg: %.1f",
                         local_data.latitude, local_data.longitude,
                         local_data.altitude, local_data.heading);
            }
        }
        cycle++;
    }   /* end while(1) */

}   /* end gps_task */




/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────── */

esp_err_t gps_handler_init(void)
{
    ESP_LOGI(TAG, "Initializing GPS handler (LC76G via I2C at 0x%02X)", LC76G_I2C_ADDR);

    /* Create mutex for thread-safe GPS data access */
    gps_mutex = xSemaphoreCreateMutex();
    if (gps_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create GPS mutex");
        return ESP_FAIL;
    }

    /* Get the shared I2C bus handle from display_init */
    i2c_master_bus_handle_t i2c_bus = display_get_i2c_handle();
    if (i2c_bus == NULL) {
        ESP_LOGE(TAG, "I2C bus handle is NULL — call display_init() first");
        return ESP_FAIL;
    }

    /* ── I2C Bus Scan — SKIP LC76G addresses to avoid corrupting its I2C state ──
     * Per wiki: Probing 0x50/0x54/0x58 can poison the LC76G I2C state machine.
     * A single wrong read on 0x50 corrupts it. Only power cycle recovers. */
    ESP_LOGI(TAG, "I2C bus scan (0x03–0x77, skipping LC76G 0x50/0x54/0x58):");
    int found_count = 0;
    for (uint8_t addr = 0x03; addr <= 0x77; addr++) {
        if (addr == 0x50 || addr == 0x54 || addr == 0x58) continue;  /* Skip LC76G */
        esp_err_t probe_ret = i2c_master_probe(i2c_bus, addr, 50);
        if (probe_ret == ESP_OK) {
            const char *name = "";
            if (addr == 0x5A) name = " (CST9217 touch)";
            else if (addr == 0x6B) name = " (QMI8658 IMU)";
            else if (addr == 0x34) name = " (AXP2101 PMIC)";
            else if (addr == 0x20) name = " (TCA9554 IO exp)";
            else if (addr == 0x51) name = " (PCF85063 RTC)";
            else if (addr == 0x18) name = " (ES8311 codec)";
            else if (addr == 0x40) name = " (ES7210 ADC)";
            ESP_LOGI(TAG, "  0x%02X%s", addr, name);
            found_count++;
        }
    }
    ESP_LOGI(TAG, "  Total: %d devices found", found_count);


    /* ── GPS PMIC power cycle — REQUIRED before device handle creation ──
     * Per wiki: "If the module was in a bad I2C state, only power cycling
     * recovers it." The power cycle guarantees the LC76G starts clean. */
    gps_ensure_power(i2c_bus);
    /* ── LC76G device handles — NO PROBE ── */
    ESP_LOGI(TAG, "Adding LC76G devices (0x50 write, 0x54 read) — no probe");

    /* Add LC76G write device (0x50 — CASIC command endpoint) */
    i2c_device_config_t write_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LC76G_I2C_ADDR_WRITE,
        .scl_speed_hz = LC76G_I2C_CLK_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(i2c_bus, &write_cfg, &lc76g_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add LC76G write device (0x%02X): %s",
                 LC76G_I2C_ADDR_WRITE, esp_err_to_name(ret));
        return ret;
    }

    /* Add LC76G read device (0x54 — CASIC NMEA data endpoint)
     * v15: NO disable_ack_check — per wiki, RX errors should be visible
     * so that the transmit_receive(0x50) recovery mechanism can fire. */
    i2c_device_config_t read_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LC76G_I2C_ADDR_READ,
        .scl_speed_hz = LC76G_I2C_CLK_HZ,
    };
    ret = i2c_master_bus_add_device(i2c_bus, &read_cfg, &lc76g_read_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add LC76G read device (0x%02X): %s",
                 LC76G_I2C_ADDR_READ, esp_err_to_name(ret));
        return ret;
    }

    /* Add LC76G data write device (0x58 — for sending NMEA commands)
     * v15e: NO disable_ack_check — it causes bus lockup (v15c/v15d proved).
     * 0x58 will NACK, which is fine. WAKE uses CW-to-0x50 only. */
    i2c_device_config_t dwr_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LC76G_I2C_ADDR_DATA_WR,
        .scl_speed_hz = LC76G_I2C_CLK_HZ,
    };
    ret = i2c_master_bus_add_device(i2c_bus, &dwr_cfg, &lc76g_dwr_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add LC76G data write device (0x%02X): %s — commands disabled",
                 LC76G_I2C_ADDR_DATA_WR, esp_err_to_name(ret));
        /* Non-fatal: GPS reading will work, just can't send commands */
    }

    memset(&current_gps_data, 0, sizeof(gps_data_t));
    memset(&current_gps_debug, 0, sizeof(gps_debug_t));

    ESP_LOGI(TAG, "GPS handler initialized — write=0x%02X read=0x%02X dwr=0x%02X (CASIC on I2C_NUM_0)",
             LC76G_I2C_ADDR_WRITE, LC76G_I2C_ADDR_READ, LC76G_I2C_ADDR_DATA_WR);
    return ESP_OK;
}

esp_err_t gps_handler_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        gps_task, "gps_task", 8192, NULL,
        5,   /* High priority for real-time GPS data */
        &gps_task_handle,
        0    /* Core 0 */
    );

    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create GPS task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "GPS task started on core 0");
    return ESP_OK;
}

esp_err_t gps_handler_get_data(gps_data_t *data)
{
    if (data == NULL) return ESP_ERR_INVALID_ARG;

    if (gps_mutex && xSemaphoreTake(gps_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(data, &current_gps_data, sizeof(gps_data_t));
        xSemaphoreGive(gps_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t gps_handler_get_debug(gps_debug_t *debug)
{
    if (debug == NULL) return ESP_ERR_INVALID_ARG;

    if (gps_mutex && xSemaphoreTake(gps_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(debug, &current_gps_debug, sizeof(gps_debug_t));
        xSemaphoreGive(gps_mutex);
        return ESP_OK;
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t gps_handler_send_cold_start(void)
{
    return lc76g_send_command("PQTMCOLD");
}

esp_err_t gps_handler_send_warm_start(void)
{
    return lc76g_send_command("PQTMWARM");
}

esp_err_t gps_handler_set_rate_hz(uint8_t hz)
{
    if (hz < 1 || hz > 10) {
        ESP_LOGE(TAG, "Invalid rate %d — must be 1-10 Hz", hz);
        return ESP_ERR_INVALID_ARG;
    }
    char body[24];
    uint16_t interval_ms = 1000 / hz;
    snprintf(body, sizeof(body), "PAIR050,%u", interval_ms);
    ESP_LOGI(TAG, "Setting GPS rate to %d Hz (%u ms)", hz, interval_ms);
    return lc76g_send_command(body);
}

esp_err_t gps_handler_set_constellations(bool gps, bool glonass, bool galileo, bool beidou)
{
    char body[48];
    snprintf(body, sizeof(body), "PAIR066,%d,%d,%d,%d,0,0",
             gps ? 1 : 0, glonass ? 1 : 0, galileo ? 1 : 0, beidou ? 1 : 0);
    ESP_LOGI(TAG, "Setting constellations: GPS=%d GLO=%d GAL=%d BDS=%d",
             gps, glonass, galileo, beidou);
    return lc76g_send_command(body);
}
