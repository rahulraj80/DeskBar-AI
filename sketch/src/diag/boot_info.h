#pragma once
// Firmware identity + reboot diagnostics shared by the serial CLI, /api/status
// and the boot log. Survives soft resets (panic, watchdog, esp_restart) via
// RTC_NOINIT memory; a power cycle starts the count again at 1.
#include <stdint.h>
#include <stddef.h>

void boot_info_init(void);                     // call first thing in setup()
void boot_info_tick(void);                     // periodically: records current uptime
void boot_info_set_action(const char *action); // last notable user/system action

uint32_t boot_info_uptime_s(void);
uint32_t boot_info_boot_count(void);           // boots since last power-on
int boot_info_reset_reason(void);              // esp_reset_reason_t of this boot
const char *boot_info_reset_reason_str(void);
bool boot_info_last_reset_was_crash(void);     // panic / watchdog / brownout
uint32_t boot_info_prior_uptime_s(void);       // uptime reached before this reset
const char *boot_info_prior_action(void);      // last action before this reset

const char *boot_info_version(void);           // "2.7.1"
uint32_t boot_info_build(void);
const char *boot_info_git_hash(void);
const char *boot_info_build_date(void);

void boot_info_format_duration(uint32_t s, char *out, size_t outsz);  // "1d 02h 03m 04s"
