/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
#ifndef OPEN_DASH_DP_CATALOG_H
#define OPEN_DASH_DP_CATALOG_H

#include <stdint.h>
#include <stddef.h>

typedef enum {
    OPENDASH_DP_CAT_ENGINE = 0,    // RPM, MAP, MAF, throttle…
    OPENDASH_DP_CAT_TEMP,          // coolant, oil, intake, EGT…
    OPENDASH_DP_CAT_PRESSURE,      // boost, oil, fuel…
    OPENDASH_DP_CAT_FUEL,          // AFR, lambda, level, consumption
    OPENDASH_DP_CAT_DRIVETRAIN,    // gear, vehicle speed, trans temp
    OPENDASH_DP_CAT_GPS,           // speed, lat, lon, alt, fix, hdop
    OPENDASH_DP_CAT_BMS,           // pack volts, current, SOC, cell mV
    OPENDASH_DP_CAT_VESC,          // motor temp, fet temp, duty, current
    OPENDASH_DP_CAT_OBD,           // OBD-II PIDs not covered above
    OPENDASH_DP_CAT_SYSTEM,        // battery V, ambient T, free heap
    OPENDASH_DP_CAT_COUNT
} opendash_dp_category_t;

typedef struct {
    uint16_t  dp_id;            // OPENDASH_DP_* constant
    const char *short_name;     // "RPM", "EGT 1", "Coolant"  (≤10 chars)
    const char *units;          // "rpm", "°C", "kPa"
    float     default_min;      // sensible arc minimum
    float     default_max;      // sensible arc maximum
    uint8_t   category;         // opendash_dp_category_t
    uint8_t   decimals;         // display decimals (0,1,2,3)
} opendash_dp_info_t;

extern const opendash_dp_info_t opendash_dp_catalog[];
extern const size_t             opendash_dp_catalog_count;

const opendash_dp_info_t *opendash_dp_lookup(uint16_t dp_id);   // O(log n)

#endif // OPEN_DASH_DP_CATALOG_H