# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Common Component — CMakeLists.txt
#
# This is an ESP-IDF component that provides shared code used by all three
# display projects (center, left-right, gps). It is included via the
# EXTRA_COMPONENT_DIRS mechanism in each project's top-level CMakeLists.txt.
#
# @see ESP-IDF Build System:
#      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-guides/build-system.html
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "src/opendash_i2c_protocol.c"
        "src/opendash_data_model.c"
        "src/opendash_checklist.c"
    INCLUDE_DIRS
        "include"
)
