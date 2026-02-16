# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Common Component — CMakeLists.txt
#
# This is an ESP-IDF component that provides shared code used by all three
# display projects (center, left-right, gps). It is included via the
# EXTRA_COMPONENT_DIRS mechanism in each project's top-level CMakeLists.txt.
#
# IMPORTANT: Any changes to this component automatically affect ALL displays.
# This includes:
#   - Font system (fonts/, convert_fonts.py, generated fonts)
#   - Shared headers (include/opendash_fonts.h, etc.)
#   - Shared implementations (src/*.c)
#
# @see DISPLAY_SYNCHRONIZATION.md for details on how displays stay synchronized
# @see ESP-IDF Build System:
#      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-guides/build-system.html
# ─────────────────────────────────────────────────────────────────────────────

# ─────────────────────────────────────────────────────────────────────────────
# Step 1: Convert TrueType fonts to LVGL C format (pre-build step)
#
# This runs once during CMake configuration for ANY display project.
# Generated fonts are shared by ALL displays (center, left-right, gps).
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
# Step 1b: Convert images (JPG/PNG) to LVGL C format (pre-build step)
#
# This runs once during CMake configuration for ANY display project.
# Generated images are shared by ALL displays (center, left-right, gps).
# Images are automatically scaled to fit display resolution.
# ─────────────────────────────────────────────────────────────────────────────

if(Python3_FOUND)
    # Run image converter script before build
    execute_process(
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/images/convert_images.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/images
        RESULT_VARIABLE IMAGE_CONV_RESULT
        OUTPUT_VARIABLE IMAGE_CONV_OUTPUT
        ERROR_VARIABLE IMAGE_CONV_ERROR
    )
    
    # Display image converter output
    if(IMAGE_CONV_OUTPUT)
        message(STATUS "Image Converter Output:")
        message(STATUS "${IMAGE_CONV_OUTPUT}")
    endif()
    
    # Check for errors (note: no images is not an error)
    if(NOT IMAGE_CONV_RESULT EQUAL 0)
        message(WARNING "Image conversion failed: ${IMAGE_CONV_ERROR}")
        message(WARNING "Continuing with build - images may not be available")
    endif()
else()
    message(WARNING "Python3 not found - skipping image conversion")
    message(WARNING "To use custom images, install Python3 and run: common/images/convert_images.py")
endif()

# ─────────────────────────────────────────────────────────────────────────────
# Step 2: Collect generated font and image files
#
# All *.c files in fonts/generated/ and images/generated/ are automatically included.
# This includes both the converted fonts/images and any future additions.
# ─────────────────────────────────────────────────────────────────────────────

file(GLOB FONT_SOURCES "${CMAKE_CURRENT_LIST_DIR}/fonts/generated/*.c")
file(GLOB IMAGE_SOURCES "${CMAKE_CURRENT_LIST_DIR}/images/generated/*.c")

# ─────────────────────────────────────────────────────────────────────────────
# Step 3: Register component with all sources
#
# This component is automatically linked to ALL display projects via
# EXTRA_COMPONENT_DIRS. Any file added here becomes available to all displays.
# ─────────────────────────────────────────────────────────────────────────────

idf_component_register(
    SRCS
        "src/opendash_i2c_protocol.c"
        "src/opendash_data_model.c"
        "src/opendash_checklist.c"
        "src/opendash_display_config.c"
        ${FONT_SOURCES}
        ${IMAGE_SOURCES}
    INCLUDE_DIRS
        "include"
        "fonts/generated"
        "images/generated"
    REQUIRES
        nvs_flash
        lvgl__lvgl
)

# ─────────────────────────────────────────────────────────────────────────────
# Step 4: Add compile definitions for LVGL compatibility
#
# Generated font files use conditional includes. With ESP-IDF managed components,
# the include path is set up to find "lvgl.h" directly (not "lvgl/lvgl.h").
# This define tells generated fonts to use the simple include path.
# ─────────────────────────────────────────────────────────────────────────────

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
