# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# ─────────────────────────────────────────────────────────────────────────
# OpenDash host test harness — standalone CMake project (NOT part of any
# firmware build). Compiles the pure-logic modules from common/src against
# thin fakes for the IDF APIs they touch (NVS, log macros, virtual clock,
# transparent mutex) and runs them with ctest on the dev machine or CI.
#
#   cmake -S test -B test/build && cmake --build test/build -j && ctest
#   --test-dir test/build --output-on-failure
# ─────────────────────────────────────────────────────────────────────────
cmake_minimum_required(VERSION 3.16)
project(opendash_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Werror -Wno-unused-parameter)

# Vendored Unity (ThrowTheSwitch) — see unity/LICENSE.txt
add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC ${CMAKE_CURRENT_LIST_DIR}/unity)

# Code under test + stub layer. Real sources, compiled verbatim.
add_library(od_core STATIC
    stubs/stubs.c
    ../common/src/opendash_protocol.c
    ../common/src/opendash_data_model.c
    ../common/src/node_health.c
    ../common/src/opendash_parachute.c
    ../common/src/opendash_roster.c
)
target_include_directories(od_core PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}/../common/include
    ${CMAKE_CURRENT_LIST_DIR}/stubs
)

enable_testing()

foreach(suite protocol data_model node_health parachute roster)
    add_executable(test_${suite} test_${suite}.c)
    target_link_libraries(test_${suite} PRIVATE od_core unity m)
    add_test(NAME ${suite} COMMAND test_${suite})
endforeach()
