# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Center Display — CMakeLists.txt
#
# ESP-IDF project configuration for the Center Display Unit (4.3" LCD).
# This is the I2C master and main coordinator for the OpenDash system.
#
# Hardware: Waveshare ESP32-S3-Touch-LCD-4.3
# Resolution: 800×480 IPS LCD
# Touch: Capacitive touch controller
#
# @see ESP-IDF Build System:
#      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-guides/build-system.html
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)

# Include the common component directory (shared code across all displays)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opendash_center)
