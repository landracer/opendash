# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash 4-Channel HD Relay Controller — CMakeLists.txt
#
# ESP32-based 4-channel high-amp relay module for controlling:
# fans, pumps, solenoids, and other high-current devices.
#
# Hardware: ESP32 4-CH HD Relay Module (LCTech)
# http://www.chinalctech.com/cpzx/Programmer/Relay_Module/788.html
#
# No display — headless controller, receives commands via ESP-NOW from center.
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opendash_relay_4ch_hd)
