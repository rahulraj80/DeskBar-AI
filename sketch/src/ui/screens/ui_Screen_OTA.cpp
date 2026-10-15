#include "ui_Screen_OTA.h"
#include "ui_Screen_Utility.h"
#include "updater/updater.h"
#include "battery/battery.h"
#include "version.h"
#include "../../secrets/secrets_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ----------------------------------------------------------------------------
// Curated Release Catalog Data Structure (Dynamic & Real Byte Sizing)
// ----------------------------------------------------------------------------
struct ReleaseItem {
  char version[32];
  char type_name[16]; // "PATCH", "MINOR", "MAJOR"
  uint32_t color_hex;
  char date[32];
  char size_str[32];
  char changelog[1024];
  char bin_url[256];
  uint32_t size_bytes;
};

#include "generated_catalog.h"

#define MAX_CURATED_RELEASES 6
#define MAX_CATALOG_ITEMS 8
static ReleaseItem s_catalog[MAX_CATALOG_ITEMS];
static int s_catalog_count = 0;

static void init_catalog_from_generated(void) {
  s_catalog_count = (GENERATED_CATALOG_COUNT < MAX_CURATED_RELEASES) ? GENERATED_CATALOG_COUNT : MAX_CURATED_RELEASES;
  for (int i = 0; i < s_catalog_count; i++) {
    snprintf(s_catalog[i].version, sizeof(s_catalog[i].version), "%s", s_generated_catalog[i].version);
    snprintf(s_catalog[i].type_name, sizeof(s_catalog[i].type_name), "%s", s_generated_catalog[i].type_name);
    s_catalog[i].color_hex = s_generated_catalog[i].color_hex;
    snprintf(s_catalog[i].date, sizeof(s_catalog[i].date), "%s", s_generated_catalog[i].date);
    snprintf(s_catalog[i].size_str, sizeof(s_catalog[i].size_str), "%s", s_generated_catalog[i].size_str);
    snprintf(s_catalog[i].changelog, sizeof(s_catalog[i].changelog), "%s", s_generated_catalog[i].changelog);
    snprintf(s_catalog[i].bin_url, sizeof(s_catalog[i].bin_url), "%s", s_generated_catalog[i].bin_url);
    s_catalog[i].size_bytes = s_generated_catalog[i].size_bytes;
  }
}

// ----------------------------------------------------------------------------
// Approved Repositories
// ----------------------------------------------------------------------------
struct ApprovedRepo {
  char id[32];
  char name[64];
  char author[32];
  char catalog_url[256];
  char repo_url[256];
  char desc[128];
};

static ApprovedRepo s_repos[2] = {
  {
    "official",
    "TuneBar Official (rahulraj80)",
    "rahulraj80",
    "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/firmware_catalog.json",
    "https://github.com/rahulraj80/TuneBar",
    "Official firmware with HiFi radio, LAN streaming, Nixie clock & IMU auto-rotation"
  },
  {
    "upstream",
    "TuneBar Upstream (VaAndCob)",
    "VaAndCob",
    "https://raw.githubusercontent.com/VaAndCob/TuneBar/main/releases/firmware_catalog.json",
    "https://github.com/VaAndCob/TuneBar",
    "Original upstream baseline repository"
  }
};
static int s_active_repo_idx = 0;
static bool s_is_fetching = false;

// ----------------------------------------------------------------------------
// UI Containers & Elements
// ----------------------------------------------------------------------------
static lv_obj_t *s_mgr_root          = NULL;
static lv_obj_t *s_panel_overview    = NULL;
static lv_obj_t *s_panel_carousel    = NULL;
static lv_obj_t *s_panel_stepper     = NULL;
static lv_obj_t *s_panel_repos       = NULL;

// Overview Elements
static lv_obj_t *s_lbl_overview_info = NULL;
static lv_obj_t *s_lbl_btn_browse    = NULL;

