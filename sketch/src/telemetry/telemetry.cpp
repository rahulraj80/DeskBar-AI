#include "telemetry.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "battery/battery.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "user_config.h"
#include "../secrets/secrets_manager.h"
#include "ESP32-audioI2S-master/Audio.h"
#include "pcf85063/pcf85063.h"
#include "../tca9554/tca9554.h"
#include "../imu/imu_service.h"
#include "../lvgl_port/lvgl_port.h"
#include <time.h>

extern Audio audio;
extern bool ai_recording_active;
extern "C" bool is_display_deep_black(void);
extern uint32_t g_last_user_interaction_ms;
extern bool wifiEnable;
extern uint8_t mediaType;

static TelemetrySnapshot *telemetry_ring_buf = nullptr;
static uint16_t telemetry_count = 0;
static const char *s_telemetry_endpoint = TELEMETRY_ENDPOINT;

// 5-second survey accumulators (0 to 60 counts per 5-minute cycle)
static uint8_t s_surv_cpu_240m = 0;
static uint8_t s_surv_cpu_160m = 0;
static uint8_t s_surv_cpu_80m  = 0;
static uint8_t s_surv_cpu_low  = 0;
static uint8_t s_surv_bl_on    = 0;
static uint8_t s_surv_bl_dark  = 0;
static uint8_t s_surv_bl_boost = 0;
static uint8_t s_surv_disp_30hz= 0;
static uint8_t s_surv_disp_act = 0;
static uint8_t s_surv_audio_act= 0;
static uint8_t s_surv_audio_amp= 0;
static uint8_t s_surv_mic_act  = 0;
static uint8_t s_surv_wifi_conn= 0;
static uint8_t s_surv_wifi_off = 0;
static uint8_t s_surv_imu_act  = 0;
static uint8_t s_surv_imu_sleep= 0;
static uint8_t s_surv_user_act = 0;
static uint8_t s_surv_breath   = 0;

void telemetry_survey_tick_5s(void) {
    uint32_t freq = getCpuFrequencyMhz();
    if (freq == 240) s_surv_cpu_240m++;
    else if (freq == 160) s_surv_cpu_160m++;
    else if (freq == 80) s_surv_cpu_80m++;
    else s_surv_cpu_low++;

    bool is_dark = (BL_OFF || is_display_deep_black());
    if (is_dark) s_surv_bl_dark++;
    else s_surv_bl_on++;

    if (bsp_get_backlight_power()) s_surv_bl_boost++;
    if (bsp_get_audio_amp_power()) s_surv_audio_amp++;

    if (lvgl_port_is_30hz_mode()) s_surv_disp_30hz++;
    if (!BL_OFF) s_surv_disp_act++;

    if (audio.isRunning()) s_surv_audio_act++;
    if (ai_recording_active) s_surv_mic_act++;

    if (WiFi.status() == WL_CONNECTED) s_surv_wifi_conn++;
    if (WiFi.getMode() == WIFI_OFF) s_surv_wifi_off++;

    if (imu_sensor_is_sleeping()) s_surv_imu_sleep++;
    else s_surv_imu_act++;

    if (millis() - g_last_user_interaction_ms <= 60000UL) s_surv_user_act++;

    if (clock_face_active) s_surv_breath++;
}

void telemetry_init(void) {
    if (!telemetry_ring_buf) {
        telemetry_ring_buf = (TelemetrySnapshot *)heap_caps_malloc(
            sizeof(TelemetrySnapshot) * TELEMETRY_MAX_BUFFER,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );
        if (telemetry_ring_buf) {
            memset(telemetry_ring_buf, 0, sizeof(TelemetrySnapshot) * TELEMETRY_MAX_BUFFER);
            log_i("[TELEMETRY] Ring buffer allocated in PSRAM (%u bytes)",
                  (unsigned)(sizeof(TelemetrySnapshot) * TELEMETRY_MAX_BUFFER));
        } else {
            log_e("[TELEMETRY] Failed to allocate PSRAM ring buffer!");
        }
    }
    telemetry_count = 0;
    telemetry_collect_snapshot(); // Capture immediate boot record with reset_reason
}

