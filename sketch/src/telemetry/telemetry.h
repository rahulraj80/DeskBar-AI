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
};

#define TELEMETRY_MAX_BUFFER 60 // 60 snapshots = 1 hour at 60s intervals

void telemetry_init(void);
void telemetry_collect_snapshot(void);
bool telemetry_flush_batch(void);
bool telemetry_is_flushing(void);
uint16_t telemetry_get_buffered_count(void);
bool telemetry_get_latest(TelemetrySnapshot *out);
