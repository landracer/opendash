# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Left Gauge — Main Component CMakeLists.txt
#
# Hardware: Waveshare ESP32-S3-Touch-LCD-2.8C (480×480 Round IPS LCD)
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "main.c"
        "display_init.c"
        "ui_manager.c"
        "../../common/src/opendash_bt_ota.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        common
        lvgl__lvgl
        driver
        esp_driver_ledc
        esp_driver_i2c
        esp_driver_spi
        esp_lcd
        esp_driver_gpio
        esp_timer
        esp_wifi
        esp_event
        app_update
        nvs_flash
        bt
)