void telemetry_collect_snapshot(void) {
    if (!telemetry_ring_buf) {
        telemetry_init();
        if (!telemetry_ring_buf) return;
    }

    if (telemetry_count >= TELEMETRY_MAX_BUFFER) {
        // Shift oldest out to maintain newest TELEMETRY_MAX_BUFFER
        memmove(&telemetry_ring_buf[0], &telemetry_ring_buf[1],
                sizeof(TelemetrySnapshot) * (TELEMETRY_MAX_BUFFER - 1));
        telemetry_count = TELEMETRY_MAX_BUFFER - 1;
    }

    float volt = 0.0f;
    uint8_t pct = 0;
    getBatteryStatus(&volt, &pct);

    TelemetrySnapshot snap;
    memset(&snap, 0, sizeof(snap));
    snap.ts = (uint32_t)(millis() / 1000);
    snap.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    size_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_dram = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    snap.dram_free_kb = (uint16_t)(free_dram / 1024);
    snap.dram_used_pct = (total_dram > 0) ? (uint8_t)(100 - (free_dram * 100 / total_dram)) : 0;
    snap.sram_total_kb = (uint16_t)(total_dram / 1024);

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    snap.psram_free_kb = (uint16_t)(free_psram / 1024);
    snap.psram_used_pct = (total_psram > 0) ? (uint8_t)(100 - (free_psram * 100 / total_psram)) : 0;

    snap.batt_mv = battery_get_and_reset_5min_average_mv();
    snap.batt_pct = voltageToBatteryPercent((snap.batt_mv * BATTERY_VDIV) / 1000.0f);
    snap.rssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : (int8_t)0;
    snap.audio_playing = audio.isRunning() ? 1 : 0;
    snap.screen_state = BL_OFF ? 0 : 1;
    snap.reset_reason = (uint8_t)esp_reset_reason();

    // 5-second survey counts
    snap.cpu_240m_cnt   = s_surv_cpu_240m;
    snap.cpu_160m_cnt   = s_surv_cpu_160m;
    snap.cpu_80m_cnt    = s_surv_cpu_80m;
    snap.cpu_low_cnt    = s_surv_cpu_low;
    snap.bl_on_cnt      = s_surv_bl_on;
    snap.bl_dark_cnt    = s_surv_bl_dark;
    snap.bl_boost_cnt   = s_surv_bl_boost;
    snap.disp_30hz_cnt  = s_surv_disp_30hz;
    snap.disp_active_cnt= s_surv_disp_act;
    snap.audio_active_cnt = s_surv_audio_act;
    snap.audio_amp_cnt  = s_surv_audio_amp;
    snap.mic_active_cnt = s_surv_mic_act;
    snap.wifi_conn_cnt  = s_surv_wifi_conn;
    snap.wifi_off_cnt   = s_surv_wifi_off;
    snap.imu_active_cnt = s_surv_imu_act;
    snap.imu_sleep_cnt  = s_surv_imu_sleep;
    snap.user_active_cnt= s_surv_user_act;
    snap.breathing_cnt  = s_surv_breath;

    // Reset accumulators for next 5-min cycle
    s_surv_cpu_240m = 0; s_surv_cpu_160m = 0; s_surv_cpu_80m = 0; s_surv_cpu_low = 0;
    s_surv_bl_on = 0; s_surv_bl_dark = 0; s_surv_bl_boost = 0;
    s_surv_disp_30hz = 0; s_surv_disp_act = 0;
    s_surv_audio_act = 0; s_surv_audio_amp = 0; s_surv_mic_act = 0;
    s_surv_wifi_conn = 0; s_surv_wifi_off = 0;
    s_surv_imu_act = 0; s_surv_imu_sleep = 0;
    s_surv_user_act = 0; s_surv_breath = 0;

    if (now.year > 0 && now.month >= 1 && now.month <= 12) {
        snprintf(snap.capture_time, sizeof(snap.capture_time), "%04d-%02d-%02d %02d:%02d:%02d",
                 2000 + now.year, now.month, now.dayOfMonth, now.hour, now.minute, now.second);
    } else {
        snprintf(snap.capture_time, sizeof(snap.capture_time), "Uptime %us", snap.uptime_s);
    }

    telemetry_ring_buf[telemetry_count++] = snap;
    log_i("[TELEMETRY] Snapshot (%d/%d): time=%s, uptime=%us, DRAM used=%d%% (free %dKB), PSRAM used=%d%%, reset=%d",
          telemetry_count, TELEMETRY_MAX_BUFFER, snap.capture_time, snap.uptime_s,
          snap.dram_used_pct, snap.dram_free_kb, snap.psram_used_pct, snap.reset_reason);
}

