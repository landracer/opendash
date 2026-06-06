# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash GPS / Telemetry Unit — Main Component CMakeLists.txt
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "main.c"
        "display_init.c"
        "ui_manager.c"
        "gps_handler.c"
        "imu_handler.c"
        "sd_logger.c"
        "../../common/src/opendash_bt_ota.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        common
        lvgl__lvgl
        driver
        esp_lcd
        esp_timer
        esp_driver_gpio
        esp_driver_spi
        esp_driver_i2c
        esp_driver_uart
        esp_wifi
        esp_event
        app_update
        nvs_flash
        bt
        fatfs
        sdmmc
)