// Carousel Elements
static int       s_active_card_idx   = 0;
static lv_obj_t *s_lbl_badge         = NULL;
static lv_obj_t *s_lbl_ver           = NULL;
static lv_obj_t *s_lbl_meta          = NULL;
static lv_obj_t *s_lbl_batt_check    = NULL;
static lv_obj_t *s_btn_flash         = NULL;
static lv_obj_t *s_lbl_btn_flash     = NULL;
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
static void refresh_overview_text(void);

// ----------------------------------------------------------------------------
// Flash Execution Trigger (Real Background HTTP Streaming)
// ----------------------------------------------------------------------------
static void trigger_flash_action(const char *ver_str, const char *bin_url) {
  uint8_t batt_pct = 0;
  float batt_v = 0.0f;
  if (!ota_check_battery_allowed(&batt_pct, &batt_v)) {
    log_w("[OTA] Blocked: Battery %u%%, %.2fV", (unsigned)batt_pct, batt_v);
    ota_ui_show_blocked_battery(batt_pct);
    return;
  }

  log_i("[OTA] Triggering real HTTP flash for version: %s from: %s", ver_str, bin_url);
  ota_start_http_flash(ver_str, bin_url);
}

void ui_ota_trigger_flash_url(const char *version_str, const char *bin_url) {
  trigger_flash_action(version_str, bin_url);
}

// ----------------------------------------------------------------------------
// Dynamic GitHub Catalog Fetcher
// ----------------------------------------------------------------------------
void ui_ota_fetch_live_catalog(void) {
  if (s_is_fetching) return;
  if (WiFi.status() != WL_CONNECTED) {
    log_w("[OTA] Wi-Fi not connected. Using cached catalog.");
    refresh_overview_text();
    return;
  }
  s_is_fetching = true;

  xTaskCreatePinnedToCore([](void *p) {
    const char *url = s_repos[s_active_repo_idx].catalog_url;
    log_i("[OTA] Fetching dynamic catalog from: %s", url);

    HTTPClient http;
    http.setTimeout(8000);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    WiFiClientSecure secure;
    secure.setInsecure();

    bool beginOk = http.begin(secure, url);
    if (!beginOk) {
      log_e("[OTA] Failed to begin HTTP connection to catalog");
      s_is_fetching = false;
      vTaskDelete(NULL);
      return;
    }

    int code = http.GET();
    if (code == HTTP_CODE_OK) {
      String json = http.getString();
      http.end();

      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, json);
      if (!err && doc["releases"].is<JsonArray>()) {
        JsonArray arr = doc["releases"].as<JsonArray>();
        int count = 0;
        for (JsonObject r : arr) {
          if (count >= MAX_CURATED_RELEASES) break;
          const char *v = r["version"] | "";
          const char *t = r["type"] | "patch";
          const char *d = r["date"] | "";
          const char *c = r["changelog"] | "";
          const char *u = r["url"] | "";
          uint32_t sz = r["size"] | 0;

          if (v[0] == 'v') {
            snprintf(s_catalog[count].version, sizeof(s_catalog[count].version), "%s", v);
          } else {
            snprintf(s_catalog[count].version, sizeof(s_catalog[count].version), "v%s", v);
          }

          if (strcasecmp(t, "major") == 0) {
            snprintf(s_catalog[count].type_name, sizeof(s_catalog[count].type_name), "MAJOR");
            s_catalog[count].color_hex = 0xB388FF;
          } else if (strcasecmp(t, "minor") == 0) {
            snprintf(s_catalog[count].type_name, sizeof(s_catalog[count].type_name), "MINOR");
            s_catalog[count].color_hex = 0x00B0FF;
          } else {
            snprintf(s_catalog[count].type_name, sizeof(s_catalog[count].type_name), "PATCH");
            s_catalog[count].color_hex = 0x00FF88;
          }

          snprintf(s_catalog[count].date, sizeof(s_catalog[count].date), "%s", d);
          snprintf(s_catalog[count].changelog, sizeof(s_catalog[count].changelog), "%s", c);
          snprintf(s_catalog[count].bin_url, sizeof(s_catalog[count].bin_url), "%s", u);
          s_catalog[count].size_bytes = sz;

          if (sz > 0) {
            float mb = (float)sz / (1024.0f * 1024.0f);
            snprintf(s_catalog[count].size_str, sizeof(s_catalog[count].size_str), "%.2f MB", mb);
          } else {
            snprintf(s_catalog[count].size_str, sizeof(s_catalog[count].size_str), "Dynamic");
          }
          count++;
        }
        if (count > 0) s_catalog_count = count;
        log_i("[OTA] Catalog updated from GitHub! %d releases loaded.", count);

        // Schedule UI update on LVGL thread
        lv_async_call([](void *p) {
          refresh_overview_text();
          if (s_panel_carousel && !lv_obj_has_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN)) {
            update_carousel_card(s_active_card_idx);
          }
        }, NULL);
      } else {
        log_w("[OTA] Catalog JSON parse error: %s", err.c_str());
      }
    } else {
      log_w("[OTA] Catalog HTTP GET returned code: %d", code);
      http.end();
    }
    s_is_fetching = false;
    vTaskDelete(NULL);
  }, "ota_cat_task", 6144, NULL, 1, NULL, 1);
}

