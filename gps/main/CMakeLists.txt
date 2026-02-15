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
    INCLUDE_DIRS
        "."
)
