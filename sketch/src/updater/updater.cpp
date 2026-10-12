#include "updater.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "network/network.h"
#include "ui/ui.h"
#include "battery/battery.h"
#include "ESP32-audioI2S-master/Audio.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>

const char *compile_date = TUNEBAR_BUILD_DATE;
const char *current_version = TUNEBAR_VERSION_STR;

bool firmware_checked = false;

static TaskHandle_t otaTaskHandle = NULL;
static volatile bool ota_abort = false;

// Neon Blue Full-Screen UI Elements
static lv_obj_t *s_neon_screen  = NULL;
static lv_obj_t *s_neon_title   = NULL;
static lv_obj_t *s_neon_caution = NULL;
static lv_obj_t *s_neon_target  = NULL;
static lv_obj_t *s_neon_bar     = NULL;
static lv_obj_t *s_neon_metrics = NULL;

// -------------------------------------------------------------
// Version String Parser to Integer
// -------------------------------------------------------------
int versionToNumber(const char *version) {
  if (!version || !*version) return 0;
  int major = 0, minor = 0, patch = 0;
  const char *p = version;
  if (*p == 'v' || *p == 'V') p++;
  while (*p && *p != '.') {
    if (*p >= '0' && *p <= '9') major = major * 10 + (*p - '0');
    p++;
  }
  if (*p == '.') p++; else return major * 10000;
  while (*p && *p != '.') {
    if (*p >= '0' && *p <= '9') minor = minor * 10 + (*p - '0');
    p++;
  }
  if (*p == '.') p++; else return major * 10000 + minor * 100;
  while (*p && *p >= '0' && *p <= '9') {
    patch = patch * 10 + (*p - '0');
    p++;
  }
  return major * 10000 + minor * 100 + patch;
}

// -------------------------------------------------------------
// Core OTA Safety Check & Teardown
// -------------------------------------------------------------
bool ota_check_battery_allowed(uint8_t *out_pct, float *out_v) {
  float v = 0.0f; uint8_t p = 0;
  getBatteryStatus(&v, &p);
  if (out_pct) *out_pct = p;
  if (out_v) *out_v = v;
  // Allow if battery >= 70% or if charging (voltage >= 4.15V)
  if (p >= 70 || v >= 4.15f) {
    return true;
  }
  return false;
}

extern "C" void bsp_set_backlight_power(bool enable);
extern void screenPowerOn(void);
extern void exit_clock_breathing(void);

void ota_pre_update_teardown(void) {
  log_i("[OTA TEARDOWN] Freeing application resources for firmware update...");
  extern Audio audio;
  if (audio.isRunning()) {
    audio.stopSong();
  }
  exit_clock_breathing();
  SCREEN_OFF_DELAY = 0; // Inhibit screen auto-off
  BL_OFF = false;
  bsp_set_backlight_power(true);
  screenPowerOn();
}

