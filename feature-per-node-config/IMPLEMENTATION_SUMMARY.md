<!-- Licensed under Sovereign Individual License v1.0 — see LICENSE file -->
# Per-Node Display Configuration Feature Implementation

## Overview

This directory contains a staging implementation of the per-node display configuration feature as specified in the `PER_NODE_DISPLAY_CONFIG_SPEC.md` document. The implementation provides the core interfaces and data structures needed for users to configure which data points appear on each node's display.

## Files and Components

### Core Headers
- `opendash_dp_catalog.h` - Data point catalog with metadata
- `opendash_layout.h` - Layout structure definition
- `opendash_layout_store.h` - NVS storage helpers

### Core Implementations
- `opendash_dp_catalog.c` - Catalog implementation with lookup functionality
- `opendash_layout_store.c` - NVS storage implementation
- `test_feature.c` - Test program to verify functionality

## Implementation Details

### Data Point Catalog (`opendash_dp_catalog.h`)

The catalog defines all available data points with metadata:
- `dp_id`: Unique identifier for each data point
- `short_name`: Human-readable name (≤10 chars)
- `units`: Physical units
- `default_min`/`default_max`: Sensible default ranges for arc gauges
- `category`: Categorization for organization
- `decimals`: Number of decimal places for display

### Layout Structure (`opendash_layout.h`)

The layout structure defines how data points are arranged on displays:
- `version`: Version of the layout structure (0x01)
- `mode`: Display mode (0-7)
- `slot_count`: Number of slots (1-16)
- `arc_dp_id`: DP ID for arc gauge (0 if no arc)
- `arc_min`/`arc_max`: Minimum and maximum values for arc
- `slot_dp_ids`: Array of DP IDs for each slot

### NVS Storage (`opendash_layout_store.h` and `opendash_layout_store.c`)

The NVS storage implementation provides:
- `opendash_layout_load()` - Load a layout from NVS
- `opendash_layout_save()` - Save a layout to NVS
- `opendash_layout_load_or_default()` - Load or use defaults
- `opendash_layout_factory_reset()` - Reset all layouts to defaults

## Key Features Implemented

1. **Data Point Catalog**: 
   - Comprehensive catalog of 100+ data points across 10 categories
   - Lookup functionality by DP ID
   - Metadata for each data point

2. **Layout Structure**:
   - Well-defined layout structure for screen configuration
   - Support for arc gauges and multiple slots
   - Versioning for future compatibility

3. **NVS Storage**:
   - Complete NVS storage implementation
   - Persistent storage using NVS namespace `od_layout`
   - Key format: `m<mode>` (e.g., `m0`, `m1`, ...)
   - Version validation and error handling

4. **Test Coverage**:
   - Verification that catalog lookup works correctly
   - All data points are properly defined
   - Layout structure is correctly defined
   - NVS storage functions work correctly
   - The system can be compiled and executed successfully

The test output confirms that:
- RPM data point is found with correct units
- Coolant data point is found with correct units
- Catalog contains 100+ data points
- First 3 data points are correctly identified
- Layout structure is properly defined
- NVS storage functions work correctly