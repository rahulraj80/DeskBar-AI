#include "alarm.h"
#include <Preferences.h>
#include <WiFi.h>
#include "ESP32-audioI2S-master/Audio.h"
#include "file/file.h"
#include "task_msg/task_msg.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "tca9554/tca9554.h"

extern Audio audio;
extern "C" void exit_clock_breathing(void);
extern void audioSetVolume(uint8_t vol);

static Preferences alarm_pref;
static bool s_alarm_enabled = false;
static uint8_t s_alarm_hour = 7;
static uint8_t s_alarm_min = 0;
static bool s_alarm_active = false;
static int8_t s_last_triggered_min = -1;

void alarm_init(void) {
    alarm_pref.begin("tb_alarm", false);
    s_alarm_enabled = alarm_pref.getBool("en", false);
    s_alarm_hour = alarm_pref.getUChar("hr", 7);
    s_alarm_min = alarm_pref.getUChar("min", 0);
    alarm_pref.end();
    log_i("[ALARM] Initialized: %s, Time: %02d:%02d",
          s_alarm_enabled ? "ENABLED" : "DISABLED", s_alarm_hour, s_alarm_min);
}

void alarm_set_time(uint8_t hour, uint8_t min) {
    if (hour > 23) hour = 23;
    if (min > 59) min = 59;
    s_alarm_hour = hour;
    s_alarm_min = min;
    alarm_pref.begin("tb_alarm", false);
    alarm_pref.putUChar("hr", s_alarm_hour);
    alarm_pref.putUChar("min", s_alarm_min);
    alarm_pref.end();
    log_i("[ALARM] Time updated: %02d:%02d", s_alarm_hour, s_alarm_min);
}

void alarm_set_enabled(bool enabled) {
    s_alarm_enabled = enabled;
    alarm_pref.begin("tb_alarm", false);
    alarm_pref.putBool("en", s_alarm_enabled);
    alarm_pref.end();
    log_i("[ALARM] State updated: %s", s_alarm_enabled ? "ENABLED" : "DISABLED");
}

bool alarm_is_enabled(void) {
    return s_alarm_enabled;
}

bool alarm_is_active(void) {
    return s_alarm_active;
}

void alarm_get_time(uint8_t *hour, uint8_t *min) {
    if (hour) *hour = s_alarm_hour;
    if (min) *min = s_alarm_min;
}

void alarm_trigger(void) {
    s_alarm_active = true;
    log_i("[ALARM] >>> WAKE UP! ALARM TRIGGERED <<<");

    // 1. Wake screen and disable breathing
    exit_clock_breathing();
    screenPowerOn();
    backlight_state = 2; // HIGH
    setUpduty(LCD_BL_HIGH);

    // 2. Turn audio amp on and ramp volume
    bsp_set_audio_amp_power(true);
    audioSetVolume(18);

    // 3. Play stream or safe fallback
    if (WiFi.status() == WL_CONNECTED && stationListLength > 0 && stations != nullptr && stationIndex < stationListLength) {
        log_i("[ALARM] Streaming radio station: %s", stations[stationIndex].name);
        audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
    } else {
        log_i("[ALARM] Radio stream unavailable, screen awake with full brightness");
    }
}

void alarm_stop(void) {
    if (s_alarm_active) {
        s_alarm_active = false;
        log_i("[ALARM] Alarm dismissed / stopped.");
        if (audio.isRunning()) {
            audio.stopSong();
        }
    }
}

void alarm_check(uint8_t hour, uint8_t min, uint8_t sec) {
    if (!s_alarm_enabled) return;

    if (hour == s_alarm_hour && min == s_alarm_min) {
        if (s_last_triggered_min != min) {
            s_last_triggered_min = min;
            UIStatusPayload msg = {};
            msg.type = STATUS_ALARM_TRIGGER;
            xQueueSend(ui_status_queue, &msg, 100);
        }
    } else {
        // Reset trigger flag when minute moves away
        if (s_last_triggered_min != -1 && s_last_triggered_min != min) {
            s_last_triggered_min = -1;
        }
    }
}