static void refresh_overview_text(void) {
  if (!s_lbl_overview_info) return;
  char ibuf[320];
  const char *latest_v = (s_catalog_count > 0) ? s_catalog[0].version : "v1.2.6";
  const char *latest_sz = (s_catalog_count > 0) ? s_catalog[0].size_str : "4.04 MB";
  bool wifi_ok = (WiFi.status() == WL_CONNECTED);

  snprintf(ibuf, sizeof(ibuf),
           "Current Build : v%s (Build #%d - %s)\n"
           "Active Repo   : %s\n"
           "Latest GitHub : %s (%s • %s)",
           TUNEBAR_VERSION_STR, TUNEBAR_BUILD_NUM, TUNEBAR_GIT_HASH,
           s_repos[s_active_repo_idx].name,
           latest_v,
           wifi_ok ? "Live Catalog" : "Cached",
           latest_sz);
  lv_label_set_text(s_lbl_overview_info, ibuf);

  if (s_lbl_btn_browse) {
    char bbuf[48];
    snprintf(bbuf, sizeof(bbuf), LV_SYMBOL_LIST " Browse Releases (%d)", s_catalog_count);
    lv_label_set_text(s_lbl_btn_browse, bbuf);
  }
}

// ----------------------------------------------------------------------------
// Carousel Auto-Scroll Logic
// ----------------------------------------------------------------------------
static void autoscroll_timer_cb(lv_timer_t *t) {
  if (!s_obj_scroll_box || !lv_obj_is_valid(s_obj_scroll_box)) return;
  if (lv_obj_has_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN)) return;

  if (s_user_touching || (millis() - s_user_touch_ms < 5000)) {
    return;
  }

  lv_coord_t can_scroll_bot = lv_obj_get_scroll_bottom(s_obj_scroll_box);

  if (s_as_state == AS_DOWN) {
    if (can_scroll_bot <= 2) {
      s_as_state = AS_PAUSE_BOT;
      s_as_timer = millis();
    } else {
      lv_obj_scroll_by(s_obj_scroll_box, 0, -1, LV_ANIM_OFF);
    }
  } else if (s_as_state == AS_PAUSE_BOT) {
    if (millis() - s_as_timer >= 2000) {
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
  int total_cards = s_catalog_count + 1; // 6 curated releases + 1 Archives panel
  if (idx < 0) idx = 0;
  if (idx >= total_cards) idx = total_cards - 1;
  s_active_card_idx = idx;

  if (idx >= s_catalog_count) {
    // -------------------------------------------------------------
    // CARD 7: GITHUB RELEASE ARCHIVES PANEL
    // -------------------------------------------------------------
    if (s_lbl_badge) {
      lv_label_set_text(s_lbl_badge, "ARCHIVE");
      lv_obj_set_style_bg_color(s_lbl_badge, lv_color_hex(0xFFA500), 0);
    }
    if (s_lbl_ver) {
      lv_label_set_text(s_lbl_ver, "Archives");
    }
    if (s_lbl_meta) {
      lv_label_set_text(s_lbl_meta, "GitHub Releases\nOlder Versions");
    }
    if (s_lbl_batt_check) {
      lv_label_set_text(s_lbl_batt_check, "Use Stepper ->");
      lv_obj_set_style_text_color(s_lbl_batt_check, lv_color_hex(0x00E5FF), 0);
    }
    if (s_btn_flash) {
      lv_obj_set_style_bg_color(s_btn_flash, lv_color_hex(0x005588), 0);
    }
    if (s_lbl_btn_flash) {
      lv_label_set_text(s_lbl_btn_flash, LV_SYMBOL_SETTINGS " Open Stepper");
    }
    if (s_lbl_card_counter) {
      char cbuf[32];
      snprintf(cbuf, sizeof(cbuf), "Card %d of %d: Archives", total_cards, total_cards);
      lv_label_set_text(s_lbl_card_counter, cbuf);
    }
    if (s_lbl_changelog) {
      lv_label_set_text(s_lbl_changelog,
        "📦 GitHub Release Archives\n"
        "────────────────────────────\n"
        "You have reached the end of the 6 curated releases.\n\n"
        "Older builds (v1.0.0 through v2.0.5) are preserved in the GitHub archives.\n\n"
        "To install an older archive release:\n"
        "1. Tap [Open Stepper] on the left to dial any version (e.g. v1.2.9, v1.0.0).\n"
        "2. Tap [Find on GitHub] to verify and pull the binary directly.\n\n"
        "Repository:\n"
        "https://github.com/rahulraj80/TuneBar/releases"
      );
    }
  } else {
    // -------------------------------------------------------------
    // CURATED RELEASE CARDS (1 to 6)
    // -------------------------------------------------------------
    const ReleaseItem *item = &s_catalog[idx];

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

    if (s_btn_flash) {
      lv_obj_set_style_bg_color(s_btn_flash, lv_color_hex(0x008855), 0);
    }
    if (s_lbl_btn_flash) {
      lv_label_set_text(s_lbl_btn_flash, LV_SYMBOL_DOWNLOAD " Flash This");
    }

    if (s_lbl_card_counter) {
      char cbuf[32];
      snprintf(cbuf, sizeof(cbuf), "Release %d of %d", idx + 1, s_catalog_count);
      lv_label_set_text(s_lbl_card_counter, cbuf);
    }
    if (s_lbl_changelog) {
      lv_label_set_text(s_lbl_changelog, item->changelog);
    }
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
  if (s_catalog_count == 0) {
    init_catalog_from_generated();
  }
  if (s_mgr_root) return;

  // Root Full-Screen Panel: 640x172
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

  // Title
  lv_obj_t *title = lv_label_create(s_panel_overview);
  lv_label_set_text(title, "TuneBar Firmware Manager");
  lv_obj_set_style_text_color(title, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 10);

  // Enlarged Close Button [X]: 46x38px matching Settings standard
  lv_obj_t *btn_close = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_close, 46, 38);
  lv_obj_align(btn_close, LV_ALIGN_TOP_RIGHT, -10, 6);
  lv_obj_set_style_bg_color(btn_close, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_bg_opa(btn_close, 0, 0);
  lv_obj_set_style_border_color(btn_close, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_border_opa(btn_close, 255, 0);
  lv_obj_set_style_border_width(btn_close, 2, 0);
  lv_obj_set_style_radius(btn_close, 6, 0);
  lv_obj_t *lbl_x = lv_label_create(btn_close);
  lv_label_set_text(lbl_x, "X");
  lv_obj_set_style_text_color(lbl_x, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_x, &lv_font_montserrat_18, 0);
  lv_obj_center(lbl_x);
  lv_obj_add_event_cb(btn_close, [](lv_event_t *e) { ui_ota_close_manager(); }, LV_EVENT_CLICKED, NULL);

  // Status Information
  s_lbl_overview_info = lv_label_create(s_panel_overview);
  lv_obj_set_style_text_color(s_lbl_overview_info, lv_color_hex(0xCCCCCC), 0);
  lv_obj_set_style_text_font(s_lbl_overview_info, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(s_lbl_overview_info, 3, 0);
  lv_obj_align(s_lbl_overview_info, LV_ALIGN_TOP_LEFT, 16, 36);
  refresh_overview_text();

  // Switch Repo button (lowered to y=50px to prevent overlap)
  lv_obj_t *btn_switch_repo = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_switch_repo, 130, 28);
  lv_obj_align(btn_switch_repo, LV_ALIGN_TOP_RIGHT, -10, 50);
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

  // Reset Secrets & Cloud Identity Button
  lv_obj_t *btn_reset_sec = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_reset_sec, 130, 26);
  lv_obj_align(btn_reset_sec, LV_ALIGN_TOP_RIGHT, -10, 82);
  lv_obj_set_style_bg_color(btn_reset_sec, lv_color_hex(0x3B1010), 0);
  lv_obj_set_style_border_color(btn_reset_sec, lv_color_hex(0xEF4444), 0);
  lv_obj_set_style_border_width(btn_reset_sec, 1, 0);
  lv_obj_set_style_radius(btn_reset_sec, 4, 0);
  lv_obj_t *lbl_rs = lv_label_create(btn_reset_sec);
  lv_label_set_text(lbl_rs, LV_SYMBOL_TRASH " Reset Secrets");
  lv_obj_set_style_text_color(lbl_rs, lv_color_hex(0xEF4444), 0);
  lv_obj_set_style_text_font(lbl_rs, &lv_font_montserrat_10, 0);
  lv_obj_center(lbl_rs);

  static uint32_t s_confirm_reset_ms = 0;
  lv_obj_add_event_cb(btn_reset_sec, [](lv_event_t *e) {
    uint32_t now = millis();
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (now - s_confirm_reset_ms < 3500) {
      lv_label_set_text(lbl, "Erasing...");
      secrets_erase_all();
      delay(250);
      ESP.restart();
    } else {
      s_confirm_reset_ms = now;
      lv_label_set_text(lbl, "Confirm Erase?");
      lv_obj_set_style_bg_color(btn, lv_color_hex(0x991B1B), 0);
    }
  }, LV_EVENT_CLICKED, NULL);

  // Bottom Navigation Buttons
  lv_obj_t *btn_browse = lv_btn_create(s_panel_overview);
  lv_obj_set_size(btn_browse, 185, 36);
  lv_obj_align(btn_browse, LV_ALIGN_BOTTOM_LEFT, 16, -12);
  lv_obj_set_style_bg_color(btn_browse, lv_color_hex(0x005588), 0);
  s_lbl_btn_browse = lv_label_create(btn_browse);
  lv_label_set_text(s_lbl_btn_browse, LV_SYMBOL_LIST " Browse Releases (6)");
  lv_obj_set_style_text_font(s_lbl_btn_browse, &lv_font_montserrat_12, 0);
  lv_obj_center(s_lbl_btn_browse);
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
    trigger_flash_action(s_catalog[0].version, s_catalog[0].bin_url);
  }, LV_EVENT_CLICKED, NULL);

  // =========================================================================
  // SUBPANEL 2: LANDSCAPE SPLIT CAROUSEL
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
    int total_cards = s_catalog_count + 1;
    if (s_active_card_idx < total_cards - 1) update_carousel_card(s_active_card_idx + 1);
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
  lv_label_set_text(s_lbl_meta, "Date: Oct 12, 2026\nSize: 4.04 MB");
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
  s_lbl_btn_flash = lv_label_create(s_btn_flash);
  lv_label_set_text(s_lbl_btn_flash, LV_SYMBOL_DOWNLOAD " Flash This");
  lv_obj_set_style_text_font(s_lbl_btn_flash, &lv_font_montserrat_12, 0);
  lv_obj_center(s_lbl_btn_flash);
  lv_obj_add_event_cb(s_btn_flash, [](lv_event_t *e) {
    if (s_active_card_idx >= s_catalog_count) {
      // Archives Card: Open Version Stepper to query older releases
      lv_obj_add_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_panel_stepper, LV_OBJ_FLAG_HIDDEN);
      return;
    }
    const ReleaseItem *item = &s_catalog[s_active_card_idx];
    trigger_flash_action(item->version, item->bin_url);
  }, LV_EVENT_CLICKED, NULL);

  // Right Changelog Scrolling Container (420px)
  s_obj_scroll_box = lv_obj_create(s_panel_carousel);
  lv_obj_set_size(s_obj_scroll_box, 415, 132);
  lv_obj_align(s_obj_scroll_box, LV_ALIGN_BOTTOM_RIGHT, -10, -4);
  lv_obj_set_style_bg_color(s_obj_scroll_box, lv_color_hex(0x0C121A), 0);
  lv_obj_set_style_border_color(s_obj_scroll_box, lv_color_hex(0x223344), 0);
  lv_obj_set_style_border_width(s_obj_scroll_box, 1, 0);
  lv_obj_set_style_radius(s_obj_scroll_box, 6, 0);
  lv_obj_set_style_pad_all(s_obj_scroll_box, 8, 0);
  lv_obj_set_scroll_dir(s_obj_scroll_box, LV_DIR_VER);

  // Touch handlers to suspend auto-scroll on interaction
  lv_obj_add_event_cb(s_obj_scroll_box, [](lv_event_t *e) {
    s_user_touching = true;
    s_user_touch_ms = millis();
  }, LV_EVENT_PRESSED, NULL);

  lv_obj_add_event_cb(s_obj_scroll_box, [](lv_event_t *e) {
    s_user_touching = false;
    s_user_touch_ms = millis();
  }, LV_EVENT_RELEASED, NULL);

  s_lbl_changelog = lv_label_create(s_obj_scroll_box);
  lv_label_set_text(s_lbl_changelog, s_catalog[0].changelog);
  lv_obj_set_style_text_color(s_lbl_changelog, lv_color_hex(0xDDDDDD), 0);
  lv_obj_set_style_text_font(s_lbl_changelog, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_line_space(s_lbl_changelog, 4, 0);
  lv_obj_set_width(s_lbl_changelog, 395);
  lv_label_set_long_mode(s_lbl_changelog, LV_LABEL_LONG_WRAP);

  // Start auto-scroll timer: 50ms interval (20px/s)
  if (!s_autoscroll_timer) {
    s_autoscroll_timer = lv_timer_create(autoscroll_timer_cb, 50, NULL);
  }

  // =========================================================================
  // SUBPANEL 3: NUMERIC STEPPER QUERY (Find on GitHub)
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

  const int col_x[3] = { 100, 190, 280 };
  int *step_ptrs[3]  = { &s_step_major, &s_step_minor, &s_step_patch };
  lv_obj_t **lbl_ptrs[3] = { &s_lbl_step_maj, &s_lbl_step_min, &s_lbl_step_pat };

  for (int i = 0; i < 3; i++) {
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

    lv_obj_t *lbl_num = lv_label_create(s_panel_stepper);
    char nb[16]; snprintf(nb, sizeof(nb), "%d", *step_ptrs[i]);
    lv_label_set_text(lbl_num, nb);
    lv_obj_set_style_text_color(lbl_num, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_num, &lv_font_montserrat_18, 0);
    lv_obj_align(lbl_num, LV_ALIGN_TOP_LEFT, col_x[i] + 16, 76);
    *lbl_ptrs[i] = lbl_num;

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

    // 1. Search curated/live catalog first
    int match_idx = -1;
    for (int i = 0; i < s_catalog_count; i++) {
      if (strcmp(s_catalog[i].version, target) == 0) {
        match_idx = i;
        break;
      }
    }

    if (match_idx >= 0) {
      char sbuf[64];
      snprintf(sbuf, sizeof(sbuf), "Found %s (%s)! Opening card...", target, s_catalog[match_idx].size_str);
      lv_label_set_text(s_lbl_step_status, sbuf);
      lv_obj_set_style_text_color(s_lbl_step_status, lv_color_hex(0x00FF88), 0);
      update_carousel_card(match_idx);
      lv_obj_add_flag(s_panel_stepper, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN);
      return;
    }

    // 2. Query GitHub direct URL
    if (WiFi.status() != WL_CONNECTED) {
      lv_label_set_text(s_lbl_step_status, "Wi-Fi offline. Cannot query GitHub.");
      lv_obj_set_style_text_color(s_lbl_step_status, lv_color_hex(0xFFA500), 0);
      return;
    }

    lv_label_set_text(s_lbl_step_status, "Probing GitHub releases/bin...");
    lv_obj_set_style_text_color(s_lbl_step_status, lv_color_hex(0x00E5FF), 0);

    struct ProbeData { char ver[32]; };
    ProbeData *pd = (ProbeData *)malloc(sizeof(ProbeData));
    if (!pd) return;
    snprintf(pd->ver, sizeof(pd->ver), "%s", target);

    xTaskCreatePinnedToCore([](void *p) {
      ProbeData *d = (ProbeData *)p;
      char probe_url[256];
      snprintf(probe_url, sizeof(probe_url),
               "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-%s.bin",
               d->ver);

      HTTPClient http;
      http.setTimeout(8000);
      http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      WiFiClientSecure sec;
      sec.setInsecure();

      bool bOk = http.begin(sec, probe_url);
      int code = bOk ? http.sendRequest("HEAD") : -1;
      int sz = http.getSize();
      http.end();

      struct ProbeRes { char ver[32]; int code; int sz; };
      ProbeRes *res = (ProbeRes *)malloc(sizeof(ProbeRes));
      if (res) {
        snprintf(res->ver, sizeof(res->ver), "%s", d->ver);
        res->code = code;
        res->sz = sz;
        lv_async_call([](void *rp) {
          ProbeRes *r = (ProbeRes *)rp;
          if (s_lbl_step_status) {
            if (r->code == 200) {
              char b[96];
              float mb = (r->sz > 0) ? ((float)r->sz / (1024.0f * 1024.0f)) : 0.0f;
              snprintf(b, sizeof(b), "Found %s! (%.2f MB)\nReady on GitHub.", r->ver, mb);
              lv_label_set_text(s_lbl_step_status, b);
              lv_obj_set_style_text_color(s_lbl_step_status, lv_color_hex(0x00FF88), 0);
            } else {
              char b[96];
              snprintf(b, sizeof(b), "%s not found on GitHub (HTTP %d).", r->ver, r->code);
              lv_label_set_text(s_lbl_step_status, b);
              lv_obj_set_style_text_color(s_lbl_step_status, lv_color_hex(0xFFA500), 0);
            }
          }
          free(r);
        }, res);
      }
      free(d);
      vTaskDelete(NULL);
    }, "ota_probe_task", 4096, pd, 1, NULL, 1);
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

  // Repo 1 Card (TuneBar Official)
  lv_obj_t *repo_card1 = lv_obj_create(s_panel_repos);
  lv_obj_set_size(repo_card1, 608, 48);
  lv_obj_align(repo_card1, LV_ALIGN_TOP_MID, 0, 38);
  lv_obj_set_style_bg_color(repo_card1, lv_color_hex(0x101722), 0);
  lv_obj_set_style_border_color(repo_card1, lv_color_hex(0x00E5FF), 0);
  lv_obj_set_style_border_width(repo_card1, 1, 0);
  lv_obj_set_style_radius(repo_card1, 6, 0);
  lv_obj_set_style_pad_all(repo_card1, 6, 0);
  lv_obj_clear_flag(repo_card1, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lbl_repo1 = lv_label_create(repo_card1);
  lv_label_set_text(lbl_repo1, "1. TuneBar Official (rahulraj80)\nhttps://github.com/rahulraj80/TuneBar");
  lv_obj_set_style_text_color(lbl_repo1, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_text_font(lbl_repo1, &lv_font_montserrat_12, 0);
  lv_obj_align(lbl_repo1, LV_ALIGN_TOP_LEFT, 4, 2);

  lv_obj_t *badge1 = lv_btn_create(repo_card1);
  lv_obj_set_size(badge1, 90, 24);
  lv_obj_align(badge1, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(badge1, lv_color_hex(0x008855), 0);
  lv_obj_t *lbl_b1 = lv_label_create(badge1);
  lv_label_set_text(lbl_b1, "ACTIVE");
  lv_obj_set_style_text_font(lbl_b1, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_b1);
  lv_obj_add_event_cb(badge1, [](lv_event_t *e) {
    s_active_repo_idx = 0;
    ui_ota_fetch_live_catalog();
    lv_obj_add_flag(s_panel_repos, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);

  // Repo 2 Card (TuneBar Upstream)
  lv_obj_t *repo_card2 = lv_obj_create(s_panel_repos);
  lv_obj_set_size(repo_card2, 608, 48);
  lv_obj_align(repo_card2, LV_ALIGN_TOP_MID, 0, 92);
  lv_obj_set_style_bg_color(repo_card2, lv_color_hex(0x101722), 0);
  lv_obj_set_style_border_color(repo_card2, lv_color_hex(0x334455), 0);
  lv_obj_set_style_border_width(repo_card2, 1, 0);
  lv_obj_set_style_radius(repo_card2, 6, 0);
  lv_obj_set_style_pad_all(repo_card2, 6, 0);
  lv_obj_clear_flag(repo_card2, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *lbl_repo2 = lv_label_create(repo_card2);
  lv_label_set_text(lbl_repo2, "2. TuneBar Upstream (VaAndCob)\nhttps://github.com/VaAndCob/TuneBar");
  lv_obj_set_style_text_color(lbl_repo2, lv_color_hex(0xAAAAAA), 0);
  lv_obj_set_style_text_font(lbl_repo2, &lv_font_montserrat_12, 0);
  lv_obj_align(lbl_repo2, LV_ALIGN_TOP_LEFT, 4, 2);

  lv_obj_t *badge2 = lv_btn_create(repo_card2);
  lv_obj_set_size(badge2, 90, 24);
  lv_obj_align(badge2, LV_ALIGN_RIGHT_MID, -4, 0);
  lv_obj_set_style_bg_color(badge2, lv_color_hex(0x223344), 0);
  lv_obj_t *lbl_b2 = lv_label_create(badge2);
  lv_label_set_text(lbl_b2, "SELECT");
  lv_obj_set_style_text_font(lbl_b2, &lv_font_montserrat_12, 0);
  lv_obj_center(lbl_b2);
  lv_obj_add_event_cb(badge2, [](lv_event_t *e) {
    s_active_repo_idx = 1;
    ui_ota_fetch_live_catalog();
    lv_obj_add_flag(s_panel_repos, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_CLICKED, NULL);

  // Footer Note
  lv_obj_t *lbl_footer = lv_label_create(s_panel_repos);
  lv_label_set_text(lbl_footer, "Approved repositories query releases dynamically from GitHub over HTTPS.");
  lv_obj_set_style_text_color(lbl_footer, lv_color_hex(0x667788), 0);
  lv_obj_set_style_text_font(lbl_footer, &lv_font_montserrat_12, 0);
  lv_obj_align(lbl_footer, LV_ALIGN_BOTTOM_LEFT, 16, -6);
}

void ui_ota_open_manager(void) {
  if (!s_mgr_root) return;
  lv_obj_clear_flag(s_mgr_root, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(s_panel_overview, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_panel_carousel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_panel_stepper, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_panel_repos, LV_OBJ_FLAG_HIDDEN);
  refresh_overview_text();
  ui_ota_fetch_live_catalog(); // trigger background catalog fetch from GitHub!
}

void ui_ota_close_manager(void) {
  if (!s_mgr_root) return;
  lv_obj_add_flag(s_mgr_root, LV_OBJ_FLAG_HIDDEN);
  if (ui_Utility_Button_returnMenu) {
    lv_obj_clear_flag(ui_Utility_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
  }
}
