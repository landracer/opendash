# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Center Display — Main Component CMakeLists.txt
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "main.c"
        "display_init.c"
        "ui_manager.c"
    INCLUDE_DIRS
        "."
)
