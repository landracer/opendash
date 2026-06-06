/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file ui_manager.c
 * @brief OpenDash GPS / Telemetry Unit — UI Manager Implementation
 *
 * Professional round-display layout for the 466×466 AMOLED. Three cycling
 * display modes, matching the center display's button-to-cycle pattern.
 *
 * ───────── MODE: GPS ─────────
 *         ┌──────────────────┐
 *        ╱    ◻ 3D FIX        ╲
 *       │                      │
 *       │       125            │   ← Speed (big)
 *       │      km/h            │
 *       │                      │
 *       │ ┌──────────────────┐ │
 *       │ │ 12 Sats  HDOP:1.2│ │   ← Info panel
 *       │ │ Lat:  39.1234    │ │
 *       │ │ Lon: -84.5678    │ │
 *       │ └──────────────────┘ │
 *        ╲     275°  ALT:325m ╱
 *         └──────────────────┘
 *
 * ───────── MODE: LAP ─────────
 *         ┌──────────────────┐
 *        ╱    ◻ LAP MODE      ╲
 *       │                      │
 *       │   01:23.456          │   ← Lap time (big)
 *       │                      │
 *       │ ┌──────────────────┐ │
 *       │ │ Δ: +0.234s       │ │   ← Delta to best
 *       │ │ Best: 01:22.222  │ │
 *       │ │ Speed: 125 km/h  │ │
 *       │ └──────────────────┘ │
 *        ╲     LAP #3         ╱
 *         └──────────────────┘
 *
 * ───────── MODE: GFORCE ─────────
 *         ┌──────────────────┐
 *        ╱    ◻ G-FORCE       ╲
 *       │                      │
 *       │       1.23 G         │   ← Total G (big)
 *       │                      │
 *       │ ┌──────────────────┐ │
 *       │ │ Lat:  0.45 G     │ │
 *       │ │ Lon: -0.12 G     │ │
 *       │ │ Vert:  0.98 G    │ │
 *       │ │ Gyro: 12.3°/s    │ │
 *       │ └──────────────────┘ │
 *        ╲  Pitch: 2° Roll: 1°╱
 *         └──────────────────┘
 *
 * Architecture:
 *   - All LVGL objects created once in ui_manager_init()
 *   - Mode switch only updates label text (zero allocation)
 *   - Render task on core 1, data update task on core 0
 *   - Thread safety via LVGL mutex from BSP (display_lvgl_lock)
 *
 * @see LVGL Documentation: https://docs.lvgl.io/master/
 */

#include "ui_manager.h"
#include "display_init.h"
#include "gps_handler.h"
#include "imu_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "opendash_common.h"
#include "opendash_fonts.h"
#include "opendash_ui_styles.h"

/* Background image */
#if __has_include("background_gps.h")
#include "background_gps.h"
#define HAS_BACKGROUND_IMAGE 1
#else
#define HAS_BACKGROUND_IMAGE 0
#endif

#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "ui_manager";

/* Display dimensions */
#define LCD_H_RES   466
#define LCD_V_RES   466

/* Update intervals */
#define UI_UPDATE_INTERVAL_MS   200   /* 5 Hz display refresh */

/* ──────────────────────────────────────────────────────────────────────────
 * UI Object References — Created once, updated per-mode
 * ──────────────────────────────────────────────────────────────────────── */

/* Main screen */
static lv_obj_t *screen_main = NULL;

/* Top status labels */
static lv_obj_t *status_label = NULL;       /* "3D FIX" / "LAP MODE" / "G-FORCE" */

/* Center primary display (big value) */
static lv_obj_t *primary_value = NULL;      /* Speed / Lap time / Total G */
static lv_obj_t *primary_unit = NULL;       /* "km/h" / "" / "G" */

/* Info panel (center box with 4 info lines) */
static lv_obj_t *info_panel = NULL;
static lv_obj_t *info_line[4] = {NULL};     /* 4 rows of info text */

/* Bottom status labels */
static lv_obj_t *bottom_left = NULL;
static lv_obj_t *bottom_right = NULL;

/* Mode indicator at very bottom */
static lv_obj_t *mode_indicator = NULL;

