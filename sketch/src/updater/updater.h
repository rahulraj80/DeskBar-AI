#pragma once
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>
#include "version.h"

extern bool firmware_checked;
extern const char* compile_date;
extern const char* current_version;

// Core OTA Safety and Teardown APIs
bool ota_check_battery_allowed(uint8_t *out_pct, float *out_v);
void ota_pre_update_teardown(void);

// Bold Neon Blue Full-Screen Display APIs
void ota_ui_show_neon_progress(const char *target_version);
void ota_ui_update_progress(uint8_t pct, size_t loaded, size_t total, uint32_t speed_kbs);
void ota_ui_show_complete(const char *build_str);
void ota_ui_show_blocked_battery(uint8_t batt_pct);
void ota_post_boot_toast(void);

// Real HTTP Background OTA Flasher
bool ota_start_http_flash(const char *version_str, const char *bin_url);

// Legacy and helper functions
const char *newFirmwareAvailable();
void ota_show_popup();
void notifyUpdate(const char *latestVer);
void memoryInfo(char *buf, size_t len);
void systemInfo(char *buf, size_t len);