// -------------------------------------------------------------
// Bold Neon Blue Full-Screen Display APIs
// -------------------------------------------------------------
void ota_ui_show_neon_progress(const char *target_version) {
  char *ver_copy = strdup(target_version ? target_version : "v" TUNEBAR_VERSION_STR);
  lv_async_call([](void *p) {
    char *ver = (char *)p;
    if (s_neon_screen) {
      lv_obj_del(s_neon_screen);
      s_neon_screen = NULL;
    }

    // 1. Full-screen pitch-black container
    s_neon_screen = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_neon_screen);
    lv_obj_set_size(s_neon_screen, 640, 172);
    lv_obj_center(s_neon_screen);
    lv_obj_set_style_bg_color(s_neon_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_neon_screen, 255, 0);
    lv_obj_clear_flag(s_neon_screen, LV_OBJ_FLAG_SCROLLABLE);

    // 2. Bold Neon Blue Title
    s_neon_title = lv_label_create(s_neon_screen);
    lv_label_set_text(s_neon_title, "⚡ OTA UPDATE IN PROGRESS ⚡");
    lv_obj_set_style_text_color(s_neon_title, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(s_neon_title, &lv_font_montserrat_18, 0);
    lv_obj_align(s_neon_title, LV_ALIGN_TOP_MID, 0, 12);

    // 3. Warning / Caution Label
    s_neon_caution = lv_label_create(s_neon_screen);
    lv_label_set_text(s_neon_caution, "DO NOT TOUCH DEVICE OR DISCONNECT POWER CABLE");
    lv_obj_set_style_text_color(s_neon_caution, lv_color_hex(0x00B0FF), 0);
    lv_obj_set_style_text_font(s_neon_caution, &lv_font_montserrat_14, 0);
    lv_obj_align(s_neon_caution, LV_ALIGN_TOP_MID, 0, 38);

    // 4. Target Version Label
    s_neon_target = lv_label_create(s_neon_screen);
    char tbuf[96];
    snprintf(tbuf, sizeof(tbuf), "Installing: TuneBar %s (Build #%d - %s)", ver, TUNEBAR_BUILD_NUM, TUNEBAR_GIT_HASH);
    lv_label_set_text(s_neon_target, tbuf);
    lv_obj_set_style_text_color(s_neon_target, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_neon_target, &lv_font_montserrat_12, 0);
    lv_obj_align(s_neon_target, LV_ALIGN_TOP_MID, 0, 62);

    // 5. Neon Glowing Progress Bar
    s_neon_bar = lv_bar_create(s_neon_screen);
    lv_obj_set_size(s_neon_bar, 520, 16);
    lv_obj_align(s_neon_bar, LV_ALIGN_CENTER, 0, 14);
    lv_bar_set_range(s_neon_bar, 0, 100);
    lv_bar_set_value(s_neon_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_neon_bar, lv_color_hex(0x081828), 0);
    lv_obj_set_style_border_color(s_neon_bar, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_border_width(s_neon_bar, 2, 0);
    lv_obj_set_style_radius(s_neon_bar, 8, 0);
    lv_obj_set_style_bg_color(s_neon_bar, lv_color_hex(0x00E5FF), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_neon_bar, 6, LV_PART_INDICATOR);

    // 6. Metrics Label (Progress & Speed)
    s_neon_metrics = lv_label_create(s_neon_screen);
    lv_label_set_text(s_neon_metrics, "0% (0.00 / 0.00 MB)");
    lv_obj_set_style_text_color(s_neon_metrics, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(s_neon_metrics, &lv_font_montserrat_14, 0);
    lv_obj_align(s_neon_metrics, LV_ALIGN_BOTTOM_MID, 0, -12);

    free(ver);
  }, ver_copy);
}

struct NeonProgressData {
  uint8_t pct;
  size_t loaded;
  size_t total;
  uint32_t speed_kbs;
};

void ota_ui_update_progress(uint8_t pct, size_t loaded, size_t total, uint32_t speed_kbs) {
  NeonProgressData *d = (NeonProgressData *)malloc(sizeof(NeonProgressData));
  if (!d) return;
  d->pct = pct;
  d->loaded = loaded;
  d->total = total;
  d->speed_kbs = speed_kbs;

  lv_async_call([](void *p) {
    NeonProgressData *data = (NeonProgressData *)p;
    if (s_neon_bar) {
      lv_bar_set_value(s_neon_bar, data->pct, LV_ANIM_OFF);
    }
    if (s_neon_metrics) {
      char buf[64];
      float mb_loaded = (float)data->loaded / (1024.0f * 1024.0f);
      float mb_total  = (data->total > 0) ? ((float)data->total / (1024.0f * 1024.0f)) : mb_loaded;
      if (data->speed_kbs > 0) {
        snprintf(buf, sizeof(buf), "%u%% (%.2f / %.2f MB) • %u KB/s", (unsigned)data->pct, mb_loaded, mb_total, (unsigned)data->speed_kbs);
      } else {
        snprintf(buf, sizeof(buf), "%u%% (%.2f / %.2f MB)", (unsigned)data->pct, mb_loaded, mb_total);
      }
      lv_label_set_text(s_neon_metrics, buf);
    }
    free(data);
  }, d);
}

void ota_ui_show_complete(const char *build_str) {
  char *str_copy = strdup(build_str ? build_str : TUNEBAR_VERSION_STR);
  lv_async_call([](void *p) {
    char *b = (char *)p;
    if (s_neon_title) {
      lv_label_set_text(s_neon_title, "✅ UPDATE SUCCESSFUL! REBOOTING...");
      lv_obj_set_style_text_color(s_neon_title, lv_color_hex(0x00FF88), 0);
    }
    if (s_neon_bar) {
      lv_bar_set_value(s_neon_bar, 100, LV_ANIM_OFF);
      lv_obj_set_style_bg_color(s_neon_bar, lv_color_hex(0x00FF88), LV_PART_INDICATOR);
      lv_obj_set_style_border_color(s_neon_bar, lv_color_hex(0x00FF88), 0);
    }
    if (s_neon_metrics) {
      char buf[64];
      snprintf(buf, sizeof(buf), "Rebooting into v%s in 2s...", b);
      lv_label_set_text(s_neon_metrics, buf);
      lv_obj_set_style_text_color(s_neon_metrics, lv_color_hex(0x00FF88), 0);
    }
    free(b);
  }, str_copy);
}

void ota_ui_show_blocked_battery(uint8_t batt_pct) {
  lv_async_call([](void *p) {
    uintptr_t pct = (uintptr_t)p;
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, 540, 140);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x181002), 0);
    lv_obj_set_style_bg_opa(box, 255, 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0xFFA500), 0); // Amber warning
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "⚠️ OTA UPDATE BLOCKED: BATTERY LOW");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFA500), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_18, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_t *msg = lv_label_create(box);
    char msg_buf[128];
    snprintf(msg_buf, sizeof(msg_buf), "Battery is at %u%% (Minimum 70%% required).\nPlease connect USB charger to prevent bricking during flash.", (unsigned)pct);
    lv_label_set_text(msg, msg_buf);
    lv_obj_set_style_text_color(msg, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_align(msg, LV_ALIGN_CENTER, 0, 10);

    // Auto delete after 6 seconds
    lv_timer_create([](lv_timer_t *t) {
      lv_obj_t *obj = (lv_obj_t *)t->user_data;
      if (obj) lv_obj_del(obj);
      lv_timer_del(t);
    }, 6000, box);
  }, (void *)(uintptr_t)batt_pct);
}

