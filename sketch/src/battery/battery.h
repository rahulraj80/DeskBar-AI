#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BATTERY_ADC_PIN   4       // ADC_BATT GPIO4
#define BATTERY_VDIV      3.0f    // 2:1 hardware divider ratio (scaling raw mV * 3.0)
#define BATTERY_VREF      3.3f    // Reference voltage
#define BATTERY_ADC_MAX   4095.0f // 12-bit ADC
#define BATTERY_CURVE_PTS 14

// Voltage (battery side) at which the monotonic discharge clamp is reset even
// when the fragile ADC charging check (>= 1340 mV) momentarily fails due to
// noise. 4.0V is clearly charged for a 4.2V LiPo, so a recovered battery can
// never stay stuck at a low percentage.
#define BATTERY_CHARGE_RESET_V 4.0f

// Discharge-clamp escape hatch below BATTERY_CHARGE_RESET_V. The clamp may only
// ever fall, so a USB charge started at e.g. 3.6V kept the last discharge
// percentage frozen until the pack crossed 4.0V. The clamp now re-syncs to the
// curve once the voltage has climbed BATTERY_RATCHET_RISE_V above the lowest
// point seen since it engaged AND the curve says the charge went up by more
// than noise (BATTERY_PCT_RECOVER_PTS).
#define BATTERY_RATCHET_RISE_V   0.030f
#define BATTERY_PCT_RECOVER_PTS  5

// A fleet-averaged curve can repeat a point or wobble by a few mV between
// adjacent samples; a larger adjacent inversion means the curve shape is
// broken (the shape that maps a full battery to a low percentage).
#define BATTERY_CURVE_JITTER_V   0.010f

#ifdef __cplusplus
extern "C" {
#endif

void battery_init(void);
float readRawBatteryVoltage(void);
float readBatteryVoltage(void);
uint8_t voltageToBatteryPercent(float voltage);
void getBatteryStatus(float *voltage_out, uint8_t *percent_out);
bool bsp_is_battery_charging(void);
uint32_t battery_read_adc_mv_safe(void);

// 2-Point Breathing Sampling Accumulator
void battery_accumulate_sample(uint16_t raw_adc_mv, bool screen_is_on);
uint16_t battery_get_and_reset_5min_average_mv(void);
uint16_t battery_get_rolling_average_mv(void);

// Dynamic Server-Assisted Battery Discharge Calibration
bool battery_sync_from_cloud(const char *table_override);
const char* battery_get_cal_source(void);
const char* battery_get_cal_source_label(void);
const char* battery_get_cal_timestamp(void);
void battery_get_curve_points(float *out_volts, uint8_t *out_pcts, uint8_t *out_count);
bool battery_save_nvs_curve(const float *volts, const uint8_t *pcts, uint8_t count, const char *source);

#ifdef __cplusplus
}
#endif
