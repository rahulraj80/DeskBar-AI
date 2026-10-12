#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float ax, ay, az;      // in g (1g = 9.807 m/s^2)
    float gx, gy, gz;      // in degrees/sec (dps)
    float temp_c;          // in Celsius
    uint32_t timestamp_ms; // reading timestamp
    bool is_inverted;      // true if 180-deg landscape
    const char *orientation_str; // "Normal Landscape", "Inverted Landscape", "Portrait (Ignored)", "Flat / Neutral"
} imu_reading_t;

// Lifecycle
bool imu_sensor_init(void);
bool imu_sensor_poll(void);

// Data Access
bool imu_sensor_get_latest(imu_reading_t *out);
bool imu_sensor_is_available(void);

// Auto-rotation Control
void imu_sensor_set_autorotate_enabled(bool enabled);
bool imu_sensor_is_autorotate_enabled(void);
void imu_sensor_force_orientation(bool inverted);

#ifdef __cplusplus
}
#endif