// -------------------------------------------------------------
// Real HTTP Background OTA Flasher
// -------------------------------------------------------------
struct HttpOtaParams {
  char version[32];
  char url[256];
};

static void http_ota_task(void *pvParameters) {
  HttpOtaParams *params = (HttpOtaParams *)pvParameters;
  log_i("[OTA TASK] Starting background HTTP flash for %s from: %s", params->version, params->url);

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);

  WiFiClientSecure secureClient;
  secureClient.setInsecure(); // GitHub raw content over HTTPS

  bool beginOk = false;
  if (strncmp(params->url, "https://", 8) == 0) {
    beginOk = http.begin(secureClient, params->url);
  } else {
    beginOk = http.begin(params->url);
  }

  if (!beginOk) {
    log_e("[OTA TASK] HTTP begin failed!");
    free(params);
    vTaskDelete(NULL);
    return;
  }

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    log_e("[OTA TASK] HTTP GET failed with code: %d", httpCode);
    http.end();
    free(params);
    vTaskDelete(NULL);
    return;
  }

  int totalLength = http.getSize();
  log_i("[OTA TASK] Content-Length: %d bytes", totalLength);

  if (!Update.begin(totalLength > 0 ? totalLength : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    log_e("[OTA TASK] Update.begin failed!");
    http.end();
    free(params);
    vTaskDelete(NULL);
    return;
  }

  WiFiClient *stream = http.getStreamPtr();
  uint8_t buff[2048];
  size_t written = 0;
  uint32_t last_progress_ms = millis();
  uint32_t speed_bytes = 0;

  while (http.connected() && (totalLength <= 0 || written < (size_t)totalLength)) {
    size_t sizeAvailable = stream->available();
    if (sizeAvailable) {
      size_t readBytes = stream->readBytes(buff, ((sizeAvailable > sizeof(buff)) ? sizeof(buff) : sizeAvailable));
      size_t bytesWritten = Update.write(buff, readBytes);
      if (bytesWritten != readBytes) {
        log_e("[OTA TASK] Update write mismatch!");
        break;
      }
      written += bytesWritten;
      speed_bytes += bytesWritten;

      uint32_t now = millis();
      if (now - last_progress_ms >= 300) {
        uint32_t dt = now - last_progress_ms;
        uint32_t speed_kbs = (speed_bytes * 1000ULL) / (dt * 1024ULL);
        last_progress_ms = now;
        speed_bytes = 0;
        uint8_t pct = (totalLength > 0) ? (uint8_t)((written * 100ULL) / (size_t)totalLength) : 50;
        ota_ui_update_progress(pct, written, totalLength > 0 ? totalLength : written, speed_kbs);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }

  if (Update.end(true)) {
    log_i("[OTA TASK] Update completed successfully! Total bytes: %u", (unsigned)written);
    ota_ui_show_complete(params->version);
    http.end();
    free(params);
    delay(2000);
    ESP.restart();
  } else {
    log_e("[OTA TASK] Update.end failed with error: %d", Update.getError());
    http.end();
    free(params);
    vTaskDelete(NULL);
  }
}

bool ota_start_http_flash(const char *version_str, const char *bin_url) {
  uint8_t batt_pct = 0;
  float batt_v = 0.0f;
  if (!ota_check_battery_allowed(&batt_pct, &batt_v)) {
    log_w("[OTA] Blocked: Battery %u%%, %.2fV", (unsigned)batt_pct, batt_v);
    ota_ui_show_blocked_battery(batt_pct);
    return false;
  }

  ota_pre_update_teardown();
  ota_ui_show_neon_progress(version_str);

  HttpOtaParams *params = (HttpOtaParams *)malloc(sizeof(HttpOtaParams));
  if (!params) return false;
  snprintf(params->version, sizeof(params->version), "%s", version_str ? version_str : TUNEBAR_VERSION_STR);
  snprintf(params->url, sizeof(params->url), "%s", bin_url ? bin_url : "");

  xTaskCreatePinnedToCore(http_ota_task, "http_ota_task", 8192, params, 5, NULL, 1);
  return true;
}

void ota_post_boot_toast(void) {
  Preferences prefs;
  prefs.begin("ota_state", false);
  String last_ver = prefs.getString("last_ver", "");
  if (last_ver.length() > 0 && last_ver != TUNEBAR_VERSION_STR) {
    log_i("[OTA TOAST] Booted into new version %s (previous: %s)", TUNEBAR_VERSION_STR, last_ver.c_str());
    prefs.putString("last_ver", TUNEBAR_VERSION_STR);
    prefs.end();

    lv_async_call([](void *p) {
      lv_obj_t *toast = lv_obj_create(lv_layer_top());
      lv_obj_set_size(toast, 440, 36);
      lv_obj_align(toast, LV_ALIGN_TOP_MID, 0, 8);
      lv_obj_set_style_bg_color(toast, lv_color_hex(0x001B2E), 0);
      lv_obj_set_style_bg_opa(toast, 245, 0);
      lv_obj_set_style_border_color(toast, lv_color_hex(0x00E5FF), 0);
      lv_obj_set_style_border_width(toast, 2, 0);
      lv_obj_set_style_radius(toast, 18, 0);
      lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);

      lv_obj_t *lbl = lv_label_create(toast);
      char buf[64];
      snprintf(buf, sizeof(buf), "✨ Successfully updated to v%s (Build #%d)", TUNEBAR_VERSION_STR, TUNEBAR_BUILD_NUM);
      lv_label_set_text(lbl, buf);
      lv_obj_set_style_text_color(lbl, lv_color_hex(0x00E5FF), 0);
      lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
      lv_obj_center(lbl);

      lv_timer_create([](lv_timer_t *t) {
        lv_obj_t *obj = (lv_obj_t *)t->user_data;
        if (obj) lv_obj_del(obj);
        lv_timer_del(t);
      }, 4000, toast);
    }, NULL);
  } else {
    if (last_ver.length() == 0) {
      prefs.putString("last_ver", TUNEBAR_VERSION_STR);
    }
    prefs.end();
  }
}

// -------------------------------------------------------------
// Legacy manifest fetcher for compatibility
// -------------------------------------------------------------
const char *newFirmwareAvailable() {
  return nullptr;
}

void ota_show_popup() {
  ota_ui_show_neon_progress("v" TUNEBAR_VERSION_STR);
}

void notifyUpdate(const char *latestVer) {
  // Handled via on-device firmware manager
}

void memoryInfo(char *buf, size_t len) {
  size_t ifree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t itotal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  size_t iused = (itotal > ifree) ? (itotal - ifree) : 0;
  uint8_t ipct = (itotal > 0) ? (uint8_t)(iused * 100 / itotal) : 0;

  size_t pfree = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t ptotal = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  size_t pused = (ptotal > pfree) ? (ptotal - pfree) : 0;
  uint8_t ppct = (ptotal > 0) ? (uint8_t)(pused * 100 / ptotal) : 0;

  snprintf(buf, len,
           "DRAM (Int): %u / %u KB (%u%%, %u KB free)\n"
           "PSRAM(Ext): %.1f / %.1f MB (%u%%, %.1f MB free)",
           (unsigned)(iused / 1024), (unsigned)(itotal / 1024), ipct, (unsigned)(ifree / 1024),
           (float)pused / (1024.0f * 1024.0f), (float)ptotal / (1024.0f * 1024.0f), ppct, (float)pfree / (1024.0f * 1024.0f));
}

void systemInfo(char *buf, size_t len) {
  size_t used = LittleFS.usedBytes();
  size_t total = LittleFS.totalBytes();
  float pct_free = (float)(total - used) * 100.0f / (float)total;
  uint64_t mac = ESP.getEfuseMac();
  snprintf(buf, len,
           "TuneBar v%s (Rahul Mod)\n"
           "Build  : #%d (%s)\n"
           "MAC    : %04X%08X\n"
           "Flash  : %u / %u KB (%.0f%% free)",
           current_version, TUNEBAR_BUILD_NUM, TUNEBAR_GIT_HASH,
           (uint16_t)(mac >> 32), (uint32_t)mac,
           (unsigned)(used / 1024), (unsigned)(total / 1024), pct_free);
}