#include "telemetry.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "battery/battery.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "user_config.h"
#include "ESP32-audioI2S-master/Audio.h"
#include "pcf85063/pcf85063.h"
#include <time.h>

extern Audio audio;

static TelemetrySnapshot *telemetry_ring_buf = nullptr;
static uint16_t telemetry_count = 0;
static const char *s_telemetry_endpoint = TELEMETRY_ENDPOINT;

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

    snap.batt_mv = (uint16_t)analogReadMilliVolts(ADC_BATT);
    snap.batt_pct = pct;
    snap.rssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : (int8_t)0;
    snap.audio_playing = audio.isRunning() ? 1 : 0;
    snap.screen_state = BL_OFF ? 0 : 1;
    snap.reset_reason = (uint8_t)esp_reset_reason();

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
    if (WiFi.status() != WL_CONNECTED) {
        log_w("[TELEMETRY] WiFi not connected, retaining %d buffered records", telemetry_count);
        is_flushing = false;
        vTaskDelete(NULL);
        return;
    }

    // Allocate JSON construction buffer in PSRAM (16KB)
    const size_t json_capacity = 16384;
    char *json_buf = (char *)heap_caps_malloc(json_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json_buf) {
        log_e("[TELEMETRY] Failed to allocate JSON buffer in PSRAM");
        is_flushing = false;
        vTaskDelete(NULL);
        return;
    }

    int offset = snprintf(json_buf, json_capacity, "{\"table\":\"" TELEMETRY_TABLE "\",\"rows\":[");
    for (uint16_t i = 0; i < telemetry_count; i++) {
        const TelemetrySnapshot &s = telemetry_ring_buf[i];
        int written = snprintf(
            json_buf + offset,
            json_capacity - offset,
            "%s{\"capture_time\":\"%s\",\"uptime_s\":%u,\"dram_free_kb\":%u,\"dram_used_pct\":%u,\"sram_total_kb\":%u,\"psram_free_kb\":%u,\"psram_used_pct\":%u,\"batt_mv\":%u,\"batt_pct\":%u,\"rssi\":%d,\"audio\":%u,\"screen\":%u,\"reset_reason\":%u}",
            (i > 0 ? "," : ""),
            s.capture_time, s.uptime_s, s.dram_free_kb, s.dram_used_pct, s.sram_total_kb,
            s.psram_free_kb, s.psram_used_pct, s.batt_mv, s.batt_pct, (int)s.rssi,
            s.audio_playing, s.screen_state, s.reset_reason
        );
        if (written < 0 || (offset + written >= (int)json_capacity - 10)) {
            log_w("[TELEMETRY] JSON buffer capacity reached at item %d", i);
            break;
        }
        offset += written;
    }
    offset += snprintf(json_buf + offset, json_capacity - offset, "]}");

    log_i("[TELEMETRY] Flushing %d rows (%d bytes) to %s (Table %s)...",
          telemetry_count, offset, s_telemetry_endpoint, TELEMETRY_TABLE);

    {
        WiFiClientSecure client;
        client.setInsecure(); // Public server with Let's Encrypt TLS
        HTTPClient http;
        http.begin(client, s_telemetry_endpoint);
        http.addHeader("Content-Type", "application/json");
        http.setTimeout(8000);

        int httpCode = http.POST((uint8_t *)json_buf, offset);

        if (httpCode > 0) {
            String resp = http.getString();
            log_i("[TELEMETRY] HTTP %d: %s", httpCode, resp.c_str());
            if (httpCode == 200 && resp.indexOf("\"status\":\"ok\"") >= 0) {
                log_i("[TELEMETRY] Batch upload SUCCESS. Clearing %d records.", telemetry_count);
                telemetry_count = 0;
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
    if (WiFi.status() != WL_CONNECTED) {
        log_w("[TELEMETRY] WiFi not connected, cannot flush");
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
