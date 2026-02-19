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
        "i2c_node.c"
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
)