static volatile bool is_flushing = false;

bool telemetry_is_flushing(void) {
    return is_flushing;
}

uint16_t telemetry_get_buffered_count(void) {
    return telemetry_count;
}

static void telemetry_worker_task(void *pvParameters) {
    if (telemetry_count == 0) {
        log_i("[TELEMETRY] No records to flush");
        is_flushing = false;
        vTaskDelete(NULL);
        return;
    }

    if (!secrets_has_cloud_consent() && !secrets_has_custom_ini()) {
        log_i("[TELEMETRY] Cloud consent not granted, retaining %d buffered records", telemetry_count);
        is_flushing = false;
        vTaskDelete(NULL);
        return;
    }

    bool was_wifi_off = (WiFi.status() != WL_CONNECTED || WiFi.getMode() == WIFI_OFF);
    if (WiFi.status() != WL_CONNECTED) {
        log_i("[TELEMETRY] Burst wake: bringing Wi-Fi up for telemetry flush...");
        if (WiFi.getMode() == WIFI_OFF) {
            WiFi.mode(WIFI_STA);
        }
        WiFi.begin();

        uint32_t wait_start = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - wait_start < 8000)) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        if (WiFi.status() != WL_CONNECTED) {
            log_w("[TELEMETRY] Burst Wi-Fi reconnect timed out (8s), retaining %d records", telemetry_count);
            if (was_wifi_off && clock_face_active && !bsp_is_battery_charging() &&
                (g_last_user_interaction_ms == 0 || millis() - g_last_user_interaction_ms >= 120000UL)) {
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
            }
            is_flushing = false;
            vTaskDelete(NULL);
            return;
        }
        log_i("[TELEMETRY] Burst Wi-Fi connected in %ums", (unsigned)(millis() - wait_start));
    }

    const char *endpoint = secrets_get_telemetry_endpoint();
    const char *table_name = secrets_get_telemetry_table();

    // Allocate JSON construction buffer in PSRAM (48KB - easily fits 60+ snapshots)
    const size_t json_capacity = 49152;
    char *json_buf = (char *)heap_caps_malloc(json_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json_buf) {
        log_e("[TELEMETRY] Failed to allocate JSON buffer in PSRAM");
        is_flushing = false;
        vTaskDelete(NULL);
        return;
    }

    uint16_t sent_records = 0;
    int offset = snprintf(json_buf, json_capacity, "{\"table\":\"%s\",\"rows\":[", table_name);
    for (uint16_t i = 0; i < telemetry_count; i++) {
        const TelemetrySnapshot &s = telemetry_ring_buf[i];
        int written = snprintf(
            json_buf + offset,
            json_capacity - offset,
            "%s{\"capture_time\":\"%s\",\"uptime_s\":%u,\"dram_free_kb\":%u,\"dram_used_pct\":%u,\"sram_total_kb\":%u,\"psram_free_kb\":%u,\"psram_used_pct\":%u,\"batt_mv\":%u,\"batt_pct\":%u,\"rssi\":%d,\"audio\":%u,\"screen\":%u,\"reset_reason\":%u,"
            "\"cpu_240m_cnt\":%u,\"cpu_160m_cnt\":%u,\"cpu_80m_cnt\":%u,\"cpu_low_cnt\":%u,\"bl_on_cnt\":%u,\"bl_dark_cnt\":%u,\"bl_boost_cnt\":%u,\"disp_30hz_cnt\":%u,\"disp_active_cnt\":%u,\"audio_active_cnt\":%u,\"audio_amp_cnt\":%u,\"mic_active_cnt\":%u,\"wifi_conn_cnt\":%u,\"wifi_off_cnt\":%u,\"imu_active_cnt\":%u,\"imu_sleep_cnt\":%u,\"user_active_cnt\":%u,\"breathing_cnt\":%u}",
            (i > 0 ? "," : ""),
            s.capture_time, s.uptime_s, s.dram_free_kb, s.dram_used_pct, s.sram_total_kb,
            s.psram_free_kb, s.psram_used_pct, s.batt_mv, s.batt_pct, (int)s.rssi,
            s.audio_playing, s.screen_state, s.reset_reason,
            s.cpu_240m_cnt, s.cpu_160m_cnt, s.cpu_80m_cnt, s.cpu_low_cnt,
            s.bl_on_cnt, s.bl_dark_cnt, s.bl_boost_cnt, s.disp_30hz_cnt, s.disp_active_cnt,
            s.audio_active_cnt, s.audio_amp_cnt, s.mic_active_cnt,
            s.wifi_conn_cnt, s.wifi_off_cnt, s.imu_active_cnt, s.imu_sleep_cnt,
            s.user_active_cnt, s.breathing_cnt
        );
        if (written < 0 || (offset + written >= (int)json_capacity - 10)) {
            log_w("[TELEMETRY] JSON buffer capacity reached at item %d", i);
            break;
        }
        offset += written;
        sent_records++;
    }
    offset += snprintf(json_buf + offset, json_capacity - offset, "]}");

    log_i("[TELEMETRY] Flushing %d rows (%d bytes) to %s (Table %s)...",
          sent_records, offset, endpoint, table_name);

    {
        WiFiClientSecure client;
        client.setInsecure(); // Public server with Let's Encrypt TLS
        HTTPClient http;
        http.begin(client, endpoint);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(8000);

        int httpCode = http.POST((uint8_t *)json_buf, offset);

        if (httpCode > 0) {
            String resp = http.getString();
            log_i("[TELEMETRY] HTTP %d: %s", httpCode, resp.c_str());
            if (httpCode == 200 && resp.indexOf("\"status\":\"ok\"") >= 0) {
                log_i("[TELEMETRY] Batch upload SUCCESS (%u records sent).", sent_records);
                if (sent_records >= telemetry_count) {
                    telemetry_count = 0;
                } else {
                    memmove(&telemetry_ring_buf[0], &telemetry_ring_buf[sent_records],
                            sizeof(TelemetrySnapshot) * (telemetry_count - sent_records));
                    telemetry_count -= sent_records;
                }
            } else {
                log_w("[TELEMETRY] Upload rejected: %s", resp.c_str());
            }
        } else {
            log_e("[TELEMETRY] POST failed, error: %s", http.errorToString(httpCode).c_str());
        }

        http.end();
        client.stop();
    }

    heap_caps_free(json_buf);

    // Return Wi-Fi to low-power sleep IF:
    // 1. Wi-Fi was off prior to burst
    // 2. Running on battery (not charging via USB)
    // 3. User has not interacted within last 2 minutes
    // 4. Audio is not playing
    // 5. Clock face is active
    if (was_wifi_off && clock_face_active && (!audio.isRunning() || mediaType >= 2) &&
        !bsp_is_battery_charging() &&
        (g_last_user_interaction_ms == 0 || millis() - g_last_user_interaction_ms >= 120000UL)) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        log_i("[TELEMETRY] Duty cycle burst complete -> Wi-Fi returned to low-power sleep");
    } else {
        log_i("[TELEMETRY] Wi-Fi remaining active (charging, playing, or user interactive)");
    }

    is_flushing = false;
    vTaskDelete(NULL);
}

bool telemetry_get_latest(TelemetrySnapshot *out) {
    if (!out || telemetry_count == 0 || !telemetry_ring_buf) {
        return false;
    }
    *out = telemetry_ring_buf[telemetry_count - 1];
    return true;
}

bool telemetry_flush_batch(void) {
    if (is_flushing) {
        log_w("[TELEMETRY] Flush already in progress");
        return false;
    }
    if (telemetry_count == 0) {
        return false;
    }
    if (!wifiEnable) {
        log_w("[TELEMETRY] WiFi disabled in settings, cannot flush");
        return false;
    }

    is_flushing = true;
    BaseType_t res = xTaskCreatePinnedToCore(
        telemetry_worker_task,
        "telem_flush",
        8192,
        NULL,
        1,
        NULL,
        1
    );

    if (res != pdPASS) {
        log_e("[TELEMETRY] Failed to spawn worker task");
        is_flushing = false;
        return false;
    }
    return true;
}
