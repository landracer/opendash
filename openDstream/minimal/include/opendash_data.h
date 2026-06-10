/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
/**
 * @file opendash_data.h
 * @brief Minimal OpenDash data point definitions for openDstream
 *
 * Standalone header — no dependencies on LVGL or common component.
 * Defines the data point IDs used in ESP-NOW batched telemetry.
 */

#ifndef OPENDASH_DATA_H
#define OPENDASH_DATA_H

#include <stdint.h>

/* Engine / OBD2 data points: 0x0100 – 0x01FF */
#define OPENDASH_DP_RPM              0x0100
#define OPENDASH_DP_VEHICLE_SPEED    0x0101
#define OPENDASH_DP_COOLANT_TEMP     0x0102
#define OPENDASH_DP_INTAKE_TEMP      0x0103
#define OPENDASH_DP_ENGINE_LOAD      0x0104
#define OPENDASH_DP_THROTTLE_POS     0x0105
#define OPENDASH_DP_BOOST_PRESSURE   0x0106
#define OPENDASH_DP_OIL_TEMP         0x0107
#define OPENDASH_DP_OIL_PRESSURE     0x0108
#define OPENDASH_DP_FUEL_PRESSURE    0x0109
#define OPENDASH_DP_AFR              0x010A
#define OPENDASH_DP_LAMBDA           0x010B
#define OPENDASH_DP_EGT              0x010C
#define OPENDASH_DP_BATTERY_VOLTAGE  0x010D
#define OPENDASH_DP_TIMING_ADVANCE   0x010E
#define OPENDASH_DP_MAF_RATE         0x010F
#define OPENDASH_DP_FUEL_LEVEL       0x0110
#define OPENDASH_DP_EGT1             0x0112
#define OPENDASH_DP_EGT2             0x0113
#define OPENDASH_DP_EGT3             0x0114
#define OPENDASH_DP_EGT4             0x0115
#define OPENDASH_DP_EGT5             0x0118
#define OPENDASH_DP_EGT6             0x0119
#define OPENDASH_DP_EGT7             0x011A
#define OPENDASH_DP_EGT8             0x011B

/* GPS / Navigation: 0x0200 – 0x02FF */
#define OPENDASH_DP_GPS_SPEED        0x0200
#define OPENDASH_DP_GPS_HEADING      0x0201
#define OPENDASH_DP_LATITUDE         0x0202
#define OPENDASH_DP_LONGITUDE        0x0203
#define OPENDASH_DP_ALTITUDE         0x0204

/* Battery / BMS: 0x0400 – 0x04FF */
#define OPENDASH_DP_PACK_VOLTAGE     0x0400
#define OPENDASH_DP_PACK_CURRENT     0x0401
#define OPENDASH_DP_SOC              0x0402

/* System: 0x0500 – 0x05FF */
#define OPENDASH_DP_CPU_TEMP         0x0500
#define OPENDASH_DP_UPTIME           0x0503

#endif /* OPENDASH_DATA_H */
