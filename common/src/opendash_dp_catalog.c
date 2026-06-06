/* Licensed under Sovereign Individual License v1.0 — see LICENSE file */
#include "opendash_dp_catalog.h"
#include <string.h>
#include <stdlib.h>

// Complete catalog with all actual DP IDs from opendash_data_model.h
const opendash_dp_info_t opendash_dp_catalog[] = {
    // Engine / OBD2 data points: 0x0100 – 0x01FF
    {0x0100, "RPM", "rpm", 0, 8000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0101, "Speed", "km/h", 0, 200, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0102, "Coolant", "°C", -40, 150, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0103, "Intake", "°C", -40, 150, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0104, "Engine Load", "%", 0, 100, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0105, "Throttle", "%", 0, 100, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0106, "Boost", "kPa", -100, 200, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0107, "Oil Temp", "°C", -40, 200, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0108, "Oil Press", "kPa", 0, 100, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0109, "Fuel Press", "kPa", 0, 100, OPENDASH_DP_CAT_ENGINE, 0},
    {0x010A, "AFR", "AFR", 0, 20, OPENDASH_DP_CAT_ENGINE, 1},
    {0x010B, "Lambda", "λ", 0, 2, OPENDASH_DP_CAT_ENGINE, 2},
    {0x010C, "EGT", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x010D, "Battery V", "V", 0, 20, OPENDASH_DP_CAT_ENGINE, 2},
    {0x010E, "Timing Adv", "°", -30, 60, OPENDASH_DP_CAT_ENGINE, 0},
    {0x010F, "MAF", "g/s", 0, 500, OPENDASH_DP_CAT_ENGINE, 1},
    {0x0110, "Fuel Level", "%", 0, 100, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0111, "Trans Temp", "°C", -40, 200, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0112, "EGT 1", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0113, "EGT 2", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0114, "EGT 3", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0115, "EGT 4", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0116, "O2 Lambda", "λ", 0, 2, OPENDASH_DP_CAT_ENGINE, 2},
    {0x0117, "MD RPM", "rpm", 0, 8000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0118, "EGT 5", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x0119, "EGT 6", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x011A, "EGT 7", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x011B, "EGT 8", "°C", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    {0x011C, "OBD2 Flags", "", 0, 3, OPENDASH_DP_CAT_ENGINE, 0},
    {0x011D, "MIL On", "", 0, 1, OPENDASH_DP_CAT_ENGINE, 0},
    {0x011E, "DTC Count", "", 0, 1000, OPENDASH_DP_CAT_ENGINE, 0},
    
    // GPS / Navigation data points: 0x0200 – 0x02FF
    {0x0200, "GPS Speed", "km/h", 0, 200, OPENDASH_DP_CAT_GPS, 0},
    {0x0201, "Heading", "°", 0, 360, OPENDASH_DP_CAT_GPS, 0},
    {0x0202, "Lat", "°", -90, 90, OPENDASH_DP_CAT_GPS, 6},
    {0x0203, "Lon", "°", -180, 180, OPENDASH_DP_CAT_GPS, 6},
    {0x0204, "Alt", "m", -1000, 10000, OPENDASH_DP_CAT_GPS, 0},
    {0x0205, "Sat Count", "", 0, 20, OPENDASH_DP_CAT_GPS, 0},
    {0x0206, "HDOP", "", 0, 20, OPENDASH_DP_CAT_GPS, 1},
    {0x0207, "Lap Number", "", 0, 1000, OPENDASH_DP_CAT_GPS, 0},
    {0x0208, "Lap Time", "ms", 0, 100000, OPENDASH_DP_CAT_GPS, 0},
    {0x0209, "Best Lap", "ms", 0, 100000, OPENDASH_DP_CAT_GPS, 0},
    {0x020A, "Lap Delta", "ms", -10000, 10000, OPENDASH_DP_CAT_GPS, 0},
    {0x020B, "Sector Time", "ms", 0, 10000, OPENDASH_DP_CAT_GPS, 0},
    {0x020C, "Predictive Lap", "ms", 0, 100000, OPENDASH_DP_CAT_GPS, 0},
    {0x020D, "GPS Fix", "", 0, 3, OPENDASH_DP_CAT_GPS, 0},
    
    // IMU / Motion data points: 0x0300 – 0x03FF
    {0x0300, "G-force Lat", "g", -5, 5, OPENDASH_DP_CAT_DRIVETRAIN, 1},
    {0x0301, "G-force Long", "g", -5, 5, OPENDASH_DP_CAT_DRIVETRAIN, 1},
    {0x0302, "G-force Vert", "g", -5, 5, OPENDASH_DP_CAT_DRIVETRAIN, 1},
    {0x0303, "Yaw Rate", "°/s", -100, 100, OPENDASH_DP_CAT_DRIVETRAIN, 0},
    {0x0304, "Pitch Rate", "°/s", -100, 100, OPENDASH_DP_CAT_DRIVETRAIN, 0},
    {0x0305, "Roll Rate", "°/s", -100, 100, OPENDASH_DP_CAT_DRIVETRAIN, 0},
    {0x0306, "Pitch Angle", "°", -90, 90, OPENDASH_DP_CAT_DRIVETRAIN, 0},
    {0x0307, "Roll Angle", "°", -180, 180, OPENDASH_DP_CAT_DRIVETRAIN, 0},
    
    // Battery / BMS data points: 0x0400 – 0x04FF
    {0x0400, "Pack Volts", "V", 0, 100, OPENDASH_DP_CAT_BMS, 2},
    {0x0401, "Pack Current", "A", -100, 100, OPENDASH_DP_CAT_BMS, 1},
    {0x0402, "SOC", "%", 0, 100, OPENDASH_DP_CAT_BMS, 0},
    {0x0403, "Cell Min", "V", 2, 4.5, OPENDASH_DP_CAT_BMS, 3},
    {0x0404, "Cell Max", "V", 2, 4.5, OPENDASH_DP_CAT_BMS, 3},
    {0x0405, "Cell Delta", "mV", 0, 1000, OPENDASH_DP_CAT_BMS, 0},
    {0x0406, "BMS Temp Max", "°C", -40, 100, OPENDASH_DP_CAT_BMS, 0},
    {0x0407, "Pack Power", "W", -10000, 10000, OPENDASH_DP_CAT_BMS, 0},
    {0x0408, "Energy Used", "Wh", 0, 10000, OPENDASH_DP_CAT_BMS, 0},
    {0x0409, "SOH", "%", 0, 100, OPENDASH_DP_CAT_BMS, 0},
    {0x040A, "BMS Temp IC", "°C", -40, 100, OPENDASH_DP_CAT_BMS, 0},
    {0x040B, "BMS Balance", "", 0, 1, OPENDASH_DP_CAT_BMS, 0},
    {0x040C, "BMS Charging", "", 0, 1, OPENDASH_DP_CAT_BMS, 0},
    {0x040D, "Energy Charged", "Wh", 0, 10000, OPENDASH_DP_CAT_BMS, 0},
    {0x0410, "Cell V Base", "V", 2, 4.5, OPENDASH_DP_CAT_BMS, 3},
    
    // System data points: 0x0500 – 0x05FF
    {0x0500, "CPU Temp", "°C", -40, 100, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0501, "Free Heap", "KB", 0, 10000, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0502, "WiFi RSSI", "dBm", -100, 0, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0503, "Uptime", "s", 0, 1000000, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0504, "SD Free", "MB", 0, 1000, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0505, "Log Session", "", 0, 1000, OPENDASH_DP_CAT_SYSTEM, 0},
    
    // VESC motor controller data points: 0x0600 – 0x06FF
    {0x0600, "VESC ERPM", "rpm", 0, 10000, OPENDASH_DP_CAT_VESC, 0},
    {0x0601, "VESC Current", "A", 0, 100, OPENDASH_DP_CAT_VESC, 1},
    {0x0602, "VESC Duty", "%", 0, 100, OPENDASH_DP_CAT_VESC, 0},
    {0x0603, "VESC AH", "Ah", 0, 100, OPENDASH_DP_CAT_VESC, 2},
    {0x0604, "VESC AH Charged", "Ah", 0, 100, OPENDASH_DP_CAT_VESC, 2},
    {0x0605, "VESC WH", "Wh", 0, 10000, OPENDASH_DP_CAT_VESC, 0},
    {0x0606, "VESC WH Charged", "Wh", 0, 10000, OPENDASH_DP_CAT_VESC, 0},
    {0x0607, "VESC Temp FET", "°C", -40, 150, OPENDASH_DP_CAT_VESC, 0},
    {0x0608, "VESC Temp Motor", "°C", -40, 150, OPENDASH_DP_CAT_VESC, 0},
    {0x0609, "VESC Current In", "A", 0, 100, OPENDASH_DP_CAT_VESC, 1},
    {0x060A, "VESC PID Pos", "°", 0, 360, OPENDASH_DP_CAT_VESC, 0},
    {0x060B, "VESC Tacho", "", 0, 10000, OPENDASH_DP_CAT_VESC, 0},
    {0x060C, "VESC V In", "V", 0, 100, OPENDASH_DP_CAT_VESC, 1},
    {0x060D, "VESC ADC 1", "V", 0, 10, OPENDASH_DP_CAT_VESC, 2},
    {0x060E, "VESC ADC 2", "V", 0, 10, OPENDASH_DP_CAT_VESC, 2},
    {0x060F, "VESC ADC 3", "V", 0, 10, OPENDASH_DP_CAT_VESC, 2},
    {0x0610, "VESC PPM", "", 0, 1, OPENDASH_DP_CAT_VESC, 2},
    {0x0611, "VESC RPM", "rpm", 0, 10000, OPENDASH_DP_CAT_VESC, 0},
    {0x0612, "VESC Power In", "W", 0, 10000, OPENDASH_DP_CAT_VESC, 0},
    {0x0613, "VESC Power Motor", "W", 0, 10000, OPENDASH_DP_CAT_VESC, 0},
    {0x0614, "VESC Fault", "", 0, 100, OPENDASH_DP_CAT_VESC, 0},
    {0x0620, "Wheel RPM FL", "rpm", 0, 1000, OPENDASH_DP_CAT_VESC, 0},
    {0x0621, "Wheel RPM FR", "rpm", 0, 1000, OPENDASH_DP_CAT_VESC, 0},
    {0x0622, "Wheel RPM RL", "rpm", 0, 1000, OPENDASH_DP_CAT_VESC, 0},
    {0x0623, "Wheel RPM RR", "rpm", 0, 1000, OPENDASH_DP_CAT_VESC, 0},
    {0x0624, "Wheel Speed Avg", "km/h", 0, 200, OPENDASH_DP_CAT_VESC, 0},
    
    // Relay / MOS controller data points: 0x0700 – 0x07FF
    {0x0700, "Relay 4 CH1", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0701, "Relay 4 CH2", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0702, "Relay 4 CH3", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0703, "Relay 4 CH4", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0710, "Relay 8A CH1", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0711, "Relay 8A CH2", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0712, "Relay 8A CH3", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0713, "Relay 8A CH4", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0714, "Relay 8A CH5", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0715, "Relay 8A CH6", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0716, "Relay 8A CH7", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0717, "Relay 8A CH8", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0720, "Relay 8B CH1", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0721, "Relay 8B CH2", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0722, "Relay 8B CH3", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0723, "Relay 8B CH4", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0724, "Relay 8B CH5", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0725, "Relay 8B CH6", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0726, "Relay 8B CH7", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0727, "Relay 8B CH8", "", 0, 1, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0730, "MOS 4A CH1", "", 0, 100, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0731, "MOS 4A CH2", "", 0, 100, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0732, "MOS 4A CH3", "", 0, 100, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0733, "MOS 4A CH4", "", 0, 100, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0740, "MOS 4B CH1", "", 0, 100, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0741, "MOS 4B CH2", "", 0, 100, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0742, "MOS 4B CH3", "", 0, 100, OPENDASH_DP_CAT_SYSTEM, 0},
    {0x0743, "MOS 4B CH4", "", 0, 100, OPENDASH_DP_CAT_SYSTEM, 0},
};

const size_t opendash_dp_catalog_count = sizeof(opendash_dp_catalog) / sizeof(opendash_dp_catalog[0]);

const opendash_dp_info_t *opendash_dp_lookup(uint16_t dp_id) {
    // Simple linear search for demonstration - in practice this would be binary search
    for (size_t i = 0; i < opendash_dp_catalog_count; i++) {
        if (opendash_dp_catalog[i].dp_id == dp_id) {
            return &opendash_dp_catalog[i];
        }
    }
    return NULL;
}