/* State */
static gps_display_mode_t current_mode = GPS_DISPLAY_MODE_GPS;
static opendash_display_layout_t current_layout;
static TaskHandle_t ui_update_task_handle = NULL;
static TaskHandle_t ui_render_task_handle = NULL;

/* Lap timing state */
static uint32_t lap_start_ticks = 0;
static uint32_t lap_best_time_ms = 0;
static uint32_t lap_count = 0;
static bool lap_in_progress = false;

/* Touch gesture state */
static int16_t swipe_start_x = 0;
static uint32_t last_swipe_time = 0;

/**
 * @brief Touch swipe event handler — cycle display modes.
 */
static void touch_swipe_handler(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        lv_point_t pt;
        lv_indev_get_point(lv_indev_active(), &pt);
        swipe_start_x = pt.x;
    }
    else if (code == LV_EVENT_RELEASED) {
        uint32_t now = esp_log_timestamp();
        if (now - last_swipe_time < 500) return;

        lv_point_t pt;
        lv_indev_get_point(lv_indev_active(), &pt);
        int16_t dx = pt.x - swipe_start_x;

        if (dx > 60 || dx < -60) {
            last_swipe_time = now;
            gps_display_mode_t next = (current_mode + 1) % GPS_DISPLAY_MODE_COUNT;
            current_mode = next;
            ESP_LOGI(TAG, "Swipe → mode %d", current_mode);
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * UI Creation (called once)
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Create the full screen layout for the 466×466 round display.
 *
 * All widgets created here. Mode switches only change label text.
 */
static void create_screen_layout(void)
{
    screen_main = lv_obj_create(NULL);
    lv_obj_set_size(screen_main, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(screen_main, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(screen_main, 0, 0);
    lv_obj_set_style_pad_all(screen_main, 0, 0);
    lv_obj_clear_flag(screen_main, LV_OBJ_FLAG_SCROLLABLE);

    /* ── Background image ── */
#if HAS_BACKGROUND_IMAGE
    lv_obj_t *bg = lv_image_create(screen_main);
    lv_image_set_src(bg, &background_gps_dsc);
    lv_obj_set_size(bg, LCD_H_RES, LCD_V_RES);
    lv_obj_center(bg);
    lv_obj_set_style_image_opa(bg, 76, 0);
    lv_obj_move_to_index(bg, 0);
#endif

    /* ── Status label (top center) ── */
    status_label = lv_label_create(screen_main);
    lv_label_set_text(status_label, "NO FIX");
    lv_obj_set_style_text_font(status_label, &OPENDASH_FONT_DEFAULT_SMALL, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF3333), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 35);

    /* ── Primary value (large, center-top) ── */
    primary_value = lv_label_create(screen_main);
    lv_label_set_text(primary_value, "---");
    lv_obj_set_style_text_font(primary_value, &OPENDASH_FONT_DEFAULT_LARGE, 0);
    lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(primary_value, LV_ALIGN_TOP_MID, 0, 80);

    /* ── Primary unit label ── */
    primary_unit = lv_label_create(screen_main);
    lv_label_set_text(primary_unit, "km/h");
    lv_obj_set_style_text_font(primary_unit, &OPENDASH_FONT_DEFAULT_MEDIUM, 0);
    lv_obj_set_style_text_color(primary_unit, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(primary_unit, LV_ALIGN_TOP_MID, 0, 150);

    /* ── Info panel (rounded box, centered) ── */
    info_panel = lv_obj_create(screen_main);
    lv_obj_set_size(info_panel, 360, 150);
    lv_obj_align(info_panel, LV_ALIGN_CENTER, 0, 30);
    lv_obj_set_style_bg_color(info_panel, lv_color_hex(0x1A1A1A), 0);
    lv_obj_set_style_bg_opa(info_panel, 230, 0);
    lv_obj_set_style_border_color(info_panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(info_panel, 1, 0);
    lv_obj_set_style_radius(info_panel, 12, 0);
    lv_obj_set_style_pad_all(info_panel, 10, 0);
    lv_obj_clear_flag(info_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 4 info lines inside the panel */
    for (int i = 0; i < 4; i++) {
        info_line[i] = lv_label_create(info_panel);
        lv_label_set_text(info_line[i], "---");
        lv_obj_set_style_text_font(info_line[i], &OPENDASH_FONT_DEFAULT_SMALL, 0);
        lv_obj_set_style_text_color(info_line[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(info_line[i], 340);
        lv_obj_align(info_line[i], LV_ALIGN_TOP_LEFT, 0, i * 32 + 2);
    }

    /* ── Bottom left/right labels ── */
    bottom_left = lv_label_create(screen_main);
    lv_label_set_text(bottom_left, "---");
    lv_obj_set_style_text_font(bottom_left, &OPENDASH_FONT_DEFAULT_SMALL, 0);
    lv_obj_set_style_text_color(bottom_left, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(bottom_left, LV_ALIGN_BOTTOM_LEFT, 50, -55);

    bottom_right = lv_label_create(screen_main);
    lv_label_set_text(bottom_right, "---");
    lv_obj_set_style_text_font(bottom_right, &OPENDASH_FONT_DEFAULT_SMALL, 0);
    lv_obj_set_style_text_color(bottom_right, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(bottom_right, LV_ALIGN_BOTTOM_RIGHT, -50, -55);

    /* ── Mode indicator (very bottom center) ── */
    mode_indicator = lv_label_create(screen_main);
    lv_label_set_text(mode_indicator, "GPS");
    lv_obj_set_style_text_font(mode_indicator, &OPENDASH_FONT_DEFAULT_SMALL, 0);
    lv_obj_set_style_text_color(mode_indicator, lv_color_hex(0x666666), 0);
    lv_obj_align(mode_indicator, LV_ALIGN_BOTTOM_MID, 0, -30);

    ESP_LOGI(TAG, "Screen layout created for %dx%d round AMOLED", LCD_H_RES, LCD_V_RES);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Mode-Specific Data Update Functions
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief Update UI with GPS mode data.
 */
static void update_gps_mode(void)
{
    gps_data_t gps = {0};
    gps_handler_get_data(&gps);

    /* Status label: fix status */
    if (gps.fix_valid) {
        lv_label_set_text(status_label, "3D FIX");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0x33FF33), 0);
    } else {
        lv_label_set_text(status_label, "NO FIX");
        lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF3333), 0);
    }

    /* Primary: speed (convert to user's unit preference) */
    char buf[32];
    if (gps.fix_valid) {
        float display_speed = opendash_convert_speed(gps.speed, current_layout.speed_unit);
        snprintf(buf, sizeof(buf), "%.0f", display_speed);
    } else {
        snprintf(buf, sizeof(buf), "---");
    }
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, opendash_get_speed_suffix(current_layout.speed_unit));

    /* Info panel lines */
    snprintf(buf, sizeof(buf), "%d/%d Sats  HDOP: %.1f", gps.satellites, gps.visible_sats, gps.hdop);
    lv_label_set_text(info_line[0], buf);

    snprintf(buf, sizeof(buf), "Lat:  %.5f", gps.latitude);
    lv_label_set_text(info_line[1], buf);

    snprintf(buf, sizeof(buf), "Lon: %.5f", gps.longitude);
    lv_label_set_text(info_line[2], buf);

    /* Altitude: convert m → ft if using imperial distance */
    float display_alt = gps.altitude;
    const char *alt_unit = "m";
    if (current_layout.distance_unit == OPENDASH_DISTANCE_MI) {
        display_alt = gps.altitude * 3.28084f;  /* meters to feet */
        alt_unit = "ft";
    }
    float display_acc = gps.accuracy;
    const char *acc_unit = "m";
    if (current_layout.distance_unit == OPENDASH_DISTANCE_MI) {
        display_acc = gps.accuracy * 3.28084f;
        acc_unit = "ft";
    }
    snprintf(buf, sizeof(buf), "Alt: %.0f %s  Acc: %.0f %s", display_alt, alt_unit, display_acc, acc_unit);
    lv_label_set_text(info_line[3], buf);

    /* Bottom labels */
    if (gps.fix_valid && gps.speed > 1.0f) {
        snprintf(buf, sizeof(buf), "%.0f°", gps.heading);
    } else {
        snprintf(buf, sizeof(buf), "---°");
    }
    lv_label_set_text(bottom_left, buf);

    if (gps.fix_valid) {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d UTC", gps.hour, gps.minute, gps.second);
    } else {
        snprintf(buf, sizeof(buf), "--:--:-- UTC");
    }
    lv_label_set_text(bottom_right, buf);

    lv_label_set_text(mode_indicator, "[GPS]  LAP  GFORCE  DEBUG");
}

/**
 * @brief Update UI with LAP mode data.
 */
static void update_lap_mode(void)
{
    gps_data_t gps = {0};
    gps_handler_get_data(&gps);

    /* Status */
    lv_label_set_text(status_label, "LAP MODE");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x00AAFF), 0);

    /* Auto-start lap when moving and with fix */
    if (gps.fix_valid && gps.speed > 5.0f && !lap_in_progress) {
        lap_in_progress = true;
        lap_start_ticks = xTaskGetTickCount();
        lap_count++;
    }

    /* Primary: lap time */
    char buf[64];
    uint32_t elapsed_ms = 0;

    if (lap_in_progress) {
        elapsed_ms = (xTaskGetTickCount() - lap_start_ticks) * portTICK_PERIOD_MS;
        uint32_t min = elapsed_ms / 60000;
        uint32_t sec = (elapsed_ms % 60000) / 1000;
        uint32_t ms  = elapsed_ms % 1000;
        snprintf(buf, sizeof(buf), "%02lu:%02lu.%03lu", min, sec, ms);
    } else {
        snprintf(buf, sizeof(buf), "--:--.---");
    }
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, "");

    /* Info panel */
    if (lap_in_progress && lap_best_time_ms > 0) {
        int32_t delta_ms = (int32_t)elapsed_ms - (int32_t)lap_best_time_ms;
        snprintf(buf, sizeof(buf), "Delta: %+.3f s", delta_ms / 1000.0f);
        if (delta_ms > 0) {
            lv_obj_set_style_text_color(info_line[0], lv_color_hex(0xFF3333), 0);  /* Red = slower */
        } else {
            lv_obj_set_style_text_color(info_line[0], lv_color_hex(0x33FF33), 0);  /* Green = faster */
        }
    } else {
        snprintf(buf, sizeof(buf), "Delta: ---.--- s");
        lv_obj_set_style_text_color(info_line[0], lv_color_hex(0xFFFF00), 0);
    }
    lv_label_set_text(info_line[0], buf);

    /* Best time */
    if (lap_best_time_ms > 0) {
        uint32_t min = lap_best_time_ms / 60000;
        uint32_t sec = (lap_best_time_ms % 60000) / 1000;
        uint32_t ms  = lap_best_time_ms % 1000;
        snprintf(buf, sizeof(buf), "Best: %02lu:%02lu.%03lu", min, sec, ms);
    } else {
        snprintf(buf, sizeof(buf), "Best: --:--.---");
    }
    lv_label_set_text(info_line[1], buf);

    /* Current speed (convert to user preference) */
    float lap_speed = opendash_convert_speed(gps.speed, current_layout.speed_unit);
    snprintf(buf, sizeof(buf), "Speed: %.0f %s", lap_speed,
             opendash_get_speed_suffix(current_layout.speed_unit));
    lv_label_set_text(info_line[2], buf);

    /* Satellites */
    snprintf(buf, sizeof(buf), "Sats: %d   Fix: %s", gps.satellites, gps.fix_valid ? "YES" : "NO");
    lv_label_set_text(info_line[3], buf);

    /* Bottom */
    snprintf(buf, sizeof(buf), "Lap #%lu", lap_count);
    lv_label_set_text(bottom_left, buf);
    lv_label_set_text(bottom_right, lap_in_progress ? "RUNNING" : "STOPPED");

    lv_label_set_text(mode_indicator, "GPS  [LAP]  GFORCE  DEBUG");
}

/**
 * @brief Update UI with G-FORCE mode data.
 */
static void update_gforce_mode(void)
{
    imu_data_t imu = {0};
    imu_handler_get_data(&imu);

    /* Status */
    lv_label_set_text(status_label, "G-FORCE");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF8800), 0);

    /* Primary: total G */
    char buf[64];
    snprintf(buf, sizeof(buf), "%.2f", imu.total_g);
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, "G");

    /* Color-code total G */
    if (imu.total_g > 2.0f) {
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFF3333), 0);  /* Red = high G */
    } else if (imu.total_g > 1.5f) {
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFAA00), 0);  /* Orange */
    } else {
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);  /* White = normal */
    }

    /* Info panel: axis breakdown */
    snprintf(buf, sizeof(buf), "Lateral:    %+.2f G", imu.g_lateral);
    lv_label_set_text(info_line[0], buf);

    snprintf(buf, sizeof(buf), "Longitudinal: %+.2f G", imu.g_longitudinal);
    lv_label_set_text(info_line[1], buf);

    snprintf(buf, sizeof(buf), "Vertical:   %+.2f G", imu.g_vertical);
    lv_label_set_text(info_line[2], buf);

    float total_gyro = sqrtf(imu.gyro_x * imu.gyro_x + imu.gyro_y * imu.gyro_y + imu.gyro_z * imu.gyro_z);
    snprintf(buf, sizeof(buf), "Gyro: %.1f deg/s", total_gyro);
    lv_label_set_text(info_line[3], buf);

    /* Bottom: pitch and roll */
    snprintf(buf, sizeof(buf), "Pitch: %.1f°", imu.pitch);
    lv_label_set_text(bottom_left, buf);

    snprintf(buf, sizeof(buf), "Roll: %.1f°", imu.roll);
    lv_label_set_text(bottom_right, buf);

    lv_label_set_text(mode_indicator, "GPS  LAP  [GFORCE]  DEBUG");
}

