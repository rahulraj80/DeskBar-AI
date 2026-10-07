#include "battery.h"
#include <Arduino.h>

static const float volt_table[] = {
  4.20f, 4.10f, 4.00f, 3.90f, 3.80f,
  3.70f, 3.60f, 3.50f, 3.40f, 3.30f, 3.20f
};
static const uint8_t pct_table[] = {
  100,    90,    80,    70,    60,
   50,    40,    30,    20,    10,    0
};
#define CURVE_POINTS 11

// 10-second Exponential Moving Average state (8 bytes DRAM)
static float s_smoothed_voltage = -1.0f;
static uint32_t s_last_sample_ms = 0;

float readRawBatteryVoltage() {
  uint32_t mv = analogReadMilliVolts(BATTERY_ADC_PIN);
  if (mv > 0) {
    return (mv * BATTERY_VDIV) / 1000.0f;
  }
  int raw = analogRead(BATTERY_ADC_PIN);
  return (raw / BATTERY_ADC_MAX) * BATTERY_VREF * BATTERY_VDIV;
}

float readBatteryVoltage() {
  uint32_t now = millis();
  
  // Sample once per second (1 Hz) or on first boot call
  if (s_smoothed_voltage < 0.0f || (now - s_last_sample_ms >= 1000)) {
    float raw_v = readRawBatteryVoltage();
    if (s_smoothed_voltage < 0.0f) {
      // First boot: latch immediately so UI never lags starting from 0V
      s_smoothed_voltage = raw_v;
    } else {
      // 10-second exponential moving average (alpha = 0.10)
      // Filters Wi-Fi TX current sags (350-400mA) and audio amplifier spikes
      s_smoothed_voltage = (0.10f * raw_v) + (0.90f * s_smoothed_voltage);
    }
    s_last_sample_ms = now;
  }
  return s_smoothed_voltage;
}

uint8_t voltageToBatteryPercent(float v) {
  if (v >= volt_table[0]) return 100;
  if (v <= volt_table[CURVE_POINTS - 1]) return 0;
  for (int i = 0; i < CURVE_POINTS - 1; i++) {
    if (v <= volt_table[i] && v >= volt_table[i + 1]) {
      float diff = volt_table[i] - volt_table[i + 1];
      if (diff <= 0.0001f) return pct_table[i];
      float ratio = (v - volt_table[i + 1]) / diff;
      return (uint8_t)(pct_table[i + 1] + ratio * (pct_table[i] - pct_table[i + 1]));
    }
  }
  return 0;
}

void getBatteryStatus(float *voltage_out, uint8_t *percent_out) {
  *voltage_out = readBatteryVoltage();
  *percent_out = voltageToBatteryPercent(*voltage_out);
}
