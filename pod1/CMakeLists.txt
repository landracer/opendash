# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash Pod 1 — CMakeLists.txt
#
# ESP-IDF project configuration for Pod 1 display unit (1.75" AMOLED).
# Same hardware as GPS unit (Waveshare ESP32-S3-Touch-AMOLED-1.75) but
# WITHOUT GPS/IMU — receives all data from Center via ESP-NOW.
#
# Displays: OIL TEMP, WATER TEMP, AFR, BOOST, DEBUG screens.
#
# Hardware: Waveshare ESP32-S3-Touch-AMOLED-1.75
# Resolution: 466×466 Round AMOLED (CO5300 QSPI)
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)

# Include the common component directory (shared code across all displays)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opendash_pod1)
