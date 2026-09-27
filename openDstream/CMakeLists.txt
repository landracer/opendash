# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────────
# OpenDash openDstream — ESP-NOW to USB Bridge Node
#
# ESP-IDF project configuration for the openDstream bridge node.
# This is a headless ESP32-WROOM-32 that receives ESP-NOW data from the
# OpenDash network and forwards parsed data over USB CDC-ACM to the host PC
# (multidisplay-app Qt application).
#
# Hardware: ESP32-WROOM-32 (ESP32-DevKitC form factor)
# Interface: USB CDC-ACM (virtual serial port @ /dev/ttyACM*)
# Role: ESP-NOW Slave → USB Bridge to multidisplay-app
#
# @see docs/data-ESPNOW-usbnode.md for the data format specification
# ─────────────────────────────────────────────────────────────────────────────

cmake_minimum_required(VERSION 3.16)

# Include the common component directory (shared code across all displays)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../common")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(opendash_opendstream)
