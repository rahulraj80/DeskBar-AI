#pragma once
#include <stdint.h>

#define BATTERY_ADC_PIN   4       // ADC_BATT GPIO4
#define BATTERY_VDIV      3.0f    // 2:1 hardware divider ratio (scaling raw mV * 3.0)
#define BATTERY_VREF      3.3f    // Reference voltage
#define BATTERY_ADC_MAX   4095.0f // 12-bit ADC

float readRawBatteryVoltage();
float readBatteryVoltage();
uint8_t voltageToBatteryPercent(float voltage);
void getBatteryStatus(float *voltage_out, uint8_t *percent_out);
