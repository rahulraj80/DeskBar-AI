#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t  bl_start;        // Min backlight floor (default: 91)
    uint8_t  bl_end;          // Max backlight ceiling (default: 120)
    uint8_t  bl_black;        // Backlight PWM during dark (default: 0 = boost cut)
    uint16_t ramp_up_ms;      // Fade-in duration ms (default: 1500)
    uint16_t hold_high_ms;    // Full brightness duration ms (default: 2000)
    uint16_t ramp_down_ms;    // Fade-out duration ms (default: 1500)
    uint16_t hold_dark_ms;    // Full dark duration ms (default: 5000)
    bool     inactivity_tier; // Double dark duration after 30m idle (default: true)
    bool     gate_wifi;       // Duty-cycle Wi-Fi off during dark on battery (default: true)
    bool     gate_audio;      // Gate NS_MODE audio amp when dark/idle (default: true)
    bool     gate_sensors;    // Put IMU to sleep when dark (default: true)
} power_profile_t;

void power_profile_init(void);
const power_profile_t* power_profile_get(void);
void power_profile_set(const power_profile_t *config);
void power_profile_reset_defaults(void);

#ifdef __cplusplus
}
#endif
