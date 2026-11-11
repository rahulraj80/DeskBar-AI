#include "boot_info.h"
#include <Arduino.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_attr.h>
#include <string.h>
#include "version.h"

static const uint32_t BOOT_INFO_MAGIC = 0x54554E45;  // "TUNE"

RTC_NOINIT_ATTR static uint32_t rtc_magic;
RTC_NOINIT_ATTR static uint32_t rtc_boot_count;
RTC_NOINIT_ATTR static uint32_t rtc_uptime_s;
RTC_NOINIT_ATTR static char rtc_action[48];

// Snapshots taken at boot: the RTC copies are overwritten while running, so
// reading them later would report this boot, not the one that crashed.
static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;
static uint32_t s_prior_uptime_s = 0;
static char s_prior_action[48] = "";

static const char *reason_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "Power-on";
        case ESP_RST_EXT:       return "External reset";
        case ESP_RST_SW:        return "Software restart";
        case ESP_RST_PANIC:     return "Crash (panic)";
        case ESP_RST_INT_WDT:   return "Interrupt watchdog";
        case ESP_RST_TASK_WDT:  return "Task watchdog";
        case ESP_RST_WDT:       return "Watchdog";
        case ESP_RST_DEEPSLEEP: return "Deep-sleep wake";
        case ESP_RST_BROWNOUT:  return "Brownout";
        case ESP_RST_SDIO:      return "SDIO reset";
        case ESP_RST_USB:       return "USB reset";
        case ESP_RST_JTAG:      return "JTAG reset";
        default:                return "Unknown";
    }
}

void boot_info_init(void) {
    s_reset_reason = esp_reset_reason();
    bool valid = (rtc_magic == BOOT_INFO_MAGIC) && s_reset_reason != ESP_RST_POWERON;
    if (!valid) {
        rtc_magic = BOOT_INFO_MAGIC;
        rtc_boot_count = 1;
        s_prior_uptime_s = 0;
        strncpy(s_prior_action, "Power-on", sizeof(s_prior_action) - 1);
    } else {
        rtc_boot_count++;
        s_prior_uptime_s = rtc_uptime_s;
        memcpy(s_prior_action, rtc_action, sizeof(s_prior_action));
        s_prior_action[sizeof(s_prior_action) - 1] = '\0';
    }
    rtc_uptime_s = 0;
    boot_info_set_action("Booting");

    char up[24];
    boot_info_format_duration(s_prior_uptime_s, up, sizeof(up));
    Serial.printf("[BOOT] DeskBar AI v%s build %u (%s) | boot #%u | reset: %s | prior uptime %s | prior action: %s\n",
                  TUNEBAR_VERSION_STR, (unsigned)TUNEBAR_BUILD_NUM, TUNEBAR_GIT_HASH,
                  (unsigned)rtc_boot_count, reason_str(s_reset_reason), up, s_prior_action);
}

void boot_info_tick(void) { rtc_uptime_s = boot_info_uptime_s(); }

void boot_info_set_action(const char *action) {
    strncpy(rtc_action, action ? action : "", sizeof(rtc_action) - 1);
    rtc_action[sizeof(rtc_action) - 1] = '\0';
}

uint32_t boot_info_uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }
uint32_t boot_info_boot_count(void) { return rtc_boot_count; }
int boot_info_reset_reason(void) { return (int)s_reset_reason; }
const char *boot_info_reset_reason_str(void) { return reason_str(s_reset_reason); }
bool boot_info_last_reset_was_crash(void) {
    return s_reset_reason == ESP_RST_PANIC || s_reset_reason == ESP_RST_INT_WDT ||
           s_reset_reason == ESP_RST_TASK_WDT || s_reset_reason == ESP_RST_WDT ||
           s_reset_reason == ESP_RST_BROWNOUT;
}
uint32_t boot_info_prior_uptime_s(void) { return s_prior_uptime_s; }
const char *boot_info_prior_action(void) { return s_prior_action; }

const char *boot_info_version(void) { return TUNEBAR_VERSION_STR; }
uint32_t boot_info_build(void) { return TUNEBAR_BUILD_NUM; }
const char *boot_info_git_hash(void) { return TUNEBAR_GIT_HASH; }
const char *boot_info_build_date(void) { return TUNEBAR_BUILD_DATE; }

void boot_info_format_duration(uint32_t s, char *out, size_t outsz) {
    uint32_t d = s / 86400, h = (s / 3600) % 24, m = (s / 60) % 60, sec = s % 60;
    if (d) snprintf(out, outsz, "%ud %02uh %02um %02us", (unsigned)d, (unsigned)h, (unsigned)m, (unsigned)sec);
    else if (h) snprintf(out, outsz, "%uh %02um %02us", (unsigned)h, (unsigned)m, (unsigned)sec);
    else snprintf(out, outsz, "%um %02us", (unsigned)m, (unsigned)sec);
}
