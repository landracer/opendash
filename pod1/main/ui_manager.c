/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file ui_manager.c
 * @brief OpenDash Pod 1 — UI Manager Implementation
 *
 * Professional round-display layout for the 466×466 AMOLED.
 * Six cycling display modes showing engine parameters received via ESP-NOW
 * plus local IMU data for g-force and parachute deployment voting.
 *
 * ───────── MODE: OIL TEMP ─────────
 *         ┌──────────────────┐
 *        ╱    ◻ OIL TEMP      ╲
 *       │                      │
 *       │       220            │   ← Oil temp (big)
 *       │       °F             │
 *       │                      │
 *       │ ┌──────────────────┐ │
 *       │ │ Min: 180  Max:240│ │   ← Info panel
 *       │ │ Coolant: 195 °F  │ │
 *       │ │ Oil PSI: 45      │ │
 *       │ │ RPM: 5200        │ │
 *       │ └──────────────────┘ │
 *        ╲                    ╱
 *         └──────────────────┘
 *
 * Each sensor mode follows the same layout pattern:
 * - Large primary value centered
 * - Info panel with related values
 * - Min/max tracking
 * - Mode indicator at bottom
 *
 * Architecture:
 *   - All LVGL objects created once in ui_manager_init()
 *   - Mode switch only updates label text (zero allocation)
 *   - Data arrives via ESP-NOW → ui_manager_update_value()
 *   - IMU data read locally for GFORCE mode
 */

#include "ui_manager.h"
#include "display_init.h"
#include "imu_handler.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "opendash_common.h"
#include "opendash_data_model.h"
#include "opendash_fonts.h"
#include "opendash_ui_styles.h"

/* Background image */
#if __has_include("background_pod1.h")
#include "background_pod1.h"
#define HAS_BACKGROUND_IMAGE 1
#else
#define HAS_BACKGROUND_IMAGE 0
#endif

#include <stdio.h>
#include <string.h>
#include <math.h>

static const char *TAG = "ui_manager";

#define LCD_H_RES   466
#define LCD_V_RES   466
#define UI_UPDATE_INTERVAL_MS   200   /* 5 Hz display refresh */

/* ──────────────────────────────────────────────────────────────────────────
 * Cached data point values (updated via ESP-NOW from center)
 * ──────────────────────────────────────────────────────────────────────── */
static float s_oil_temp    = 0.0f;
static float s_oil_psi     = 0.0f;
static float s_coolant     = 0.0f;
static float s_afr         = 0.0f;
static float s_boost       = 0.0f;
static float s_rpm         = 0.0f;
static float s_intake_temp = 0.0f;
static float s_battery_v   = 0.0f;
static float s_lambda      = 0.0f;
static float s_egt_max     = 0.0f;

/* Min/max tracking per mode */
static float s_mode_min[POD_DISPLAY_MODE_COUNT];
static float s_mode_max[POD_DISPLAY_MODE_COUNT];
static bool  s_mode_has_data[POD_DISPLAY_MODE_COUNT];

/* ──────────────────────────────────────────────────────────────────────────
 * UI Object References — Created once, updated per-mode
 * ──────────────────────────────────────────────────────────────────────── */
static lv_obj_t *screen_main = NULL;

/* Full-screen readout objects */
static lv_obj_t *status_label = NULL;
static lv_obj_t *primary_value = NULL;
static lv_obj_t *primary_unit = NULL;
static lv_obj_t *min_max_label = NULL;

/* Debug overview panel */
static lv_obj_t *info_panel = NULL;
static lv_obj_t *info_line[8] = {NULL};

/* Navigation */
static lv_obj_t *mode_indicator = NULL;

/* State */
static pod_display_mode_t current_mode = POD_DISPLAY_MODE_OIL;
static pod_display_mode_t prev_mode = (pod_display_mode_t)-1;
static opendash_display_layout_t current_layout;
static TaskHandle_t ui_update_task_handle = NULL;
static TaskHandle_t ui_render_task_handle = NULL;

