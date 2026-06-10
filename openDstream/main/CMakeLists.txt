# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# openDstream — Main Component CMakeLists.txt
# Headless ESP-NOW relay — no common, no LVGL, no display
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_usb_serial_jtag esp_driver_gpio
    REQUIRES esp_wifi nvs_flash
)