/**
 * @brief Update UI with DEBUG mode data — GPS diagnostics.
 */
static void update_debug_mode(void)
{
    gps_data_t gps = {0};
    gps_handler_get_data(&gps);

    gps_debug_t dbg = {0};
    gps_handler_get_debug(&dbg);

    /* Status */
    lv_label_set_text(status_label, "DEBUG");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF00FF), 0);

    /* Primary: visible satellite count (BIG) */
    char buf[128];
    snprintf(buf, sizeof(buf), "%d/%d", gps.satellites, gps.visible_sats);
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, "used/vis sats");

    /* Color-code by satellite health */
    if (gps.fix_valid) {
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0x33FF33), 0);  /* Green = fix */
    } else if (gps.visible_sats > 0) {
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFAA00), 0);  /* Orange = seeing sats */
    } else {
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFF3333), 0);  /* Red = no sats at all */
    }

    /* Info panel: NMEA statistics */
    snprintf(buf, sizeof(buf), "Bytes:%lu Sent:%lu C:%lu",
             (unsigned long)dbg.total_bytes, (unsigned long)dbg.total_sentences,
             (unsigned long)dbg.cycle);
    lv_label_set_text(info_line[0], buf);

    snprintf(buf, sizeof(buf), "GGA:%lu RMC:%lu GSV:%lu",
             (unsigned long)dbg.cnt_gga, (unsigned long)dbg.cnt_rmc,
             (unsigned long)dbg.cnt_gsv);
    lv_label_set_text(info_line[1], buf);

    snprintf(buf, sizeof(buf), "GSA:%lu TXT:%lu PQTM:%lu",
             (unsigned long)dbg.cnt_gsa, (unsigned long)dbg.cnt_txt,
             (unsigned long)dbg.cnt_pqtm);
    lv_label_set_text(info_line[2], buf);

    snprintf(buf, sizeof(buf), "Cmds:%lu/%lu WarmUp:%lu",
             (unsigned long)dbg.cmds_ok, (unsigned long)dbg.cmds_sent,
             (unsigned long)dbg.warm_ups);
    lv_label_set_text(info_line[3], buf);

    /* Bottom: fix status and HDOP */
    snprintf(buf, sizeof(buf), "Fix:%d Q:%d", gps.fix_valid, gps.fix_quality);
    lv_label_set_text(bottom_left, buf);

    snprintf(buf, sizeof(buf), "HDOP:%.1f", gps.hdop);
    lv_label_set_text(bottom_right, buf);

    lv_label_set_text(mode_indicator, "GPS  LAP  GFORCE  [DEBUG]");
}

