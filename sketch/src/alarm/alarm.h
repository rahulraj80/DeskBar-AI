#pragma once
#include <Arduino.h>

void alarm_init(void);
void alarm_check(uint8_t hour, uint8_t min, uint8_t sec);
void alarm_trigger(void);
void alarm_stop(void);
void alarm_set_time(uint8_t hour, uint8_t min);
void alarm_set_enabled(bool enabled);
bool alarm_is_enabled(void);
bool alarm_is_active(void);
void alarm_get_time(uint8_t *hour, uint8_t *min);
