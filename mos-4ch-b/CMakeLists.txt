# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash 4-Channel MOS FET Controller B — CMakeLists.txt
#
# ESP32-based 4-channel MOS FET module with PWM support.
# Headless controller — receives commands via ESP-NOW from center.
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opendash_mos_4ch_b)