/* Touch gesture state */
static int16_t swipe_start_x = 0;
static uint32_t last_swipe_time = 0;

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
            current_mode = (current_mode + 1) % POD_DISPLAY_MODE_COUNT;
            ESP_LOGI(TAG, "Swipe → mode %d", current_mode);
        }
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * UI Creation (called once)
 * ──────────────────────────────────────────────────────────────────────── */

static void create_screen_layout(void)
{
    screen_main = lv_obj_create(NULL);
    lv_obj_set_size(screen_main, LCD_H_RES, LCD_V_RES);
    lv_obj_set_style_bg_color(screen_main, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(screen_main, 0, 0);
    lv_obj_set_style_pad_all(screen_main, 0, 0);
    lv_obj_clear_flag(screen_main, LV_OBJ_FLAG_SCROLLABLE);

#if HAS_BACKGROUND_IMAGE
    lv_obj_t *bg = lv_image_create(screen_main);
    lv_image_set_src(bg, &background_pod1_dsc);
    lv_obj_set_size(bg, LCD_H_RES, LCD_V_RES);
    lv_obj_center(bg);
    lv_obj_set_style_image_opa(bg, 76, 0);
    lv_obj_move_to_index(bg, 0);
#endif

    /* ═══ Full-Screen Readout Objects ═══ */

    /* Status label (top center — mode name) */
    status_label = lv_label_create(screen_main);
    lv_label_set_text(status_label, "OIL TEMP");
    lv_obj_set_style_text_font(status_label, &OPENDASH_FONT_DEFAULT_MEDIUM, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF8800), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 50);

    /* Primary value — XXXL 160px, dead center */
    primary_value = lv_label_create(screen_main);
    lv_label_set_text(primary_value, "---");
    lv_obj_set_style_text_font(primary_value, &kimbalt_160, 0);
    lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(primary_value, LV_ALIGN_CENTER, 0, -25);

    /* Unit label — below primary */
    primary_unit = lv_label_create(screen_main);
    lv_label_set_text(primary_unit, "\xc2\xb0""F");
    lv_obj_set_style_text_font(primary_unit, &OPENDASH_FONT_DEFAULT_MEDIUM, 0);
    lv_obj_set_style_text_color(primary_unit, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(primary_unit, LV_ALIGN_CENTER, 0, 55);

    /* Min/Max label — below unit */
    min_max_label = lv_label_create(screen_main);
    lv_label_set_text(min_max_label, "MIN: ---   MAX: ---");
    lv_obj_set_style_text_font(min_max_label, &OPENDASH_FONT_DEFAULT_MEDIUM, 0);
    lv_obj_set_style_text_color(min_max_label, lv_color_hex(0x888888), 0);
    lv_obj_align(min_max_label, LV_ALIGN_CENTER, 0, 90);

    /* ═══ Debug Overview Panel (hidden by default) ═══ */
    info_panel = lv_obj_create(screen_main);
    lv_obj_set_size(info_panel, 380, 300);
    lv_obj_align(info_panel, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(info_panel, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(info_panel, 240, 0);
    lv_obj_set_style_border_color(info_panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(info_panel, 1, 0);
    lv_obj_set_style_radius(info_panel, 16, 0);
    lv_obj_set_style_pad_all(info_panel, 12, 0);
    lv_obj_clear_flag(info_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(info_panel, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 8; i++) {
        info_line[i] = lv_label_create(info_panel);
        lv_label_set_text(info_line[i], "---");
        lv_obj_set_style_text_font(info_line[i], &OPENDASH_FONT_DEFAULT_MEDIUM, 0);
        lv_obj_set_style_text_color(info_line[i], lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_width(info_line[i], 356);
        lv_obj_align(info_line[i], LV_ALIGN_TOP_LEFT, 0, i * 34);
    }

    /* ═══ Mode Indicator (bottom) ═══ */
    mode_indicator = lv_label_create(screen_main);
    lv_label_set_text(mode_indicator, "[OIL]  CLT  AFR  BST  G  DBG");
    lv_obj_set_style_text_font(mode_indicator, &OPENDASH_FONT_DEFAULT_SMALL, 0);
    lv_obj_set_style_text_color(mode_indicator, lv_color_hex(0x666666), 0);
    lv_obj_align(mode_indicator, LV_ALIGN_BOTTOM_MID, 0, -35);

    ESP_LOGI(TAG, "Screen layout created for %dx%d round AMOLED (XXXL readout + debug overview)", LCD_H_RES, LCD_V_RES);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Helper: Track min/max for current mode's primary value
 * ──────────────────────────────────────────────────────────────────────── */
static void track_minmax(pod_display_mode_t mode, float value)
{
    if (!s_mode_has_data[mode]) {
        s_mode_min[mode] = value;
        s_mode_max[mode] = value;
        s_mode_has_data[mode] = true;
    } else {
        if (value < s_mode_min[mode]) s_mode_min[mode] = value;
        if (value > s_mode_max[mode]) s_mode_max[mode] = value;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
 * Mode layout toggling and indicator
 * ──────────────────────────────────────────────────────────────────────── */

static const char *s_mode_abbrev[] = {"OIL", "CLT", "AFR", "BST", "G", "DBG"};

static void apply_mode_layout(pod_display_mode_t mode)
{
    if (mode == POD_DISPLAY_MODE_DEBUG) {
        lv_obj_clear_flag(info_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(primary_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(primary_unit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(min_max_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(info_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(primary_value, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(primary_unit, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(min_max_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_mode_indicator(pod_display_mode_t mode)
{
    char buf[64];
    char *p = buf;
    for (int i = 0; i < POD_DISPLAY_MODE_COUNT; i++) {
        if (i > 0) { *p++ = ' '; *p++ = ' '; }
        if (i == (int)mode) {
            *p++ = '[';
            const char *s = s_mode_abbrev[i];
            while (*s) *p++ = *s++;
            *p++ = ']';
        } else {
            const char *s = s_mode_abbrev[i];
            while (*s) *p++ = *s++;
        }
    }
    *p = '\0';
    lv_label_set_text(mode_indicator, buf);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Mode-Specific Data Update Functions
 * ──────────────────────────────────────────────────────────────────────── */

static void update_oil_mode(void)
{
    float val = opendash_convert_temp(s_oil_temp, current_layout.temp_unit);
    track_minmax(POD_DISPLAY_MODE_OIL, val);

    lv_label_set_text(status_label, "OIL TEMP");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF8800), 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", val);
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, opendash_get_temp_suffix(current_layout.temp_unit));

    if (val > 260.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFF3333), 0);
    else if (val > 230.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFAA00), 0);
    else
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);

    snprintf(buf, sizeof(buf), "MIN: %.0f    MAX: %.0f",
             s_mode_min[POD_DISPLAY_MODE_OIL], s_mode_max[POD_DISPLAY_MODE_OIL]);
    lv_label_set_text(min_max_label, buf);
}

static void update_water_mode(void)
{
    float val = opendash_convert_temp(s_coolant, current_layout.temp_unit);
    track_minmax(POD_DISPLAY_MODE_WATER, val);

    lv_label_set_text(status_label, "COOLANT");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x00AAFF), 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", val);
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, opendash_get_temp_suffix(current_layout.temp_unit));

    if (val > 230.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFF3333), 0);
    else if (val > 210.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFAA00), 0);
    else
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);

    snprintf(buf, sizeof(buf), "MIN: %.0f    MAX: %.0f",
             s_mode_min[POD_DISPLAY_MODE_WATER], s_mode_max[POD_DISPLAY_MODE_WATER]);
    lv_label_set_text(min_max_label, buf);
}

static void update_afr_mode(void)
{
    track_minmax(POD_DISPLAY_MODE_AFR, s_afr);

    lv_label_set_text(status_label, "AFR");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0x33FF33), 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", s_afr);
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, "AFR");

    if (s_afr > 15.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFF3333), 0);
    else if (s_afr < 12.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0x3333FF), 0);
    else if (s_afr >= 14.2f && s_afr <= 15.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0x33FF33), 0);
    else
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);

    snprintf(buf, sizeof(buf), "MIN: %.1f    MAX: %.1f",
             s_mode_min[POD_DISPLAY_MODE_AFR], s_mode_max[POD_DISPLAY_MODE_AFR]);
    lv_label_set_text(min_max_label, buf);
}

static void update_boost_mode(void)
{
    float val = opendash_convert_pressure(s_boost, current_layout.pressure_unit);
    track_minmax(POD_DISPLAY_MODE_BOOST, val);

    lv_label_set_text(status_label, "BOOST");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF00FF), 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f", val);
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, opendash_get_pressure_suffix(current_layout.pressure_unit));

    if (val > 20.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFF3333), 0);
    else if (val > 15.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFAA00), 0);
    else
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);

    snprintf(buf, sizeof(buf), "MIN: %.1f    MAX: %.1f",
             s_mode_min[POD_DISPLAY_MODE_BOOST], s_mode_max[POD_DISPLAY_MODE_BOOST]);
    lv_label_set_text(min_max_label, buf);
}

