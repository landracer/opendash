# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# openDstream — Main Component CMakeLists.txt
# Headless ESP-NOW relay — no common, no LVGL, no display
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_wifi nvs_flash esp_driver_uart esp_driver_gpio
)
