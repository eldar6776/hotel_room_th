#pragma once
#include <stdint.h>

// Shared by C UI, Modbus and NVS loading. Units: tenths of a degree Celsius.
#define SENSOR_OFFSET_MIN_X10 (-100)
#define SENSOR_OFFSET_MAX_X10 100

static inline int16_t settings_clamp_sensor_offset(int32_t value)
{
    if (value < SENSOR_OFFSET_MIN_X10) return SENSOR_OFFSET_MIN_X10;
    if (value > SENSOR_OFFSET_MAX_X10) return SENSOR_OFFSET_MAX_X10;
    return (int16_t)value;
}
