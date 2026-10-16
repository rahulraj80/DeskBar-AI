#pragma once
#include <stdint.h>
#include <stdbool.h>

#define BATTERY_ADC_PIN   4       // ADC_BATT GPIO4
#define BATTERY_VDIV      3.0f    // 2:1 hardware divider ratio (scaling raw mV * 3.0)
#define BATTERY_VREF      3.3f    // Reference voltage
#define BATTERY_ADC_MAX   4095.0f // 12-bit ADC
#define BATTERY_CURVE_PTS 14

#ifdef __cplusplus
extern "C" {
#endif

void battery_init(void);
float readRawBatteryVoltage(void);
float readBatteryVoltage(void);
uint8_t voltageToBatteryPercent(float voltage);
void getBatteryStatus(float *voltage_out, uint8_t *percent_out);
bool bsp_is_battery_charging(void);

// 2-Point Breathing Sampling Accumulator
void battery_accumulate_sample(uint16_t raw_adc_mv, bool screen_is_on);
uint16_t battery_get_and_reset_5min_average_mv(void);
uint16_t battery_get_rolling_average_mv(void);

// Dynamic Server-Assisted Battery Discharge Calibration
bool battery_sync_from_cloud(const char *table_override);
const char* battery_get_cal_source(void);
void battery_get_curve_points(float *out_volts, uint8_t *out_pcts, uint8_t *out_count);
bool battery_save_nvs_curve(const float *volts, const uint8_t *pcts, uint8_t count, const char *source);

#ifdef __cplusplus
}
#endif
