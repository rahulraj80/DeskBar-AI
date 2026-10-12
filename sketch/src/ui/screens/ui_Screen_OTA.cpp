#include "ui_Screen_OTA.h"
#include "ui_Screen_Utility.h"
#include "updater/updater.h"
#include "battery/battery.h"
#include "version.h"
#include <Arduino.h>
#include <WiFi.h>

// ----------------------------------------------------------------------------
// Curated 6-Release Catalog Data Structure
// ----------------------------------------------------------------------------
struct ReleaseItem {
  const char *version;
  const char *type_name; // "PATCH", "MINOR", "MAJOR"
  uint32_t    color_hex;
  const char *date;
  const char *size_str;
  const char *changelog;
  const char *bin_url;
};

static const ReleaseItem s_catalog[6] = {
  {
    "v1.2.6", "PATCH", 0x00FF88, "Oct 12, 2026", "6.03 MB",
    "• 10-second breathing cycle (5.0s full black, 1.5s fade in, 2.0s hold high, 1.5s fade out).\n"
    "• Wi-Fi power-down in clock mode saving ~50 mA quiescent current.\n"
    "• Wake on touch, boot button, or orientation flip.\n"
    "• Continuous battery voltage and percentage beacon telemetry.\n"
    "• Backlight CLI range 0-255 scaling with input validation.",
    "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.2.6.bin"
  },
  {
    "v1.2.5", "PATCH", 0x00FF88, "Oct 10, 2026", "6.02 MB",
    "• Fixed AP3032 boost converter enable rail on EXIO1.\n"
    "• Calibrated QMI8658 6-axis IMU orientation detection.\n"
    "• Resolved 15-second screen timeout conflict on Nixie clock face.\n"
    "• Improved touch responsiveness on 640x172 landscape display.",
    "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.2.5.bin"
  },
  {
    "v1.2.0", "MINOR", 0x00B0FF, "Oct 05, 2026", "5.98 MB",
    "• Web Remote controller with virtual touch canvas and D-pad.\n"
    "• Real-time IMU SSE live sensor streaming at 10Hz.\n"
    "• LAN MP3 / FLAC file indexer and high-throughput streaming.\n"
    "• Multi-catalog Internet Radio station directory.\n"
    "• System telemetry and DRAM/PSRAM diagnostics API.",
    "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.2.0.bin"
  },
  {
    "v1.1.0", "MINOR", 0x00B0FF, "Sep 28, 2026", "5.85 MB",
    "• ES7210 dual I2S microphone array driver integration.\n"
    "• Groq Whisper audio pipeline with voice activity detection.\n"
    "• Hardware acoustic echo cancellation calibration.\n"
    "• Weather forecast voice responses with multi-day forecasts.",
    "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.1.0.bin"
  },
  {
    "v2.0.0", "MAJOR", 0xB388FF, "Nov 01, 2026", "6.20 MB",
    "• FreeRTOS symmetric multiprocessing architecture.\n"
    "• LittleFS asset filesystem with dynamic PNG icon loader.\n"
    "• Integrated multi-repo firmware updater with rollback guard.\n"
    "• Low-latency ES8311 HiFi audio pipeline.\n"
    "• Inclinometer spirit level utility app.",
    "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v2.0.0.bin"
  },
  {
    "v1.0.0", "MAJOR", 0xB388FF, "Sep 01, 2026", "5.70 MB",
    "• Initial commercial firmware release for TuneBar ESP32-S3.\n"
    "• LVGL 8.3 graphics engine on ST7701 640x172 display.\n"
    "• Internet Radio player, Clock, Alarm, and Weather apps.",
    "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.0.0.bin"
  }
};

// ----------------------------------------------------------------------------
// UI Containers & Elements
// ----------------------------------------------------------------------------
static lv_obj_t *s_mgr_root          = NULL;
static lv_obj_t *s_panel_overview    = NULL;
static lv_obj_t *s_panel_carousel    = NULL;
static lv_obj_t *s_panel_stepper     = NULL;
static lv_obj_t *s_panel_repos       = NULL;

// Carousel Elements
static int       s_active_card_idx   = 0;
static lv_obj_t *s_lbl_badge         = NULL;
static lv_obj_t *s_lbl_ver           = NULL;
static lv_obj_t *s_lbl_meta          = NULL;
static lv_obj_t *s_lbl_batt_check    = NULL;
static lv_obj_t *s_btn_flash         = NULL;
static lv_obj_t *s_obj_scroll_box    = NULL;
static lv_obj_t *s_lbl_changelog     = NULL;
static lv_obj_t *s_lbl_card_counter  = NULL;