/* ──────────────────────────────────────────────────────────────────────────
 * Tasks
 * ──────────────────────────────────────────────────────────────────────── */

/**
 * @brief UI data update task — fetches sensor data and updates labels.
 */
static void ui_update_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI update task started (%d ms interval)", UI_UPDATE_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        if (display_lvgl_lock(50)) {
            switch (current_mode) {
                case GPS_DISPLAY_MODE_GPS:
                    update_gps_mode();
                    break;
                case GPS_DISPLAY_MODE_LAP:
                    update_lap_mode();
                    break;
                case GPS_DISPLAY_MODE_GFORCE:
                    update_gforce_mode();
                    break;
                case GPS_DISPLAY_MODE_DEBUG:
                    update_debug_mode();
                    break;
                default:
                    break;
            }
            display_lvgl_unlock();
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(UI_UPDATE_INTERVAL_MS));
    }
}

/**
 * @brief LVGL rendering task — calls lv_timer_handler() at high frequency.
 */
static void ui_render_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI render task started");
    uint32_t keepalive_counter = 0;

    while (1) {
        if (display_lvgl_lock(10)) {
            /* Every ~5 seconds, force a full screen redraw to keep
             * the CO5300 AMOLED from entering idle/sleep mode.     */
            if (++keepalive_counter >= 500) {
                keepalive_counter = 0;
                if (screen_main) {
                    lv_obj_invalidate(screen_main);
                }
            }
            lv_timer_handler();
            display_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));  /* ~100 FPS render rate */
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Public API
 * ──────────────────────────────────────────────────────────────────────── */

