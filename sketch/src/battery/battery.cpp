#include "battery.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "../secrets/secrets_manager.h"

// Factory Golden Baseline SOC Curve (14-point piecewise linear)
static const float FACTORY_VOLTS[BATTERY_CURVE_PTS] = {
  4.065f, 4.000f, 3.970f, 3.945f, 3.930f, 3.920f, 3.910f,
  3.885f, 3.840f, 3.780f, 3.735f, 3.690f, 3.500f, 3.000f
};
static const uint8_t FACTORY_PCTS[BATTERY_CURVE_PTS] = {
  100, 95, 90, 85, 80, 70, 60, 50, 40, 30, 20, 10, 5, 0
};

static float   s_active_volts[BATTERY_CURVE_PTS];
static uint8_t s_active_pcts[BATTERY_CURVE_PTS];
static char    s_cal_source[32] = "factory_baseline";
static bool    s_initialized = false;
static uint8_t s_last_discharged_pct = 100;

// 2-Point Breathing Sampling Accumulator (5-minute telemetry cycle)
static uint32_t s_accum_sum_mv = 0;
static uint16_t s_accum_count = 0;
static float    s_rolling_adc_mv = -1.0f;

void battery_init(void) {
    if (s_initialized) return;

    // Default to factory baseline
    memcpy(s_active_volts, FACTORY_VOLTS, sizeof(s_active_volts));
    memcpy(s_active_pcts, FACTORY_PCTS, sizeof(s_active_pcts));
    strncpy(s_cal_source, "factory_baseline", sizeof(s_cal_source));

    Preferences pref;
    if (pref.begin("tb_batt", true)) {
        if (pref.isKey("valid") && pref.getBool("valid", false)) {
            size_t v_len = sizeof(s_active_volts);
            size_t p_len = sizeof(s_active_pcts);
            if (pref.getBytes("volts", s_active_volts, v_len) == v_len &&
                pref.getBytes("pcts", s_active_pcts, p_len) == p_len) {
                String src = pref.getString("source", "nvs_cached");
                strncpy(s_cal_source, src.c_str(), sizeof(s_cal_source) - 1);
                log_i("[BATT] Loaded calibrated curve from NVS (Source: %s, 100%%=%.3fV, 0%%=%.3fV)",
                      s_cal_source, s_active_volts[0], s_active_volts[BATTERY_CURVE_PTS - 1]);
            }
        }
        pref.end();
    }

    // Seed initial reading
    uint32_t initial_raw = analogReadMilliVolts(BATTERY_ADC_PIN);
    if (initial_raw > 0) {
        s_rolling_adc_mv = (float)initial_raw;
    }
    s_initialized = true;
}

float readRawBatteryVoltage(void) {
    uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
    if (mv > 0) {
        return (mv * BATTERY_VDIV) / 1000.0f;
    }
    int raw = analogRead(BATTERY_ADC_PIN);
    return (raw / BATTERY_ADC_MAX) * BATTERY_VREF * BATTERY_VDIV;
}

float readBatteryVoltage(void) {
    if (!s_initialized) battery_init();

    if (s_rolling_adc_mv <= 0.0f) {
        uint32_t raw = analogReadMilliVolts(BATTERY_ADC_PIN);
        s_rolling_adc_mv = (raw > 0) ? (float)raw : 1330.0f;
    }
    return (s_rolling_adc_mv * BATTERY_VDIV) / 1000.0f;
}

uint8_t voltageToBatteryPercent(float v) {
    if (!s_initialized) battery_init();

    uint8_t pct = 0;
    if (v >= s_active_volts[0]) {
        pct = 100;
    } else if (v <= s_active_volts[BATTERY_CURVE_PTS - 1]) {
        pct = 0;
    } else {
        for (int i = 0; i < BATTERY_CURVE_PTS - 1; i++) {
            if (v <= s_active_volts[i] && v >= s_active_volts[i + 1]) {
                float diff = s_active_volts[i] - s_active_volts[i + 1];
                if (diff <= 0.0001f) {
                    pct = s_active_pcts[i];
                    break;
                }
                float ratio = (v - s_active_volts[i + 1]) / diff;
                pct = (uint8_t)(s_active_pcts[i + 1] + ratio * (s_active_pcts[i] - s_active_pcts[i + 1]));
                break;
            }
        }
    }

    // Monotonic discharge clamp: battery percentage cannot jump upwards during discharge
    if (bsp_is_battery_charging()) {
        s_last_discharged_pct = 100;
        return pct;
    } else {
        if (pct > s_last_discharged_pct) {
            return s_last_discharged_pct;
        }
        s_last_discharged_pct = pct;
        return pct;
    }
}

void getBatteryStatus(float *voltage_out, uint8_t *percent_out) {
    *voltage_out = readBatteryVoltage();
    *percent_out = voltageToBatteryPercent(*voltage_out);
}

bool bsp_is_battery_charging(void) {
    // When connected to USB power, charging IC holds battery rail at charge voltage (>= 1340 mV at ADC pin)
    return (analogReadMilliVolts(BATTERY_ADC_PIN) >= 1340);
}