static void update_gforce_mode(void)
{
    imu_data_t imu = {0};
    imu_handler_get_data(&imu);
    track_minmax(POD_DISPLAY_MODE_GFORCE, imu.total_g);

    lv_label_set_text(status_label, "G-FORCE");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF8800), 0);

    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", imu.total_g);
    lv_label_set_text(primary_value, buf);
    lv_label_set_text(primary_unit, "G");

    if (imu.total_g > 2.0f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFF3333), 0);
    else if (imu.total_g > 1.5f)
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFAA00), 0);
    else
        lv_obj_set_style_text_color(primary_value, lv_color_hex(0xFFFFFF), 0);

    snprintf(buf, sizeof(buf), "MIN: %.2f    MAX: %.2f",
             s_mode_min[POD_DISPLAY_MODE_GFORCE], s_mode_max[POD_DISPLAY_MODE_GFORCE]);
    lv_label_set_text(min_max_label, buf);
}

static void update_debug_mode(void)
{
    lv_label_set_text(status_label, "OVERVIEW");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFF00FF), 0);

    char buf[64];
    float dt, dp;

    snprintf(buf, sizeof(buf), "RPM:     %.0f", s_rpm);
    lv_label_set_text(info_line[0], buf);

    snprintf(buf, sizeof(buf), "AFR:     %.1f   \xce\xbb: %.3f", s_afr, s_lambda);
    lv_label_set_text(info_line[1], buf);

    dp = opendash_convert_pressure(s_boost, current_layout.pressure_unit);
    snprintf(buf, sizeof(buf), "Boost:   %.1f %s", dp,
             opendash_get_pressure_suffix(current_layout.pressure_unit));
    lv_label_set_text(info_line[2], buf);

    dt = opendash_convert_temp(s_oil_temp, current_layout.temp_unit);
    snprintf(buf, sizeof(buf), "Oil:     %.0f%s", dt,
             opendash_get_temp_suffix(current_layout.temp_unit));
    lv_label_set_text(info_line[3], buf);

    dt = opendash_convert_temp(s_coolant, current_layout.temp_unit);
    snprintf(buf, sizeof(buf), "Coolant: %.0f%s", dt,
             opendash_get_temp_suffix(current_layout.temp_unit));
    lv_label_set_text(info_line[4], buf);

    dp = opendash_convert_pressure(s_oil_psi, current_layout.pressure_unit);
    snprintf(buf, sizeof(buf), "Oil PSI: %.0f %s", dp,
             opendash_get_pressure_suffix(current_layout.pressure_unit));
    lv_label_set_text(info_line[5], buf);

    imu_data_t imu = {0};
    imu_handler_get_data(&imu);
    snprintf(buf, sizeof(buf), "G-Force: %.2f   P:%.0f R:%.0f",
             imu.total_g, imu.pitch, imu.roll);
    lv_label_set_text(info_line[6], buf);

    snprintf(buf, sizeof(buf), "Batt: %.1fV  Heap: %luK",
             s_battery_v, (unsigned long)(esp_get_free_heap_size() / 1024));
    lv_label_set_text(info_line[7], buf);
}