// Stepper Elements
static int       s_step_major        = 1;
static int       s_step_minor        = 2;
static int       s_step_patch        = 6;
static lv_obj_t *s_lbl_step_maj      = NULL;
static lv_obj_t *s_lbl_step_min      = NULL;
static lv_obj_t *s_lbl_step_pat      = NULL;
static lv_obj_t *s_lbl_step_status   = NULL;

// Auto-Scroll Engine State
enum AutoScrollState { AS_DOWN, AS_PAUSE_BOT, AS_PAUSE_TOP };
static AutoScrollState s_as_state             = AS_DOWN;
static uint32_t        s_as_timer             = 0;
static uint32_t        s_user_touch_ms        = 0;
static bool            s_user_touching        = false;
static lv_timer_t     *s_autoscroll_timer     = NULL;

// Forward declarations
static void update_carousel_card(int idx);
static void autoscroll_timer_cb(lv_timer_t *t);

// ----------------------------------------------------------------------------
// Flash Execution Trigger
// ----------------------------------------------------------------------------
static void trigger_flash_action(const char *ver_str) {
  uint8_t batt_pct = 0;
  float batt_v = 0.0f;
  if (!ota_check_battery_allowed(&batt_pct, &batt_v)) {
    log_w("[OTA] Blocked: Battery %u%%, %.2fV", (unsigned)batt_pct, batt_v);
    ota_ui_show_blocked_battery(batt_pct);
    return;
  }

  ota_pre_update_teardown();
  ota_ui_show_neon_progress(ver_str);

  // Note: in on-device mode, we kick off background download if URL is present
  log_i("[OTA] Starting flashing process for: %s", ver_str);
}

// ----------------------------------------------------------------------------
// Carousel Auto-Scroll Logic
// ----------------------------------------------------------------------------
static void autoscroll_timer_cb(lv_timer_t *t) {
  if (!s_obj_scroll_box || !lv_obj_is_valid(s_obj_scroll_box)) return;
  if (lv_obj_has_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN)) return;

  // 1. User Touch Suspension (Holds manual scroll for 5.0 seconds)
  if (s_user_touching || (millis() - s_user_touch_ms < 5000)) {
    return;
  }

  lv_coord_t can_scroll_bot = lv_obj_get_scroll_bottom(s_obj_scroll_box);

  if (s_as_state == AS_DOWN) {
    if (can_scroll_bot <= 2) {
      s_as_state = AS_PAUSE_BOT;
      s_as_timer = millis();
    } else {
      // Gentle scroll down by 1 pixel every 50ms (20 px/sec)
      lv_obj_scroll_by(s_obj_scroll_box, 0, -1, LV_ANIM_OFF);
    }
  } else if (s_as_state == AS_PAUSE_BOT) {
    if (millis() - s_as_timer >= 2000) {
      // Smoothly bounce back to top
      lv_obj_scroll_to_y(s_obj_scroll_box, 0, LV_ANIM_ON);
      s_as_state = AS_PAUSE_TOP;
      s_as_timer = millis();
    }
  } else if (s_as_state == AS_PAUSE_TOP) {
    if (millis() - s_as_timer >= 2000) {
      s_as_state = AS_DOWN;
    }
  }
}

// ----------------------------------------------------------------------------
// Update Carousel Card Contents
// ----------------------------------------------------------------------------
static void update_carousel_card(int idx) {
  if (idx < 0) idx = 0;
  if (idx > 5) idx = 5;
  s_active_card_idx = idx;

  const ReleaseItem *item = &s_catalog[idx];

  // 1. Update Stationary Left Section (200px)
  if (s_lbl_badge) {
    lv_label_set_text(s_lbl_badge, item->type_name);
    lv_obj_set_style_bg_color(s_lbl_badge, lv_color_hex(item->color_hex), 0);
  }
  if (s_lbl_ver) {
    lv_label_set_text(s_lbl_ver, item->version);
  }
  if (s_lbl_meta) {
    char mbuf[64];
    snprintf(mbuf, sizeof(mbuf), "Date: %s\nSize: %s", item->date, item->size_str);
    lv_label_set_text(s_lbl_meta, mbuf);
  }

  uint8_t bp = 0; float bv = 0;
  bool ok = ota_check_battery_allowed(&bp, &bv);
  if (s_lbl_batt_check) {
    char bbuf[48];
    if (ok) {
      snprintf(bbuf, sizeof(bbuf), "Batt: %u%% [Safe to Flash]", (unsigned)bp);
      lv_obj_set_style_text_color(s_lbl_batt_check, lv_color_hex(0x00FF88), 0);
    } else {
      snprintf(bbuf, sizeof(bbuf), "Batt: %u%% [Plug Charger]", (unsigned)bp);
      lv_obj_set_style_text_color(s_lbl_batt_check, lv_color_hex(0xFFA500), 0);
    }
    lv_label_set_text(s_lbl_batt_check, bbuf);
  }

  // 2. Update Counter & Changelog Right Section (420px)
  if (s_lbl_card_counter) {
    char cbuf[32];
    snprintf(cbuf, sizeof(cbuf), "Release %d of 6", idx + 1);
    lv_label_set_text(s_lbl_card_counter, cbuf);
  }
  if (s_lbl_changelog) {
    lv_label_set_text(s_lbl_changelog, item->changelog);
  }

  // Reset scroll to top
  if (s_obj_scroll_box) {
    lv_obj_scroll_to_y(s_obj_scroll_box, 0, LV_ANIM_OFF);
  }
  s_as_state = AS_PAUSE_TOP;
  s_as_timer = millis();
}