esp_err_t ui_manager_init(const opendash_display_layout_t *layout)
{
    if (layout == NULL) {
        ESP_LOGE(TAG, "Layout pointer is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(&current_layout, layout, sizeof(opendash_display_layout_t));

    ESP_LOGI(TAG, "Creating UI layout for 466x466 round AMOLED");
    ESP_LOGI(TAG, "Modes: GPS, LAP, GFORCE (cycle with boot button)");

    /* Create all UI objects (once) */
    if (display_lvgl_lock(1000)) {
        create_screen_layout();
        lv_scr_load(screen_main);
        /* Register touch swipe for mode cycling */
        lv_obj_add_event_cb(screen_main, touch_swipe_handler, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(screen_main, touch_swipe_handler, LV_EVENT_RELEASED, NULL);
        display_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for UI init");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UI layout created — starting in GPS mode");
    return ESP_OK;
}

esp_err_t ui_manager_start(void)
{
    /* Data update task on core 0 (sensor fetching) */
    BaseType_t ret = xTaskCreatePinnedToCore(
        ui_update_task, "ui_update", 4096, NULL,
        4,   /* Below sensor tasks */
        &ui_update_task_handle,
        0    /* Core 0 */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI update task");
        return ESP_FAIL;
    }

    /* LVGL render task on core 1 */
    ret = xTaskCreatePinnedToCore(
        ui_render_task, "ui_render", 8192, NULL,
        5,   /* High priority for smooth rendering */
        &ui_render_task_handle,
        1    /* Core 1 */
    );
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI render task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UI tasks running (update=core0, render=core1)");
    return ESP_OK;
}

void ui_manager_update_value(uint16_t data_point_id, float value)
{
    ESP_LOGD(TAG, "External update: DP 0x%04X = %.2f", data_point_id, value);
    /* Future: route externally-pushed values to display */
}

esp_err_t ui_manager_next_screen(void)
{
    gps_display_mode_t next = (current_mode + 1) % GPS_DISPLAY_MODE_COUNT;
    current_mode = next;
    ESP_LOGI(TAG, "Display mode → %d (%s)",
             current_mode,
             current_mode == GPS_DISPLAY_MODE_GPS ? "GPS" :
             current_mode == GPS_DISPLAY_MODE_LAP ? "LAP" :
             current_mode == GPS_DISPLAY_MODE_GFORCE ? "GFORCE" : "DEBUG");
    return ESP_OK;
}

esp_err_t ui_manager_set_display_mode(gps_display_mode_t mode)
{
    if (mode >= GPS_DISPLAY_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    current_mode = mode;
    return ESP_OK;
}

uint8_t ui_manager_get_current_screen(void)
{
    return (uint8_t)current_mode;
}
