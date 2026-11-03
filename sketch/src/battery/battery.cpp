#include "battery.h"
#include <Arduino.h>
#include <math.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include "../secrets/secrets_manager.h"
#include "pcf85063/pcf85063.h"
extern PCF85063 rtc;

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
static char    s_cal_timestamp[32] = "Never";
static bool    s_initialized = false;
static uint8_t s_last_discharged_pct = 100;
// Lowest battery voltage seen since the discharge clamp last re-synced: the
// clamp releases when the pack climbs BATTERY_RATCHET_RISE_V above it.
static float   s_ratchet_low_v = 0.0f;

// 2-Point Breathing Sampling Accumulator (5-minute telemetry cycle)
static uint32_t s_accum_sum_mv = 0;
static uint16_t s_accum_count = 0;
static float    s_rolling_adc_mv = -1.0f;
static SemaphoreHandle_t s_adc_mutex = NULL;

// Forward declaration: battery_init() validates the NVS curve before the
// full definition below (see "Curve validation" section).
static bool battery_curve_is_valid(const float *volts, const uint8_t *pcts, uint8_t count);

uint32_t battery_read_adc_mv_safe(void) {
    if (!s_adc_mutex) {
        s_adc_mutex = xSemaphoreCreateMutex();
    }
    if (s_adc_mutex && xSemaphoreTake(s_adc_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
        xSemaphoreGive(s_adc_mutex);
        return mv;
    }
    return 0;
}

void battery_init(void) {
    if (s_initialized) return;

    // Default to factory baseline
    memcpy(s_active_volts, FACTORY_VOLTS, sizeof(s_active_volts));
    memcpy(s_active_pcts, FACTORY_PCTS, sizeof(s_active_pcts));
    strncpy(s_cal_source, "factory_baseline", sizeof(s_cal_source));
    strncpy(s_cal_timestamp, "Factory", sizeof(s_cal_timestamp));

    Preferences pref;
    if (pref.begin("tb_batt", true)) {
        if (pref.isKey("valid") && pref.getBool("valid", false)) {
            size_t v_len = sizeof(s_active_volts);
            size_t p_len = sizeof(s_active_pcts);
            // Both arrays must come from the same save: accepting one and
            // falling back on the other leaves a mixed curve where an NVS
            // voltage table is paired with factory percentages.
            const bool v_ok = pref.getBytes("volts", s_active_volts, v_len) == v_len;
            const bool p_ok = pref.getBytes("pcts", s_active_pcts, p_len) == p_len;
            if (v_ok && p_ok &&
                battery_curve_is_valid(s_active_volts, s_active_pcts, BATTERY_CURVE_PTS)) {
                String src = pref.getString("source", "nvs_cached");
                strncpy(s_cal_source, src.c_str(), sizeof(s_cal_source) - 1);
                String t = pref.getString("cal_time", "Unknown");
                strncpy(s_cal_timestamp, t.c_str(), sizeof(s_cal_timestamp) - 1);
                log_i("[BATT] Loaded calibrated curve from NVS (Source: %s, Time: %s, 100%%=%.3fV, 0%%=%.3fV)",
                      s_cal_source, s_cal_timestamp, s_active_volts[0], s_active_volts[BATTERY_CURVE_PTS - 1]);
            } else {
                // Corrupt / out-of-range / half-written curve in NVS: fall back
                // to the factory baseline instead of mapping a full battery to
                // a low percentage. Capture the rejected values BEFORE the
                // overwrite so the boot log reports what was actually bad.
                float bad_100 = s_active_volts[0];
                float bad_0   = s_active_volts[BATTERY_CURVE_PTS - 1];
                memcpy(s_active_volts, FACTORY_VOLTS, sizeof(s_active_volts));
                memcpy(s_active_pcts, FACTORY_PCTS, sizeof(s_active_pcts));
                strncpy(s_cal_source, "factory_baseline", sizeof(s_cal_source));
                strncpy(s_cal_timestamp, "Factory", sizeof(s_cal_timestamp));
                if (!v_ok || !p_ok) {
                    log_w("[BATT] NVS curve incomplete (volts %s, pcts %s) - falling back to factory baseline",
                          v_ok ? "ok" : "short", p_ok ? "ok" : "short");
                } else {
                    log_w("[BATT] NVS curve invalid (100%%=%.3fV, 0%%=%.3fV) - falling back to factory baseline",
                          bad_100, bad_0);
                }
            }
        }
        pref.end();
    }

    // Seed initial reading
    uint32_t initial_raw = battery_read_adc_mv_safe();
    if (initial_raw > 0) {
        s_rolling_adc_mv = (float)initial_raw;
    }
    s_initialized = true;
}

// ----------------------------------------------------------------------------
// Curve validation
// ----------------------------------------------------------------------------
// A usable SOC curve must be strictly monotonic descending in voltage and stay
// inside the physical LiPo range. The cloud endpoint and NVS are both untrusted
// inputs: a garbage curve (e.g. computed from a wrong capacity assumption) can
// map a full 4.1V battery to a low percentage, which is exactly the "4.1V but
// 4%" symptom. Rejecting bad curves here keeps the factory baseline active.
static bool battery_curve_is_valid(const float *volts, const uint8_t *pcts, uint8_t count) {
    if (!volts || !pcts || count < 2) return false;

    // Physical LiPo bounds, with slack for fleet-averaged endpoints: a peer
    // average can top out below a single device's full charge and bottom out
    // above its cut-off. NaN fails every comparison and is rejected here too.
    if (!(volts[0] >= 3.9f && volts[0] <= 4.4f)) return false;
    if (!(volts[count - 1] >= 2.7f && volts[count - 1] <= 3.5f)) return false;

    // The curve has to span a real discharge range, not be flat.
    if ((volts[0] - volts[count - 1]) < 0.3f) return false;

    // Voltages must descend, and a fleet average can legitimately repeat a
    // point or wobble up by a few mV. Only a rise beyond that jitter is a
    // broken shape. (This check used to demand volts[i+1] >= volts[i] - jitter,
    // i.e. that the curve stay near-flat: every real curve - factory, device
    // history, fleet average - was rejected by it, while a degenerate
    // flat-at-full payload sailed through. That is why no good curve could
    // ever be applied.)
    for (int i = 0; i < count - 1; i++) {
        if (volts[i + 1] > volts[i] + BATTERY_CURVE_JITTER_V) return false;
    }

    // Percentages must be strictly monotonic descending, start near 100 and
    // end near 0. Exact 100/0 is too restrictive: a fleet-average curve can
    // legitimately top out at e.g. 99% or bottom out at 1% after averaging.
    // The upper bound also rejects garbage >100 values that would otherwise
    // pass as "above 95".
    if (pcts[0] < 96 || pcts[0] > 100) return false;
    if (pcts[count - 1] > 4) return false;
    for (int i = 0; i < count - 1; i++) {
        if (pcts[i] <= pcts[i + 1]) return false;
    }

    // Shape sanity, independent of monotonicity: a fit computed from a run
    // that only fell a few tens of mV gets stretched over the whole 100->0
    // range, which puts 50% at ~4.1V and dumps the rest of the pack into one
    // final step. That is the curve that reported 4.1V as 4%.
    int mid = BATTERY_CURVE_PTS / 2;
    for (int i = 0; i < count; i++) {
        if (pcts[i] == 50) { mid = i; break; }
    }
    if (!(volts[mid] >= BATTERY_CURVE_MID_MIN_V && volts[mid] <= BATTERY_CURVE_MID_MAX_V)) return false;
    for (int i = 0; i < count - 1; i++) {
        if ((volts[i] - volts[i + 1]) > BATTERY_CURVE_MAX_STEP_V) return false;
    }
    return true;
}

float readRawBatteryVoltage(void) {
    uint32_t mv = battery_read_adc_mv_safe();
    if (mv > 0) {
        return (mv * BATTERY_VDIV) / 1000.0f;
    }
    int raw = analogRead(BATTERY_ADC_PIN);
    return (raw / BATTERY_ADC_MAX) * BATTERY_VREF * BATTERY_VDIV;
}

float readBatteryVoltage(void) {
    if (!s_initialized) battery_init();

    if (s_rolling_adc_mv <= 0.0f) {
        uint32_t raw = battery_read_adc_mv_safe();
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
        int seg = -1;
        for (int i = 0; i < BATTERY_CURVE_PTS - 1; i++) {
            if (v <= s_active_volts[i] && v >= s_active_volts[i + 1]) {
                seg = i;
                break;
            }
        }
        if (seg < 0) {
            // No exact bracket: the curve repeats a point or wobbles by a few
            // mV between two samples (normal for a fleet average). Pick the
            // nearest segment instead of falling through to 0%, which would
            // report ~empty for a nearly-full battery - the "4.1V but 4%"
            // failure mode.
            float best_d = -1.0f;
            for (int i = 0; i < BATTERY_CURVE_PTS - 1; i++) {
                float mid = 0.5f * (s_active_volts[i] + s_active_volts[i + 1]);
                float d = fabsf(v - mid);
                if (best_d < 0.0f || d < best_d) {
                    best_d = d;
                    seg = i;
                }
            }
        }
        if (seg >= 0) {
            float diff = s_active_volts[seg] - s_active_volts[seg + 1];
            float ratio = 1.0f; // degenerate segment: trust the upper point
            if (diff > 0.0001f) {
                ratio = (v - s_active_volts[seg + 1]) / diff;
                if (ratio < 0.0f) ratio = 0.0f;
                if (ratio > 1.0f) ratio = 1.0f;
            }
            pct = (uint8_t)(s_active_pcts[seg + 1] + ratio * (s_active_pcts[seg] - s_active_pcts[seg + 1]));
        }
    }

    // Monotonic discharge clamp: battery percentage cannot jump upwards during
    // discharge. The clamp is released when the battery is clearly charged:
    // either the charging IC reports charge voltage at the ADC pin, OR the
    // battery-side voltage itself is >= BATTERY_CHARGE_RESET_V. The voltage
    // check is the authoritative one - the ADC charging threshold (1340 mV)
    // sits only ~27 mV above a full 4.1V reading and can be missed by noise,
    // which previously left the percentage stuck at a low value while the
    // voltage recovered to 4.1V.
    if (bsp_is_battery_charging() || v >= BATTERY_CHARGE_RESET_V) {
        s_last_discharged_pct = 100;
        s_ratchet_low_v = v;
        return pct;
    }

    // Release the clamp when the pack is genuinely gaining charge below the
    // reset voltage: the voltage climbed BATTERY_RATCHET_RISE_V above the
    // lowest point seen since the clamp engaged AND the curve agrees the
    // charge rose by more than noise. Otherwise a USB charge started at e.g.
    // 3.6V stays frozen at the last discharge percentage until 4.0V.
    if (s_ratchet_low_v <= 0.0f ||
        (v >= s_ratchet_low_v + BATTERY_RATCHET_RISE_V &&
         pct >= s_last_discharged_pct + BATTERY_PCT_RECOVER_PTS)) {
        s_last_discharged_pct = pct;
        s_ratchet_low_v = v;
        return pct;
    }

    if (pct > s_last_discharged_pct) {
        return s_last_discharged_pct;
    }
    s_last_discharged_pct = pct;
    s_ratchet_low_v = v;
    return pct;
}

void getBatteryStatus(float *voltage_out, uint8_t *percent_out) {
    *voltage_out = readBatteryVoltage();
    *percent_out = voltageToBatteryPercent(*voltage_out);
}

bool bsp_is_battery_charging(void) {
    // When connected to USB power, charging IC holds battery rail at charge voltage (>= 1340 mV at ADC pin)
    return (battery_read_adc_mv_safe() >= 1340);
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

const char* battery_get_cal_source_label(void) {
    if (strcmp(s_cal_source, "device_history") == 0) return "This Device";
    if (strcmp(s_cal_source, "fleet_average") == 0 || strcmp(s_cal_source, "crowd_sourced") == 0) return "Crowd Sourced";
    if (strcmp(s_cal_source, "factory_baseline") == 0) return "Factory Baseline";
    return s_cal_source;
}

const char* battery_get_cal_timestamp(void) {
    return s_cal_timestamp;
}

void battery_get_curve_points(float *out_volts, uint8_t *out_pcts, uint8_t *out_count) {
    if (!s_initialized) battery_init();
    if (out_count) *out_count = BATTERY_CURVE_PTS;
    if (out_volts) memcpy(out_volts, s_active_volts, sizeof(s_active_volts));
    if (out_pcts)  memcpy(out_pcts, s_active_pcts, sizeof(s_active_pcts));
}

bool battery_save_nvs_curve(const float *volts, const uint8_t *pcts, uint8_t count, const char *source) {
    if (!volts || !pcts || count != BATTERY_CURVE_PTS) return false;

    // Reject physically impossible curves before they reach NVS: a bad curve
    // (e.g. from a wrong capacity assumption) is what maps a full 4.1V battery
    // to a low percentage.
    if (!battery_curve_is_valid(volts, pcts, count)) {
        log_e("[BATT] Refusing to save invalid curve (Source: %s, 100%%=%.3fV, 0%%=%.3fV)",
              source ? source : "custom", volts[0], volts[count - 1]);
        return false;
    }

    // Capture RTC timestamp
    rtc.getDateTime();
    if (now.year > 0 && now.month >= 1 && now.month <= 12) {
        snprintf(s_cal_timestamp, sizeof(s_cal_timestamp), "%04d-%02d-%02d %02d:%02d:%02d",
                 2000 + now.year, now.month, now.dayOfMonth, now.hour, now.minute, now.second);
    } else {
        snprintf(s_cal_timestamp, sizeof(s_cal_timestamp), "Uptime %lus", millis() / 1000);
    }

    Preferences pref;
    if (pref.begin("tb_batt", false)) {
        pref.putBool("valid", true);
        pref.putBytes("volts", volts, sizeof(float) * count);
        pref.putBytes("pcts", pcts, sizeof(uint8_t) * count);
        pref.putString("source", source ? source : "custom");
        pref.putString("cal_time", s_cal_timestamp);
        pref.end();

        memcpy(s_active_volts, volts, sizeof(s_active_volts));
        memcpy(s_active_pcts, pcts, sizeof(s_active_pcts));
        // A new curve is a new percentage basis: an old ratchet value would
        // hold the display at a stale percentage until the pack crossed 4.0V.
        s_last_discharged_pct = 100;
        s_ratchet_low_v = 0.0f;
        strncpy(s_cal_source, source ? source : "custom", sizeof(s_cal_source) - 1);
        log_i("[BATT] Saved new battery calibration to NVS (Source: %s, Time: %s)", s_cal_source, s_cal_timestamp);
        return true;
    }
    return false;
}

// log_e only survives at CORE_DEBUG_LEVEL>=2, but a refused calibration must be
// visible in the field, so every hard failure also prints to Serial.
#define BATT_CAL_ERR(fmt, ...) do { log_e(fmt, ##__VA_ARGS__); Serial.printf(fmt "\n", ##__VA_ARGS__); } while (0)

// One GET against the calibration service. table == NULL/empty asks for the
// fleet average, which the service already computes from every device with a
// usable discharge run - no need to wait for this pack to finish its own.
static bool battery_apply_cloud_curve(const char *table) {
    if (WiFi.status() != WL_CONNECTED) {
        log_w("[BATT_CAL] Wi-Fi not connected, cannot sync battery calibration");
        return false;
    }

    char url[128];
    if (table && strlen(table) > 0) {
        snprintf(url, sizeof(url), "https://autobots.my.to/battery_cal.php?table=%s", table);
    } else {
        snprintf(url, sizeof(url), "https://autobots.my.to/battery_cal.php");
    }
    log_i("[BATT_CAL] Requesting cloud calibration from: %s", url);

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    http.setTimeout(10000);

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        BATT_CAL_ERR("[BATT_CAL] HTTP GET failed: %d (%s)", httpCode, http.errorToString(httpCode).c_str());
        http.end();
        return false;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        BATT_CAL_ERR("[BATT_CAL] JSON parsing error: %s", err.c_str());
        return false;
    }

    const char *status = doc["status"] | "";
    if (strcmp(status, "ok") != 0) {
        BATT_CAL_ERR("[BATT_CAL] Server response status not ok");
        return false;
    }

    JsonArray v_arr = doc["volts"].as<JsonArray>();
    JsonArray p_arr = doc["pcts"].as<JsonArray>();
    if (v_arr.size() != BATTERY_CURVE_PTS || p_arr.size() != BATTERY_CURVE_PTS) {
        BATT_CAL_ERR("[BATT_CAL] Curve point count mismatch: got %u volts, %u pcts (expected %u)",
              v_arr.size(), p_arr.size(), BATTERY_CURVE_PTS);
        return false;
    }

    float new_volts[BATTERY_CURVE_PTS];
    uint8_t new_pcts[BATTERY_CURVE_PTS];
    for (int i = 0; i < BATTERY_CURVE_PTS; i++) {
        new_volts[i] = v_arr[i].as<float>();
        new_pcts[i]  = p_arr[i].as<uint8_t>();
    }

    // Validate the curve BEFORE it can replace the active one: reject
    // non-monotonic, out-of-range, or degenerate curves from the cloud so a
    // bad server payload can never map a full battery to a low percentage.
    if (!battery_curve_is_valid(new_volts, new_pcts, BATTERY_CURVE_PTS)) {
        BATT_CAL_ERR("[BATT_CAL] Cloud curve invalid (100%%=%.3fV, 50%%=%.3fV, 0%%=%.3fV) - keeping current curve",
                     new_volts[0], new_volts[BATTERY_CURVE_PTS / 2], new_volts[BATTERY_CURVE_PTS - 1]);
        return false;
    }

    const char *src = doc["source"] | "cloud_cal";
    bool saved = battery_save_nvs_curve(new_volts, new_pcts, BATTERY_CURVE_PTS, src);
    if (saved) {
        log_i("[BATT_CAL] Successfully synced & applied curve from cloud! source=%s 100%%=%.3fV, 50%%=%.3fV, 0%%=%.3fV",
              src, new_volts[0], new_volts[BATTERY_CURVE_PTS / 2], new_volts[BATTERY_CURVE_PTS - 1]);
    }
    return saved;
}

bool battery_sync_from_cloud(const char *table_override) {
    const char *own = (table_override && strlen(table_override) > 0)
                          ? table_override
                          : secrets_get_telemetry_table();

    if (battery_apply_cloud_curve(own)) return true;

    // This pack's own history was unusable (not enough discharge, or a fit
    // that failed shape validation). Fall back to the fleet average the same
    // service already derives from the other devices' full curves.
    if (own && strlen(own) > 0) {
        log_w("[BATT_CAL] Device curve unusable for %s - requesting fleet average", own);
        if (battery_apply_cloud_curve(NULL)) return true;
    }
    return false;
}

static volatile bool s_sync_task_running = false;

// The pull runs on its own task and brings the radio up itself: the power
// profile parks Wi-Fi when the bar is idle, and a pull that silently refuses
// while the radio is parked is exactly how the fleet curve never reached this
// pack in the first place.
static void battery_sync_worker(void *arg) {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[BATT_CAL] Wi-Fi parked - waking it for the calibration pull");
        if (WiFi.getMode() == WIFI_OFF) {
            WiFi.mode(WIFI_STA);
        }
        WiFi.begin();
        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - t0 < 8000)) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    const bool ok = battery_sync_from_cloud(NULL);
    Serial.printf("[BATT_CAL] Cloud curve pull %s (active source: %s)\n",
                  ok ? "APPLIED" : "FAILED", battery_get_cal_source());
    s_sync_task_running = false;
    vTaskDelete(NULL);
}

bool battery_sync_from_cloud_async(void) {
    if (s_sync_task_running) return false;
    s_sync_task_running = true;
    if (xTaskCreate(battery_sync_worker, "batt_cal", 10240, NULL, 2, NULL) != pdPASS) {
        s_sync_task_running = false;
        log_e("[BATT_CAL] Failed to start calibration worker");
        return false;
    }
    return true;
}
