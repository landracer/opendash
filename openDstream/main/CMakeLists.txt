# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash openDstream — Main Component CMakeLists.txt
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "main.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        common
        esp_driver_uart
        esp_driver_gpio
        esp_timer
        nvs_flash
)