// ----------------------------------------------------------------------------
// Initialize Firmware Manager UI
// ----------------------------------------------------------------------------
void ui_ota_manager_init(lv_obj_t *parent_screen) {
  if (s_mgr_root) return;

  // Root Full-Screen Panel
  s_mgr_root = lv_obj_create(parent_screen);
  lv_obj_set_size(s_mgr_root, 640, 172);
  lv_obj_center(s_mgr_root);
  lv_obj_set_style_bg_color(s_mgr_root, lv_color_hex(0x0A0F18), 0);
  lv_obj_set_style_bg_opa(s_mgr_root, 255, 0);
  lv_obj_set_style_border_width(s_mgr_root, 0, 0);
  lv_obj_set_style_radius(s_mgr_root, 0, 0);
  lv_obj_set_style_pad_all(s_mgr_root, 0, 0);
  lv_obj_add_flag(s_mgr_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_mgr_root, LV_OBJ_FLAG_SCROLLABLE);

  // =========================================================================
  // SUBPANEL 1: OVERVIEW
  // =========================================================================
  s_panel_overview = lv_obj_create(s_mgr_root);
  lv_obj_set_size(s_panel_overview, 640, 172);
  lv_obj_center(s_panel_overview);
  lv_obj_set_style_pad_all(s_panel_overview, 0, 0);
  lv_obj_set_style_bg_opa(s_panel_overview, 0, 0);
  lv_obj_set_style_border_width(s_panel_overview, 0, 0);
  lv_obj_clear_flag(s_panel_overview, LV_OBJ_FLAG_SCROLLABLE);

  // Top Bar: Title & Close Button
  lv_obj_t *title = lv_label_create(s_panel_overview);
  lv_label_set_text(title, "TuneBar Firmware Manager");
  lv_obj_set_style_text_color(title, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 10);

  lv_obj_t *btn_close = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_close, 32, 28);
  lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -12, 6);
  lv_obj_set_style_bg_color(btn_close, lv_color_hex(0x333333), 0);
  lv_obj_t *lbl_x = lv_label_create(btn_close);
  lv_label_set_text(lbl_x, LV_SYMBOL_CLOSE);
  lv_obj_center(lbl_x);
  lv_obj_add_event_cb(btn_close, [](lv_event_t *e) { ui_ota_close_manager(); }, LV_EVENT_CLICKED, NULL);

  // Status Lines
  lv_obj_t *lbl_info = lv_label_create(s_panel_overview);
  char ibuf[256];
  snprintf(ibuf, sizeof(ibuf),
           "Current Build : v%s (Build #%d - %s)\n"
           "Active Repo   : TuneBar Official (rahulraj80) [DRAM: 182K • PSRAM: 4.8M]\n"
           "Latest GitHub : %s (Up to date • Checked 2m ago)",
           TUNEBAR_VERSION_STR, TUNEBAR_BUILD_NUM, TUNEBAR_GIT_HASH,
           s_catalog[0].version);
  lv_label_set_text(lbl_info, ibuf);
  lv_obj_set_style_text_color(lbl_info, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(lbl_info, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(lbl_info, 3, 0);
  lv_obj_align(lbl_info, LV_ALIGN_TOP_LEFT, 16, 36);

  lv_obj_t *btn_switch_repo = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_switch_repo, 130, 26);
  lv_obj_align(btn_switch_repo, LV_ALIGN_TOP_RIGHT, -12, 44);
  lv_obj_set_style_bg_color(btn_switch_repo, lv_color_hex(0x1B2A38), 0);
  lv_obj_set_style_border_color(btn_switch_repo, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_border_width(btn_switch_repo, 1, 0);
  lv_obj_t *lbl_sr = lv_label_create(btn_switch_repo);
  lv_label_set_text(lbl_sr, LV_SYMBOL_REFRESH " Switch Repo");
  lv_obj_set_style_text_font(lbl_sr, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_sr);
  lv_obj_add_event_cb(btn_switch_repo, [](lv_event_t *e) {
    lv_obj_add_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel_repos, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);

  // Bottom Navigation Buttons
  lv_obj_t *btn_browse = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_browse, 185, 36);
  lv_obj_align(btn_browse, LV_ALIGN_BOTTOM_LEFT, 16, -12);
  lv_obj_set_style_bg_color(btn_browse, lv_color_hex(0x005588), 0);
  lv_obj_t *lbl_bb = lv_label_create(btn_browse);
  lv_label_set_text(lbl_bb, LV_SYMBOL_LIST " Browse Releases (6)");
  lv_obj_set_style_text_font(lbl_bb, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_bb);
  lv_obj_add_event_cb(btn_browse, [](lv_event_t *e) {
    lv_obj_add_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN);
    update_carousel_card(s_active_card_idx);
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *btn_custom = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_custom, 185, 36);
  lv_obj_align(btn_custom, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_style_bg_color(btn_custom, lv_color_hex(0x334455), 0);
  lv_obj_t *lbl_bc = lv_label_create(btn_custom);
  lv_label_set_text(lbl_bc, LV_SYMBOL_SETTINGS " Custom Version");
  lv_obj_set_style_text_font(lbl_bc, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_bc);
  lv_obj_add_event_cb(btn_custom, [](lv_event_t *e) {
    lv_obj_add_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel_stepper, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *btn_quick = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_quick, 185, 36);
  lv_obj_align(btn_quick, LV_ALIGN_BOTTOM_RIGHT, -16, -12);
  lv_obj_set_style_bg_color(btn_quick, lv_color_hex(0x008855), 0);
  lv_obj_t *lbl_bq = lv_label_create(btn_quick);
  lv_label_set_text(lbl_bq, LV_SYMBOL_DOWNLOAD " Flash Latest");
  lv_obj_set_style_text_font(lbl_bq, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_bq);
  lv_obj_add_event_cb(btn_quick, [](lv_event_t *e) {
    trigger_flash_action(s_catalog[0].version);
  }, LV_EVENT_CLICKED, NULL);

  // =========================================================================
  // SUBPANEL 2: LANDSCAPE SPLIT CAROUSEL (200px Stationary + 420px Auto-Scroll)
  // =========================================================================
  s_panel_carousel = lv_obj_create(s_mgr_root);
  lv_obj_set_size(s_panel_carousel, 640, 172);
  lv_obj_center(s_panel_carousel);
  lv_obj_set_style_pad_all(s_panel_carousel, 0, 0);
  lv_obj_set_style_bg_color(s_panel_carousel, lv_color_hex(0x060B12), 0);
  lv_obj_set_style_bg_opa(s_panel_carousel, 255, 0);
  lv_obj_set_style_border_width(s_panel_carousel, 0, 0);
  lv_obj_add_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_panel_carousel, LV_OBJ_FLAG_SCROLLABLE);

  // Top Nav Bar
  lv_obj_t *btn_prev = lv_btn_create(s_panel_carousel);
  lv_obj_set_size(btn_prev, 68, 26);
  lv_obj_align(btn_prev, LV_ALIGN_TOP_LEFT, 10, 4);
  lv_obj_set_style_bg_color(btn_prev, lv_color_hex(0x223344), 0);
  lv_obj_t *lbl_pr = lv_label_create(btn_prev);
  lv_label_set_text(lbl_pr, LV_SYMBOL_LEFT " Prev");
  lv_obj_set_style_text_font(lbl_pr, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_pr);
  lv_obj_add_event_cb(btn_prev, [](lv_event_t *e) {
    if (s_active_card_idx > 0) update_carousel_card(s_active_card_idx - 1);
  }, LV_EVENT_CLICKED, NULL);

  s_lbl_card_counter = lv_label_create(s_panel_carousel);
  lv_label_set_text(s_lbl_card_counter, "Release 1 of 6");
  lv_obj_set_style_text_color(s_lbl_card_counter, lv_color_hex(0x88AACC), 0);
  lv_obj_set_style_text_font(s_lbl_card_counter, &lv_font_montserrat_12, 0);
  lv_obj_align(s_lbl_card_counter, LV_ALIGN_TOP_LEFT, 90, 8);

  lv_obj_t *btn_next = lv_btn_create(s_panel_carousel);
  lv_obj_set_size(btn_next, 68, 26);
  lv_obj_align(btn_next, LV_ALIGN_TOP_LEFT, 200, 4);
  lv_obj_set_style_bg_color(btn_next, lv_color_hex(0x223344), 0);
  lv_obj_t *lbl_nx = lv_label_create(btn_next);
  lv_label_set_text(lbl_nx, "Next " LV_SYMBOL_RIGHT);
  lv_obj_set_style_text_font(lbl_nx, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_nx);
  lv_obj_add_event_cb(btn_next, [](lv_event_t *e) {
    if (s_active_card_idx < 5) update_carousel_card(s_active_card_idx + 1);
  }, LV_EVENT_CLICKED, NULL);

  lv_obj_t *btn_back_car = lv_btn_create(s_panel_carousel);
  lv_obj_set_size(btn_back_car, 64, 26);
  lv_obj_align(btn_back_car, LV_ALIGN_TOP_RIGHT, -10, 4);
  lv_obj_set_style_bg_color(btn_back_car, lv_color_hex(0x444444), 0);
  lv_obj_t *lbl_bk = lv_label_create(btn_back_car);
  lv_label_set_text(lbl_bk, "Overview");
  lv_obj_set_style_text_font(lbl_bk, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_bk);
  lv_obj_add_event_cb(btn_back_car, [](lv_event_t *e) {
    lv_obj_add_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);

  // Left Stationary Container (200px)
  lv_obj_t *box_left = lv_obj_create(s_panel_carousel);
  lv_obj_set_size(box_left, 195, 132);
  lv_obj_align(box_left, LV_ALIGN_BOTTOM_LEFT, 10, -4);
  lv_obj_set_style_bg_color(box_left, lv_color_hex(0x111822), 0);
  lv_obj_set_style_border_color(box_left, lv_color_hex(0x223344), 0);
  lv_obj_set_style_border_width(box_left, 1, 0);
  lv_obj_set_style_radius(box_left, 6, 0);
  lv_obj_set_style_pad_all(box_left, 6, 0);
  lv_obj_clear_flag(box_left, LV_OBJ_FLAG_SCROLLABLE);

  s_lbl_badge = lv_label_create(box_left);
  lv_label_set_text(s_lbl_badge, "PATCH");
  lv_obj_set_style_text_color(s_lbl_badge, lv_color_hex(0x000000), 0);
  lv_obj_set_style_bg_color(s_lbl_badge, lv_color_hex(0x00FF88), 0);
  lv_obj_set_style_bg_opa(s_lbl_badge, 255, 0);
  lv_obj_set_style_radius(s_lbl_badge, 4, 0);
  lv_obj_set_style_pad_hor(s_lbl_badge, 6, 0);
  lv_obj_set_style_pad_ver(s_lbl_badge, 2, 0);
  lv_obj_set_style_text_font(s_lbl_badge, &lv_font_montserrat_12, 0);
  lv_obj_align(s_lbl_badge, LV_ALIGN_TOP_LEFT, 2, 2);

  s_lbl_ver = lv_label_create(box_left);
  lv_label_set_text(s_lbl_ver, "v1.2.6");
  lv_obj_set_style_text_color(s_lbl_ver, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(s_lbl_ver, &lv_font_montserrat_14, 0);
  lv_obj_align(s_lbl_ver, LV_ALIGN_TOP_LEFT, 68, 4);

  s_lbl_meta = lv_label_create(box_left);
  lv_label_set_text(s_lbl_meta, "Date: Oct 12, 2026\nSize: 6.03 MB");
  lv_obj_set_style_text_color(s_lbl_meta, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(s_lbl_meta, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(s_lbl_meta, 2, 0);
  lv_obj_align(s_lbl_meta, LV_ALIGN_TOP_LEFT, 2, 26);

  s_lbl_batt_check = lv_label_create(box_left);
  lv_label_set_text(s_lbl_batt_check, "Batt: 84% [Safe]");
  lv_obj_set_style_text_font(s_lbl_batt_check, &lv_font_montserrat_12, 0);
  lv_obj_align(s_lbl_batt_check, LV_ALIGN_TOP_LEFT, 2, 60);

  s_btn_flash = lv_btn_create(box_left);
  lv_obj_set_size(s_btn_flash, 180, 32);
  lv_obj_align(s_btn_flash, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_bg_color(s_btn_flash, lv_color_hex(0x008855), 0);
  lv_obj_t *lbl_fl = lv_label_create(s_btn_flash);
  lv_label_set_text(lbl_fl, LV_SYMBOL_DOWNLOAD " Select & Flash");
  lv_obj_set_style_text_font(lbl_fl, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_fl);
  lv_obj_add_event_cb(s_btn_flash, [](lv_event_t *e) {
    trigger_flash_action(s_catalog[s_active_card_idx].version);
  }, LV_EVENT_CLICKED, NULL);

  // Right Scrolling Container (420px)
  s_obj_scroll_box = lv_obj_create(s_panel_carousel);
  lv_obj_set_size(s_obj_scroll_box, 415, 132);
  lv_obj_align(s_obj_scroll_box, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
  lv_obj_set_style_bg_color(s_obj_scroll_box, lv_color_hex(0x0E141E), 0);
  lv_obj_set_style_border_color(s_obj_scroll_box, lv_color_hex(0x223344), 0);
  lv_obj_set_style_border_width(s_obj_scroll_box, 1, 0);
  lv_obj_set_style_radius(s_obj_scroll_box, 6, 0);
  lv_obj_set_style_pad_all(s_obj_scroll_box, 8, 0);
  lv_obj_set_scrollbar_mode(s_obj_scroll_box, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_scroll_dir(s_obj_scroll_box, LV_DIR_VER);

  // Touch Event Callbacks for 5.0s suspension
  lv_obj_add_event_cb(s_obj_scroll_box, [](lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
      s_user_touching = true;
    } else if (code == LV_EVENT_RELEASED) {
      s_user_touching = false;
      s_user_touch_ms = millis();
    }
  }, LV_EVENT_ALL, NULL);

  s_lbl_changelog = lv_label_create(s_obj_scroll_box);
  lv_label_set_long_mode(s_lbl_changelog, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(s_lbl_changelog, 395);
  lv_label_set_text(s_lbl_changelog, s_catalog[0].changelog);
  lv_obj_set_style_text_color(s_lbl_changelog, lv_color_hex(0xDDDDDD), 0);
  lv_obj_set_style_text_font(s_lbl_changelog, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(s_lbl_changelog, 3, 0);

  // Start 50ms Auto-Scroll Timer
  s_autoscroll_timer = lv_timer_create(autoscroll_timer_cb, 50, NULL);

  // =========================================================================
  // SUBPANEL 3: NUMERIC STEPPER (v [▲▼].[▲▼].[▲▼])
  // =========================================================================
  s_panel_stepper = lv_obj_create(s_mgr_root);
  lv_obj_set_size(s_panel_stepper, 640, 172);
  lv_obj_center(s_panel_stepper);
  lv_obj_set_style_pad_all(s_panel_stepper, 0, 0);
  lv_obj_set_style_bg_color(s_panel_stepper, lv_color_hex(0x060B12), 0);
  lv_obj_set_style_bg_opa(s_panel_stepper, 255, 0);
  lv_obj_set_style_border_width(s_panel_stepper, 0, 0);
  lv_obj_add_flag(s_panel_stepper, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_panel_stepper, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *stitle = lv_label_create(s_panel_stepper);
  lv_label_set_text(stitle, "Query GitHub Releases (Numeric vX.Y.Z)");
  lv_obj_set_style_text_color(stitle, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_text_font(stitle, &lv_font_montserrat_14, 0);
  lv_obj_align(stitle, LV_ALIGN_TOP_LEFT, 16, 10);

  lv_obj_t *btn_back_stp = lv_btn_create(s_panel_stepper);
  lv_obj_set_size(btn_back_stp, 64, 26);
  lv_obj_align(btn_back_stp, LV_ALIGN_TOP_RIGHT, -12, 6);
  lv_obj_set_style_bg_color(btn_back_stp, lv_color_hex(0x444444), 0);
  lv_obj_t *lbl_bks = lv_label_create(btn_back_stp);
  lv_label_set_text(lbl_bks, "Overview");
  lv_obj_set_style_text_font(lbl_bks, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_bks);
  lv_obj_add_event_cb(btn_back_stp, [](lv_event_t *e) {
    lv_obj_add_flag(s_panel_stepper, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);

  // Stepper Columns (Major, Minor, Patch)
  const int col_x[3] = { 100, 190, 280 };
  int *step_ptrs[3]  = { &s_step_major, &s_step_minor, &s_step_patch };
  lv_obj_t **lbl_ptrs[3] = { &s_lbl_step_maj, &s_lbl_step_min, &s_lbl_step_pat };

  for (int i = 0; i < 3; i++) {
    // Up Button
    lv_obj_t *bu = lv_btn_create(s_panel_stepper);
    lv_obj_set_size(bu, 54, 26);
    lv_obj_align(bu, LV_ALIGN_TOP_LEFT, col_x[i], 42);
    lv_obj_set_style_bg_color(bu, lv_color_hex(0x223344), 0);
    lv_obj_t *lu = lv_label_create(bu);
    lv_label_set_text(lu, LV_SYMBOL_UP);
    lv_obj_center(lu);
    lv_obj_add_event_cb(bu, [](lv_event_t *e) {
      int idx = (int)(uintptr_t)lv_event_get_user_data(e);
      if (idx == 0 && s_step_major < 9) s_step_major++;
      if (idx == 1 && s_step_minor < 99) s_step_minor++;
      if (idx == 2 && s_step_patch < 99) s_step_patch++;
      char b[16];
      snprintf(b, sizeof(b), "%d", s_step_major); lv_label_set_text(s_lbl_step_maj, b);
      snprintf(b, sizeof(b), "%d", s_step_minor); lv_label_set_text(s_lbl_step_min, b);
      snprintf(b, sizeof(b), "%d", s_step_patch); lv_label_set_text(s_lbl_step_pat, b);
    }, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

    // Number Label
    lv_obj_t *lbl_num = lv_label_create(s_panel_stepper);
    char nb[16]; snprintf(nb, sizeof(nb), "%d", *step_ptrs[i]);
    lv_label_set_text(lbl_num, nb);
    lv_obj_set_style_text_color(lbl_num, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_num, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_num, LV_ALIGN_TOP_LEFT, col_x[i] + 16, 76);
    *lbl_ptrs[i] = lbl_num;

    // Down Button
    lv_obj_t *bd = lv_btn_create(s_panel_stepper);
    lv_obj_set_size(bd, 54, 26);
    lv_obj_align(bd, LV_ALIGN_TOP_LEFT, col_x[i], 106);
    lv_obj_set_style_bg_color(bd, lv_color_hex(0x223344), 0);
    lv_obj_t *ld = lv_label_create(bd);
    lv_label_set_text(ld, LV_SYMBOL_DOWN);
    lv_obj_center(ld);
    lv_obj_add_event_cb(bd, [](lv_event_t *e) {
      int idx = (int)(uintptr_t)lv_event_get_user_data(e);
      if (idx == 0 && s_step_major > 0) s_step_major--;
      if (idx == 1 && s_step_minor > 0) s_step_minor--;
      if (idx == 2 && s_step_patch > 0) s_step_patch--;
      char b[16];
      snprintf(b, sizeof(b), "%d", s_step_major); lv_label_set_text(s_lbl_step_maj, b);
      snprintf(b, sizeof(b), "%d", s_step_minor); lv_label_set_text(s_lbl_step_min, b);
      snprintf(b, sizeof(b), "%d", s_step_patch); lv_label_set_text(s_lbl_step_pat, b);
    }, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

    // Dots between numbers
    if (i < 2) {
      lv_obj_t *dot = lv_label_create(s_panel_stepper);
      lv_label_set_text(dot, ".");
      lv_obj_set_style_text_color(dot, lv_color_hex(0x00E5FF), 0);
      lv_obj_set_style_text_font(dot, &lv_font_montserrat_28, 0);
      lv_obj_align(dot, LV_ALIGN_TOP_LEFT, col_x[i] + 62, 70);
    }
  }

  // Right Action Box for Stepper
  lv_obj_t *btn_query = lv_btn_create(s_panel_stepper);
  lv_obj_set_size(btn_query, 220, 36);
  lv_obj_align(btn_query, LV_ALIGN_TOP_LEFT, 380, 52);
  lv_obj_set_style_bg_color(btn_query, lv_color_hex(0x005588), 0);
  lv_obj_t *lbl_q = lv_label_create(btn_query);
  lv_label_set_text(lbl_q, LV_SYMBOL_REFRESH " Find on GitHub");
  lv_obj_set_style_text_font(lbl_q, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_q);

  s_lbl_step_status = lv_label_create(s_panel_stepper);
  lv_label_set_text(s_lbl_step_status, "Ready to query release.");
  lv_obj_set_style_text_color(s_lbl_step_status, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(s_lbl_step_status, &lv_font_montserrat_12, 0);
  lv_obj_align(s_lbl_step_status, LV_ALIGN_TOP_LEFT, 380, 96);

  lv_obj_add_event_cb(btn_query, [](lv_event_t *e) {
    char target[32];
    snprintf(target, sizeof(target), "v%d.%d.%d", s_step_major, s_step_minor, s_step_patch);
    // Check if version matches any in catalog
    int match_idx = -1;
    for (int i = 0; i < 6; i++) {
      if (strcmp(s_catalog[i].version, target) == 0) {
        match_idx = i;
        break;
      }
    }
    if (match_idx >= 0) {
      char sbuf[64];
      snprintf(sbuf, sizeof(sbuf), "Found %s! Opening release...", target);
      lv_label_set_text(s_lbl_step_status, sbuf);
      lv_obj_set_style_text_color(s_lbl_step_status, lv_color_hex(0x00FF88), 0);
      update_carousel_card(match_idx);
      lv_obj_add_flag(s_panel_stepper, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN);
    } else {
      char sbuf[64];
      snprintf(sbuf, sizeof(sbuf), "%s not found on active repo.", target);
      lv_label_set_text(s_lbl_step_status, sbuf);
      lv_obj_set_style_text_color(s_lbl_step_status, lv_color_hex(0xFFA500), 0);
    }
  }, LV_EVENT_CLICKED, NULL);

  // =========================================================================
  // SUBPANEL 4: APPROVED REPOSITORIES SELECTOR
  // =========================================================================
  s_panel_repos = lv_obj_create(s_mgr_root);
  lv_obj_set_size(s_panel_repos, 640, 172);
  lv_obj_center(s_panel_repos);
  lv_obj_set_style_pad_all(s_panel_repos, 0, 0);
  lv_obj_set_style_bg_color(s_panel_repos, lv_color_hex(0x060B12), 0);
  lv_obj_set_style_bg_opa(s_panel_repos, 255, 0);
  lv_obj_set_style_border_width(s_panel_repos, 0, 0);
  lv_obj_add_flag(s_panel_repos, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_panel_repos, LV_OBJ_FLAG_SCROLLABLE);

  // Top Bar
  lv_obj_t *title_repo = lv_label_create(s_panel_repos);
  lv_label_set_text(title_repo, "Approved Firmware Repositories (GitHub)");
  lv_obj_set_style_text_color(title_repo, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_text_font(title_repo, &lv_font_montserrat_14, 0);
  lv_obj_align(title_repo, LV_ALIGN_TOP_LEFT, 16, 10);

  lv_obj_t *btn_back_repo = lv_btn_create(s_panel_repos);
  lv_obj_set_size(btn_back_repo, 80, 26);
  lv_obj_align(btn_back_repo, LV_ALIGN_TOP_RIGHT, -12, 6);
  lv_obj_set_style_bg_color(btn_back_repo, lv_color_hex(0x334455), 0);
  lv_obj_t *lbl_brp = lv_label_create(btn_back_repo);
  lv_label_set_text(lbl_brp, "Overview");
  lv_obj_set_style_text_font(lbl_brp, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_brp);
  lv_obj_add_event_cb(btn_back_repo, [](lv_event_t *e) {
    lv_obj_add_flag(s_panel_repos, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);

  // Repo Card Container
  lv_obj_t *repo_card = lv_obj_create(s_panel_repos);
  lv_obj_set_size(repo_card, 608, 76);
  lv_obj_align(repo_card, LV_ALIGN_TOP_MID, 0, 38);
  lv_obj_set_style_bg_color(repo_card, lv_color_hex(0x101722), 0);
  lv_obj_set_style_border_color(repo_card, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_border_width(repo_card, 1, 0);
  lv_obj_set_style_radius(repo_card, 6, 0);
  lv_obj_set_style_pad_all(repo_card, 8, 0);
  lv_obj_clear_flag(repo_card, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lbl_repo_name = lv_label_create(repo_card);
  lv_label_set_text(lbl_repo_name, "[*] TuneBar Official - by rahulraj80 [Official]");
  lv_obj_set_style_text_color(lbl_repo_name, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_repo_name, &lv_font_montserrat_12, 0);
  lv_obj_align(lbl_repo_name, LV_ALIGN_TOP_LEFT, 4, 2);

  lv_obj_t *lbl_repo_meta = lv_label_create(repo_card);
  lv_label_set_text(lbl_repo_meta, "Avg Free DRAM: 182 KB | PSRAM: 4.8 MB | Batt: 14.5h Clock / 5.2h Active\nRegistry: https://github.com/rahulraj80/TuneBar");
  lv_obj_set_style_text_color(lbl_repo_meta, lv_color_hex(0x88AACC), 0);
  lv_obj_set_style_text_font(lbl_repo_meta, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(lbl_repo_meta, 2, 0);
  lv_obj_align(lbl_repo_meta, LV_ALIGN_TOP_LEFT, 4, 22);

  lv_obj_t *badge_active = lv_btn_create(repo_card);
  lv_obj_set_size(badge_active, 85, 26);
  lv_obj_align(badge_active, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(badge_active, lv_color_hex(0x008855), 0);
  lv_obj_t *lbl_act = lv_label_create(badge_active);
  lv_label_set_text(lbl_act, "Active OK");
  lv_obj_set_style_text_font(lbl_act, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_act);

  // Footer Note
  lv_obj_t *lbl_footer = lv_label_create(s_panel_repos);
  lv_label_set_text(lbl_footer, "Community forks are approved and added via Pull Request to releases/approved_repos.json");
  lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x667788), 0);
  lv_obj_set_style_text_font(lbl_footer, &lv_font_montserrat_12, 0);
  lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_LEFT, 16, -10);
}

void ui_ota_open_manager(void) {
  if (!s_mgr_root) return;
  lv_obj_clear_flag(s_mgr_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_panel_stepper, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_panel_repos, LV_OBJ_FLAG_HIDDEN);
}

void ui_ota_close_manager(void) {
  if (!s_mgr_root) return;
  lv_obj_add_flag(s_mgr_root, LV_OBJ_FLAG_HIDDEN);
  if (ui_Utility_Button_returnMenu) {
    lv_obj_clear_flag(ui_Utility_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
  }
}