// ----------------------------------------------------------------------------
// 2-Point Breathing Sampling Accumulator
// ----------------------------------------------------------------------------
void battery_accumulate_sample(uint16_t raw_adc_mv, bool screen_is_on) {
    if (raw_adc_mv == 0) return;
    if (!s_initialized) battery_init();

    s_accum_sum_mv += raw_adc_mv;
    s_accum_count++;

    // Update rolling average for smooth live UI display
    if (s_rolling_adc_mv <= 0.0f) {
        s_rolling_adc_mv = (float)raw_adc_mv;
    } else {
        // Alpha = 0.15 for smooth response without lag
        s_rolling_adc_mv = (0.15f * (float)raw_adc_mv) + (0.85f * s_rolling_adc_mv);
    }

    log_d("[BATT_ACCUM] Sample #%u: %u mV (screen %s) | Rolling: %.1f mV (%.2fV, %u%%)",
          s_accum_count, raw_adc_mv, screen_is_on ? "ON" : "OFF",
          s_rolling_adc_mv, (s_rolling_adc_mv * BATTERY_VDIV) / 1000.0f,
          voltageToBatteryPercent((s_rolling_adc_mv * BATTERY_VDIV) / 1000.0f));
}

uint16_t battery_get_and_reset_5min_average_mv(void) {
    uint16_t avg_mv;
    if (s_accum_count > 0) {
        avg_mv = (uint16_t)((s_accum_sum_mv + (s_accum_count / 2)) / s_accum_count);
        log_i("[BATT_ACCUM] 5-min telemetry average computed: %u mV across %u samples", avg_mv, s_accum_count);
    } else {
        avg_mv = (uint16_t)analogReadMilliVolts(BATTERY_ADC_PIN);
        log_w("[BATT_ACCUM] No accumulated samples in 5-min cycle, instantaneous fallback: %u mV", avg_mv);
    }
    s_accum_sum_mv = 0;
    s_accum_count = 0;
    return avg_mv;
}

uint16_t battery_get_rolling_average_mv(void) {
    if (s_rolling_adc_mv <= 0.0f) {
        return (uint16_t)analogReadMilliVolts(BATTERY_ADC_PIN);
    }
    return (uint16_t)s_rolling_adc_mv;
}

// ----------------------------------------------------------------------------
// Cloud Calibration & NVS Curve Management
// ----------------------------------------------------------------------------
const char* battery_get_cal_source(void) {
    return s_cal_source;
}

void battery_get_curve_points(float *out_volts, uint8_t *out_pcts, uint8_t *out_count) {
    if (!s_initialized) battery_init();
    if (out_count) *out_count = BATTERY_CURVE_PTS;
    if (out_volts) memcpy(out_volts, s_active_volts, sizeof(s_active_volts));
    if (out_pcts)  memcpy(out_pcts, s_active_pcts, sizeof(s_active_pcts));
}

bool battery_save_nvs_curve(const float *volts, const uint8_t *pcts, uint8_t count, const char *source) {
    if (!volts || !pcts || count != BATTERY_CURVE_PTS) return false;

    Preferences pref;
    if (pref.begin("tb_batt", false)) {
        pref.putBool("valid", true);
        pref.putBytes("volts", volts, sizeof(float) * count);
        pref.putBytes("pcts", pcts, sizeof(uint8_t) * count);
        pref.putString("source", source ? source : "custom");
        pref.end();

        memcpy(s_active_volts, volts, sizeof(s_active_volts));
        memcpy(s_active_pcts, pcts, sizeof(s_active_pcts));
        strncpy(s_cal_source, source ? source : "custom", sizeof(s_cal_source) - 1);
        log_i("[BATT] Saved new battery calibration to NVS (Source: %s)", s_cal_source);
        return true;
    }
    return false;
}

bool battery_sync_from_cloud(const char *table_override) {
    if (WiFi.status() != WL_CONNECTED) {
        log_w("[BATT_CAL] Wi-Fi not connected, cannot sync battery calibration");
        return false;
    }

    const char *tbl = (table_override && strlen(table_override) > 0) ? table_override : secrets_get_telemetry_table();
    char url[128];
    snprintf(url, sizeof(url), "https://autobots.my.to/battery_cal.php?table=%s", tbl);
    log_i("[BATT_CAL] Requesting cloud calibration from: %s", url);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        log_e("[BATT_CAL] HTTP GET failed: %d (%s)", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        log_e("[BATT_CAL] JSON parsing error: %s", err.c_str());
        return false;
    }

    const char *status = doc["status"] | "";
    if (strcmp(status, "ok") != 0) {
        log_e("[BATT_CAL] Server response status not ok");
        return false;
    }

    JsonArray v_arr = doc["volts"].as<JsonArray>();
    JsonArray p_arr = doc["pcts"].as<JsonArray>();
    if (v_arr.size() != BATTERY_CURVE_PTS || p_arr.size() != BATTERY_CURVE_PTS) {
        log_e("[BATT_CAL] Curve point count mismatch: got %u volts, %u pcts (expected %u)",
              v_arr.size(), p_arr.size(), BATTERY_CURVE_PTS);
        return false;
    }

    float new_volts[BATTERY_CURVE_PTS];
    uint8_t new_pcts[BATTERY_CURVE_PTS];
    for (int i = 0; i < BATTERY_CURVE_PTS; i++) {
        new_volts[i] = v_arr[i].as<float>();
        new_pcts[i]  = p_arr[i].as<uint8_t>();
    }

    const char *src = doc["source"] | "cloud_cal";
    bool saved = battery_save_nvs_curve(new_volts, new_pcts, BATTERY_CURVE_PTS, src);
    if (saved) {
        log_i("[BATT_CAL] Successfully synced & applied curve from cloud! 100%%=%.3fV, 0%%=%.3fV",
              new_volts[0], new_volts[BATTERY_CURVE_PTS - 1]);
    }
    return saved;
}