/* ──────────────────────────────────────────────────────────────────────────
 * Tasks
 * ──────────────────────────────────────────────────────────────────────── */

static void ui_update_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI update task started (%d ms interval)", UI_UPDATE_INTERVAL_MS);
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        if (display_lvgl_lock(50)) {
            /* Detect mode change and apply layout switch */
            if (current_mode != prev_mode) {
                apply_mode_layout(current_mode);
                update_mode_indicator(current_mode);
                prev_mode = current_mode;
            }

            switch (current_mode) {
                case POD_DISPLAY_MODE_OIL:    update_oil_mode();    break;
                case POD_DISPLAY_MODE_WATER:  update_water_mode();  break;
                case POD_DISPLAY_MODE_AFR:    update_afr_mode();    break;
                case POD_DISPLAY_MODE_BOOST:  update_boost_mode();  break;
                case POD_DISPLAY_MODE_GFORCE: update_gforce_mode(); break;
                case POD_DISPLAY_MODE_DEBUG:  update_debug_mode();  break;
                default: break;
            }
            display_lvgl_unlock();
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(UI_UPDATE_INTERVAL_MS));
    }
}

static void ui_render_task(void *pvParameters)
{
    ESP_LOGI(TAG, "UI render task started");
    uint32_t keepalive_counter = 0;

    while (1) {
        if (display_lvgl_lock(10)) {
            if (++keepalive_counter >= 500) {
                keepalive_counter = 0;
                if (screen_main) {
                    lv_obj_invalidate(screen_main);
                }
            }
            lv_timer_handler();
            display_lvgl_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
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
    memset(s_mode_has_data, 0, sizeof(s_mode_has_data));

    ESP_LOGI(TAG, "Creating UI layout for 466x466 round AMOLED");
    ESP_LOGI(TAG, "Modes: OIL, WATER, AFR, BOOST, GFORCE, DEBUG");

    if (display_lvgl_lock(1000)) {
        create_screen_layout();
        lv_scr_load(screen_main);
        lv_obj_add_event_cb(screen_main, touch_swipe_handler, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(screen_main, touch_swipe_handler, LV_EVENT_RELEASED, NULL);
        display_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "Failed to acquire LVGL lock for UI init");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UI layout created — starting in OIL TEMP mode");
    return ESP_OK;
}

esp_err_t ui_manager_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        ui_update_task, "ui_update", 4096, NULL,
        4, &ui_update_task_handle, 0);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI update task");
        return ESP_FAIL;
    }

    ret = xTaskCreatePinnedToCore(
        ui_render_task, "ui_render", 8192, NULL,
        5, &ui_render_task_handle, 1);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create UI render task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "UI tasks running (update=core0, render=core1)");
    return ESP_OK;
}

