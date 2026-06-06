# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Center Display — Main Component CMakeLists.txt
# ──────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "main.c"
        "display_init.c"
        "ui_manager.c"
        "espnow_master.c"
        "layout_editor.c"
        "system_config.c"
        "boost_client.c"
        "boost_config_ui.c"
        "lv_mem_psram.c"
    # opendash_obd_config.c is already provided by the common component
    INCLUDE_DIRS
        "."
    REQUIRES
        common
        lvgl__lvgl
        esp_lcd
        esp_timer
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2c
        freertos
    WHOLE_ARCHIVE
)
