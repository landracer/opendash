# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Left/Right Gauges — CMakeLists.txt
#
# ESP-IDF project configuration for the Left and Right Gauge Pods (2.8" Round LCD).
# These are I2C slaves that receive display data from the Center unit.
#
# Hardware: Waveshare ESP32-S3-LCD-2.8C
# Resolution: 480×480 Round IPS LCD
#
# @see ../DISPLAY_SYNCHRONIZATION.md for how this project stays synced with others
# @see ESP-IDF Build System:
#      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-guides/build-system.html
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)

# Include the common component directory (shared code across all displays)
# This provides: fonts, protocols, data models, and shared utilities
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opendash_leftright)
