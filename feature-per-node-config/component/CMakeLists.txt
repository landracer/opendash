# Licensed under Sovereign Individual License v1.0 — see LICENSE file
# Component CMakeLists.txt for per-node display configuration feature

idf_component_register(
    SRCS "src/opendash_dp_catalog.c" "src/opendash_layout_store.c"
    INCLUDE_DIRS "include"
    REQUIRES
)