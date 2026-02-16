/**
 * @file opendash_common.h
 * @brief OpenDash Common Definitions
 *
 * Master header for the OpenDash shared library. Include this single header
 * to access all common definitions, data models, and protocol types.
 *
 * @note This file is shared across all OpenDash display nodes (Center,
 *       Left/Right, GPS). Keep it hardware-agnostic.
 *
 * @see ESP-IDF API Reference:
 *      https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32s3/api-reference/index.html
 */

#ifndef OPENDASH_COMMON_H
#define OPENDASH_COMMON_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ────────────────────────────────────────────────────────────────────────────
 * Version Information
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief OpenDash firmware major version. */
#define OPENDASH_VERSION_MAJOR  0

/** @brief OpenDash firmware minor version. */
#define OPENDASH_VERSION_MINOR  1

/** @brief OpenDash firmware patch version. */
#define OPENDASH_VERSION_PATCH  0

/** @brief Version string for display and logging. */
#define OPENDASH_VERSION_STR    "0.1.0"

/* ────────────────────────────────────────────────────────────────────────────
 * Node Identification
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief Enumeration of all OpenDash node types.
 *
 * Each physical device in the system is assigned a unique node type. This is
 * used for I2C addressing, logging, and configuration management.
 */
typedef enum {
    OPENDASH_NODE_CENTER    = 0,    /**< Center display (4.3" LCD, I2C master) */
    OPENDASH_NODE_LEFT      = 1,    /**< Left gauge pod (2.8" round LCD) */
    OPENDASH_NODE_RIGHT     = 2,    /**< Right gauge pod (2.8" round LCD) */
    OPENDASH_NODE_GPS       = 3,    /**< GPS/Telemetry unit (1.75" AMOLED) */
    OPENDASH_NODE_BMS       = 4,    /**< External BMS node (rAtTrax) */
    OPENDASH_NODE_COUNT     = 5     /**< Total number of node types */
} opendash_node_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Display Screen Sections
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum number of configurable screen sections per display. */
#define OPENDASH_MAX_SECTIONS   8

/**
 * @brief Screen section configuration.
 *
 * Each display is divided into sections that can independently show different
 * data points. The user can reconfigure which data point appears in each
 * section without modifying code.
 */
typedef struct {
    uint16_t data_point_id;         /**< Data point ID to display (see data-points.md) */
    uint8_t  display_mode;          /**< 0=numeric, 1=arc/gauge, 2=bar, 3=graph */
    uint8_t  font_size;             /**< Font size index (0=small, 1=medium, 2=large) */
} opendash_section_config_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Alarm / Warning System
 * ──────────────────────────────────────────────────────────────────────────── */

/** @brief Maximum number of configurable alarms. */
#define OPENDASH_MAX_ALARMS     16

/** @brief Alarm severity levels. */
typedef enum {
    OPENDASH_ALARM_NONE     = 0,    /**< No alarm active */
    OPENDASH_ALARM_INFO     = 1,    /**< Informational (blue indicator) */
    OPENDASH_ALARM_WARNING  = 2,    /**< Warning (yellow indicator) */
    OPENDASH_ALARM_CRITICAL = 3     /**< Critical (red indicator + flash) */
} opendash_alarm_severity_t;

/**
 * @brief Alarm configuration for a data point.
 */
typedef struct {
    uint16_t data_point_id;         /**< Data point to monitor */
    float    low_threshold;         /**< Trigger if value drops below this */
    float    high_threshold;        /**< Trigger if value exceeds this */
    uint8_t  severity;              /**< opendash_alarm_severity_t */
    bool     enabled;               /**< Whether this alarm is active */
} opendash_alarm_config_t;

/* ────────────────────────────────────────────────────────────────────────────
 * Common Return Codes
 * ──────────────────────────────────────────────────────────────────────────── */

/**
 * @brief OpenDash return codes.
 *
 * Functions in the common library return these codes. They are designed to
 * complement ESP-IDF's esp_err_t where OpenDash-specific errors are needed.
 */
typedef enum {
    OPENDASH_OK              = 0,    /**< Success */
    OPENDASH_ERR_INVALID_ARG = -1,   /**< Invalid argument passed */
    OPENDASH_ERR_NO_MEM      = -2,   /**< Out of memory */
    OPENDASH_ERR_TIMEOUT     = -3,   /**< Operation timed out */
    OPENDASH_ERR_NOT_FOUND   = -4,   /**< Requested item not found */
    OPENDASH_ERR_BUSY        = -5,   /**< Resource is busy */
    OPENDASH_ERR_CHECKSUM    = -6,   /**< Checksum validation failed */
    OPENDASH_ERR_COMM        = -7,   /**< Communication error */
    OPENDASH_ERR_GENERAL     = -99   /**< General / unspecified error */
} opendash_err_t;

#ifdef __cplusplus
}
#endif

#endif /* OPENDASH_COMMON_H */
