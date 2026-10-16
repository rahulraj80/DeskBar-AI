#include "power_profile.h"
#include <Arduino.h>
#include <Preferences.h>

static Preferences s_pwr_pref;
static power_profile_t s_cfg;
static bool s_initialized = false;

static const power_profile_t DEFAULT_PROFILE = {
    .bl_start        = 91,
    .bl_end          = 120,
    .bl_black        = 0,
    .ramp_up_ms      = 1500,
    .hold_high_ms    = 2000,
    .ramp_down_ms    = 1500,
    .hold_dark_ms    = 5000,
    .inactivity_tier = true,
    .gate_wifi       = true,
    .gate_audio      = true,
    .gate_sensors    = true
};

void power_profile_init(void) {
    if (s_initialized) return;

    s_cfg = DEFAULT_PROFILE;
    if (s_pwr_pref.begin("tb_pwr", true)) {
        if (s_pwr_pref.isKey("bl_start")) {
            s_cfg.bl_start        = s_pwr_pref.getUChar("bl_start", DEFAULT_PROFILE.bl_start);
            s_cfg.bl_end          = s_pwr_pref.getUChar("bl_end", DEFAULT_PROFILE.bl_end);
            s_cfg.bl_black        = s_pwr_pref.getUChar("bl_black", DEFAULT_PROFILE.bl_black);
            s_cfg.ramp_up_ms      = s_pwr_pref.getUShort("ramp_up", DEFAULT_PROFILE.ramp_up_ms);
            s_cfg.hold_high_ms    = s_pwr_pref.getUShort("hold_high", DEFAULT_PROFILE.hold_high_ms);
            s_cfg.ramp_down_ms    = s_pwr_pref.getUShort("ramp_down", DEFAULT_PROFILE.ramp_down_ms);
            s_cfg.hold_dark_ms    = s_pwr_pref.getUShort("hold_dark", DEFAULT_PROFILE.hold_dark_ms);
            s_cfg.inactivity_tier = s_pwr_pref.getBool("inact_tier", DEFAULT_PROFILE.inactivity_tier);
            s_cfg.gate_wifi       = s_pwr_pref.getBool("gate_wifi", DEFAULT_PROFILE.gate_wifi);
            s_cfg.gate_audio      = s_pwr_pref.getBool("gate_audio", DEFAULT_PROFILE.gate_audio);
            s_cfg.gate_sensors    = s_pwr_pref.getBool("gate_sensor", DEFAULT_PROFILE.gate_sensors);
            log_i("[PWR] Profile loaded from NVS: BL[%u-%u] Dark:%u RampUp:%ums HoldHigh:%ums RampDown:%ums Dark:%ums",
                  s_cfg.bl_start, s_cfg.bl_end, s_cfg.bl_black, s_cfg.ramp_up_ms, s_cfg.hold_high_ms, s_cfg.ramp_down_ms, s_cfg.hold_dark_ms);
        } else {
            log_i("[PWR] No existing profile in NVS, using defaults");
        }
        s_pwr_pref.end();
    }
    s_initialized = true;
}

const power_profile_t* power_profile_get(void) {
    if (!s_initialized) power_profile_init();
    return &s_cfg;
}

void power_profile_set(const power_profile_t *config) {
    if (!config) return;
    s_cfg = *config;

    if (s_pwr_pref.begin("tb_pwr", false)) {
        s_pwr_pref.putUChar("bl_start", s_cfg.bl_start);
        s_pwr_pref.putUChar("bl_end", s_cfg.bl_end);
        s_pwr_pref.putUChar("bl_black", s_cfg.bl_black);
        s_pwr_pref.putUShort("ramp_up", s_cfg.ramp_up_ms);
        s_pwr_pref.putUShort("hold_high", s_cfg.hold_high_ms);
        s_pwr_pref.putUShort("ramp_down", s_cfg.ramp_down_ms);
        s_pwr_pref.putUShort("hold_dark", s_cfg.hold_dark_ms);
        s_pwr_pref.putBool("inact_tier", s_cfg.inactivity_tier);
        s_pwr_pref.putBool("gate_wifi", s_cfg.gate_wifi);
        s_pwr_pref.putBool("gate_audio", s_cfg.gate_audio);
        s_pwr_pref.putBool("gate_sensor", s_cfg.gate_sensors);
        s_pwr_pref.end();
        log_i("[PWR] Saved updated power profile to NVS");
    }
}

void power_profile_reset_defaults(void) {
    power_profile_set(&DEFAULT_PROFILE);
}
