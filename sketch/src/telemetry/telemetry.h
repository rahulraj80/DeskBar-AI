#pragma once
#include <Arduino.h>

struct TelemetrySnapshot {
    uint32_t ts;
    uint32_t uptime_s;
    uint16_t dram_free_kb;
    uint8_t  dram_used_pct;
    uint16_t sram_total_kb;
    uint16_t psram_free_kb;
    uint8_t  psram_used_pct;
    uint16_t batt_mv;
    uint8_t  batt_pct;
    int8_t   rssi;
    uint8_t  audio_playing;
    uint8_t  screen_state;
    uint8_t  reset_reason;
    char     capture_time[20]; // YYYY-MM-DD HH:MM:SS

    // 5-second survey accumulators (0 - 60 count per 5-min cycle)
    uint8_t  cpu_240m_cnt;
    uint8_t  cpu_160m_cnt;
    uint8_t  cpu_80m_cnt;
    uint8_t  cpu_low_cnt;
    uint8_t  bl_on_cnt;
    uint8_t  bl_dark_cnt;
    uint8_t  bl_boost_cnt;
    uint8_t  disp_30hz_cnt;
    uint8_t  disp_active_cnt;
    uint8_t  audio_active_cnt;
    uint8_t  audio_amp_cnt;
    uint8_t  mic_active_cnt;
    uint8_t  wifi_conn_cnt;
    uint8_t  wifi_off_cnt;
    uint8_t  imu_active_cnt;
    uint8_t  imu_sleep_cnt;
    uint8_t  user_active_cnt;
    uint8_t  breathing_cnt;
};

#define TELEMETRY_MAX_BUFFER 60 // 60 snapshots = 1 hour at 60s intervals

void telemetry_init(void);
void telemetry_survey_tick_5s(void);
void telemetry_collect_snapshot(void);
bool telemetry_flush_batch(void);
bool telemetry_is_flushing(void);
uint16_t telemetry_get_buffered_count(void);
bool telemetry_get_latest(TelemetrySnapshot *out);
