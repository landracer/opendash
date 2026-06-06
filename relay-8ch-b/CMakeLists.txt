# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash 8-Channel Relay Controller B — CMakeLists.txt
#
# ESP32-based 8-channel relay module.
# Headless controller — receives commands via ESP-NOW from center.
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opendash_relay_8ch_b)
