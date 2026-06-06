# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash 8-Channel Relay A — Main Component CMakeLists.txt
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "main.c"
        "../../common/src/opendash_bt_ota.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        common
        driver
        esp_timer
        esp_driver_gpio
        esp_wifi
        esp_event
        app_update
        nvs_flash
        bt
)