void ui_manager_update_value(uint16_t data_point_id, float value)
{
    /* Cache incoming data point values from center */
    switch (data_point_id) {
        case OPENDASH_DP_OIL_TEMP:       s_oil_temp    = value; break;
        case OPENDASH_DP_OIL_PRESSURE:   s_oil_psi     = value; break;
        case OPENDASH_DP_COOLANT_TEMP:   s_coolant     = value; break;
        case OPENDASH_DP_AFR:            s_afr         = value; break;
        case OPENDASH_DP_BOOST_PRESSURE: s_boost       = value; break;
        case OPENDASH_DP_RPM:            s_rpm         = value; break;
        case OPENDASH_DP_INTAKE_TEMP:    s_intake_temp = value; break;
        case OPENDASH_DP_BATTERY_VOLTAGE: s_battery_v  = value; break;
        case OPENDASH_DP_LAMBDA:         s_lambda      = value; break;
        case OPENDASH_DP_EGT:            s_egt_max     = value; break;
        default:
            ESP_LOGD(TAG, "Unmapped DP 0x%04X = %.2f", data_point_id, value);
            break;
    }
}

esp_err_t ui_manager_next_screen(void)
{
    current_mode = (current_mode + 1) % POD_DISPLAY_MODE_COUNT;
    ESP_LOGI(TAG, "Display mode → %d", current_mode);
    return ESP_OK;
}

esp_err_t ui_manager_set_display_mode(pod_display_mode_t mode)
{
    if (mode >= POD_DISPLAY_MODE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    current_mode = mode;
    return ESP_OK;
}

uint8_t ui_manager_get_current_screen(void)
{
    return (uint8_t)current_mode;
}
