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

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Convert TrueType fonts to LVGL C format (pre-build step)
# ─────────────────────────────────────────────────────────────────────────────

# Check if Python3 is available
find_package(Python3 COMPONENTS Interpreter)

if(Python3_FOUND)
    # Run font converter script before build
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/fonts/convert_fonts.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/fonts
        RESULT_VARIABLE FONT_CONV_RESULT
        OUTPUT_VARIABLE FONT_CONV_OUTPUT
        ERROR_VARIABLE FONT_CONV_ERROR
    )
    
    # Display font converter output
    if(FONT_CONV_OUTPUT)
        message(STATUS "Font Converter Output:")
        message(STATUS "${FONT_CONV_OUTPUT}")
    endif()
    
    # Check for errors
    if(NOT FONT_CONV_RESULT EQUAL 0)
        message(WARNING "Font conversion failed or skipped: ${FONT_CONV_ERROR}")
        message(WARNING "Continuing with build - fonts may not be available")
    endif()
else()
    message(WARNING "Python3 not found - skipping font conversion")
    message(WARNING "To use custom fonts, install Python3 and run: common/fonts/convert_fonts.py")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: Collect generated font files
# ─────────────────────────────────────────────────────────────────────────────

file(GLOB FONT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/fonts/generated/*.c")

# ─────────────────────────────────────────────────────────────────────────────
# Step 3: Register component with all sources
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "src/opendash_i2c_protocol.c"
        "src/opendash_data_model.c"
        "src/opendash_checklist.c"
        "src/opendash_display_config.c"
        ${FONT_SOURCES}
    INCLUDE_DIRS
        "include"
        "fonts/generated"
    REQUIRES
        nvs_flash
)
