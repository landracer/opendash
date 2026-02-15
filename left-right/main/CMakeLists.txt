# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Left/Right Gauges — Main Component CMakeLists.txt
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "main.c"
        "display_init.c"
        "ui_manager.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        lvgl__lvgl
        espressif__esp_lvgl_port
)
