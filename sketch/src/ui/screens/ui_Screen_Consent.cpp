#include "ui_Screen_Consent.h"
#include "../../secrets/secrets_manager.h"
#include "../../telemetry/telemetry.h"
#include <Arduino.h>

static lv_obj_t *s_consent_backdrop = NULL;
static lv_obj_t *s_title_label = NULL;
static lv_obj_t *s_subtitle_label = NULL;
static lv_obj_t *s_warning_label = NULL;
static lv_obj_t *s_table_label = NULL;
static lv_obj_t *s_code_label = NULL;
static lv_obj_t *s_timer_label = NULL;
static lv_obj_t *s_btn_digits[4] = {NULL};
static lv_obj_t *s_lbl_digits[4] = {NULL};
static lv_timer_t *s_totp_timer = NULL;

static uint8_t s_pin_digits[4] = {0, 0, 0, 0};
static bool s_is_masked = false;

static void update_totp_display(void) {
    if (!s_code_label) return;
    uint32_t code = 0;
    uint32_t rem_sec = 0;
    if (secrets_calculate_totp(&code, &rem_sec)) {
        char code_buf[32];
        snprintf(code_buf, sizeof(code_buf), "[ %03u %03u ]",
                 (unsigned)(code / 1000), (unsigned)(code % 1000));
        lv_label_set_text(s_code_label, code_buf);
        char timer_buf[16];
        snprintf(timer_buf, sizeof(timer_buf), "(%02us)", (unsigned)rem_sec);
        if (s_timer_label) lv_label_set_text(s_timer_label, timer_buf);
    } else {
        lv_label_set_text(s_code_label, "[ CONNECT WIFI ]");
        if (s_timer_label) lv_label_set_text(s_timer_label, "(--s)");
    }
}

static void totp_timer_cb(lv_timer_t *timer) {
    if (!s_consent_backdrop || lv_obj_has_flag(s_consent_backdrop, LV_OBJ_FLAG_HIDDEN)) return;
    update_totp_display();
}

static void update_table_preview(void) {
    char pin_str[5];
    if (s_is_masked) {
        // If masked, display current table
        snprintf(pin_str, sizeof(pin_str), "%s", secrets_get_master_pin());
    } else {
        snprintf(pin_str, sizeof(pin_str), "%u%u%u%u",
                 s_pin_digits[0], s_pin_digits[1], s_pin_digits[2], s_pin_digits[3]);
    }
    char tbl_buf[32];
    secrets_calculate_table_for_pin(pin_str, tbl_buf, sizeof(tbl_buf));
    char disp_buf[64];
    snprintf(disp_buf, sizeof(disp_buf), "TABLE: %s", tbl_buf);
    if (s_table_label) {
        lv_label_set_text(s_table_label, disp_buf);
    }
}

static void digit_click_cb(lv_event_t * e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx > 3) return;

    if (s_is_masked) {
        // Unmask on first touch
        s_is_masked = false;
        s_pin_digits[0] = 0;
        s_pin_digits[1] = 0;
        s_pin_digits[2] = 0;
        s_pin_digits[3] = 0;
        s_pin_digits[idx] = 1; // Start incrementing touched digit
    } else {
        s_pin_digits[idx] = (s_pin_digits[idx] + 1) % 10;
    }

    for (int i = 0; i < 4; i++) {
        char txt[4];
        snprintf(txt, sizeof(txt), "%u", s_pin_digits[i]);
        lv_label_set_text(s_lbl_digits[i], txt);
    }

    update_table_preview();
}

static void accept_click_cb(lv_event_t * e) {
    if (!s_is_masked) {
        char pin_str[5];
        snprintf(pin_str, sizeof(pin_str), "%u%u%u%u",
                 s_pin_digits[0], s_pin_digits[1], s_pin_digits[2], s_pin_digits[3]);
        secrets_set_master_pin(pin_str);
    }
    secrets_set_cloud_consent(true);
    if (s_consent_backdrop) {
        lv_obj_add_flag(s_consent_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
    telemetry_collect_snapshot();
    telemetry_flush_batch();
}

static void decline_click_cb(lv_event_t * e) {
    secrets_set_cloud_consent(false);
    if (s_consent_backdrop) {
        lv_obj_add_flag(s_consent_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
}

static void refresh_modal_state(void) {
    bool has_existing = secrets_has_existing_pin();
    s_is_masked = has_existing;

    if (has_existing) {
        if (s_subtitle_label) {
            lv_label_set_text(s_subtitle_label, "Existing PIN configured. Tap any digit to change, or Confirm to keep current PIN.");
        }
        for (int i = 0; i < 4; i++) {
            if (s_lbl_digits[i]) lv_label_set_text(s_lbl_digits[i], "*");
        }
    } else {
        if (s_subtitle_label) {
            lv_label_set_text(s_subtitle_label, "Set your 4-digit PIN (tap digits to cycle 0-9) to create your isolated cloud table.");
        }
        // Generate random 4-digit PIN via ESP32 Hardware TRNG for unique per-device isolation
        uint32_t rnd = esp_random() % 10000;
        s_pin_digits[0] = (rnd / 1000) % 10;
        s_pin_digits[1] = (rnd / 100) % 10;
        s_pin_digits[2] = (rnd / 10) % 10;
        s_pin_digits[3] = rnd % 10;
        for (int i = 0; i < 4; i++) {
            if (s_lbl_digits[i]) {
                char txt[4];
                snprintf(txt, sizeof(txt), "%u", s_pin_digits[i]);
                lv_label_set_text(s_lbl_digits[i], txt);
            }
        }
    }

    update_table_preview();
    update_totp_display();
}

void ui_consent_modal_init(lv_obj_t *parent) {
    if (s_consent_backdrop) return;
    if (!parent) parent = lv_layer_top();

    // 1. Full-screen dimmed backdrop (640 x 172)
    s_consent_backdrop = lv_obj_create(parent);
    lv_obj_set_size(s_consent_backdrop, 640, 172);
    lv_obj_set_pos(s_consent_backdrop, 0, 0);
    lv_obj_clear_flag(s_consent_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_consent_backdrop, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_consent_backdrop, 230, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_consent_backdrop, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_consent_backdrop, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_consent_backdrop, 0, LV_PART_MAIN);

    // 2. High-tech Card Dialog (624 x 160 expanded width)
    lv_obj_t *card = lv_obj_create(s_consent_backdrop);
    lv_obj_set_size(card, 624, 160);
    lv_obj_set_pos(card, 8, 6);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x0A0F1D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, 255, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 6, LV_PART_MAIN);

    // Title
    s_title_label = lv_label_create(card);
    lv_obj_set_pos(s_title_label, 10, 3);
    lv_label_set_text(s_title_label, "CLOUD TELEMETRY & SECURITY PIN SETUP");
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_12, LV_PART_MAIN);

    // Subtitle Notice
    s_subtitle_label = lv_label_create(card);
    lv_obj_set_pos(s_subtitle_label, 10, 18);
    lv_obj_set_width(s_subtitle_label, 595);
    lv_label_set_text(s_subtitle_label, "Set your 4-digit PIN (tap digits to cycle 0-9) to create your isolated cloud table.");
    lv_obj_set_style_text_color(s_subtitle_label, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_subtitle_label, &lv_font_montserrat_10, LV_PART_MAIN);

    // Subtle Vertical Divider separating Left and Right columns
    lv_obj_t *sep = lv_obj_create(card);
    lv_obj_set_size(sep, 1, 68);
    lv_obj_set_pos(sep, 248, 36);
    lv_obj_set_style_bg_color(sep, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sep, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(sep, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(sep, 0, LV_PART_MAIN);
    lv_obj_clear_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // ==========================================
    // LEFT COLUMN: PIN Digits & Target Table
    // ==========================================
    for (int i = 0; i < 4; i++) {
        s_btn_digits[i] = lv_btn_create(card);
        lv_obj_set_size(s_btn_digits[i], 44, 28);
        lv_obj_set_pos(s_btn_digits[i], 12 + (i * 48), 36);
        lv_obj_set_style_bg_color(s_btn_digits[i], lv_color_hex(0x131B2E), LV_PART_MAIN);
        lv_obj_set_style_border_color(s_btn_digits[i], lv_color_hex(0x00F0FF), LV_PART_MAIN);
        lv_obj_set_style_border_width(s_btn_digits[i], 1, LV_PART_MAIN);
        lv_obj_set_style_radius(s_btn_digits[i], 6, LV_PART_MAIN);
        lv_obj_add_event_cb(s_btn_digits[i], digit_click_cb, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

        s_lbl_digits[i] = lv_label_create(s_btn_digits[i]);
        lv_label_set_text(s_lbl_digits[i], "0");
        lv_obj_set_align(s_lbl_digits[i], LV_ALIGN_CENTER);
        lv_obj_set_style_text_color(s_lbl_digits[i], lv_color_hex(0x00F0FF), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_lbl_digits[i], &lv_font_montserrat_16, LV_PART_MAIN);
    }

    // Target Table Preview Badge (larger font & prominent)
    s_table_label = lv_label_create(card);
    lv_obj_set_pos(s_table_label, 12, 72);
    lv_label_set_text(s_table_label, "TABLE: TB_56B4EF78CEA249AE");
    lv_obj_set_style_text_color(s_table_label, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_table_label, &lv_font_montserrat_12, LV_PART_MAIN);

    // ==========================================
    // RIGHT COLUMN: Prominent Web Access Code
    // ==========================================
    // Sub-header: "WEB ACCESS CODE:" on left, "30-DAY ACCESS" on right
    lv_obj_t *lbl_access_hdr = lv_label_create(card);
    lv_obj_set_pos(lbl_access_hdr, 264, 36);
    lv_label_set_text(lbl_access_hdr, "WEB ACCESS CODE:");
    lv_obj_set_style_text_color(lbl_access_hdr, lv_color_hex(0x94A3B8), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_access_hdr, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t *lbl_badge_30d = lv_label_create(card);
    lv_obj_set_pos(lbl_badge_30d, 510, 36);
    lv_label_set_text(lbl_badge_30d, "30-DAY ACCESS");
    lv_obj_set_style_text_color(lbl_badge_30d, lv_color_hex(0x38BDF8), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_badge_30d, &lv_font_montserrat_10, LV_PART_MAIN);

    // Large Prominent Code [ 860 193 ] (font 20)
    s_code_label = lv_label_create(card);
    lv_obj_set_pos(s_code_label, 264, 52);
    lv_label_set_text(s_code_label, "[ CONNECT WIFI ]");
    lv_obj_set_style_text_color(s_code_label, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_code_label, &lv_font_montserrat_20, LV_PART_MAIN);

    // Countdown Timer beside the code in amber font 12
    s_timer_label = lv_label_create(card);
    lv_obj_set_pos(s_timer_label, 452, 57);
    lv_label_set_text(s_timer_label, "(--s)");
    lv_obj_set_style_text_color(s_timer_label, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_timer_label, &lv_font_montserrat_12, LV_PART_MAIN);

    // Explanatory Message
    s_warning_label = lv_label_create(card);
    lv_obj_set_pos(s_warning_label, 264, 82);
    lv_obj_set_width(s_warning_label, 340);
    lv_label_set_text(s_warning_label,
        "Enter this 6-digit code on web dashboard for 30-day access. Diagnostics contain zero personal data."
    );
    lv_obj_set_style_text_color(s_warning_label, lv_color_hex(0xCBD5E1), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_warning_label, &lv_font_montserrat_10, LV_PART_MAIN);

    // ==========================================
    // FOOTER ACTION BUTTONS
    // ==========================================
    // Confirm & Connect Button
    lv_obj_t *btn_acc = lv_btn_create(card);
    lv_obj_set_size(btn_acc, 290, 28);
    lv_obj_set_pos(btn_acc, 12, 114);
    lv_obj_set_style_bg_color(btn_acc, lv_color_hex(0x064E3B), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_acc, lv_color_hex(0x34D399), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_acc, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_acc, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_acc, accept_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_acc = lv_label_create(btn_acc);
    lv_label_set_text(lbl_acc, LV_SYMBOL_OK " CONFIRM & CONNECT");
    lv_obj_set_align(lbl_acc, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lbl_acc, lv_color_hex(0x34D399), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_acc, &lv_font_montserrat_10, LV_PART_MAIN);

    // Stay Offline Button
    lv_obj_t *btn_dec = lv_btn_create(card);
    lv_obj_set_size(btn_dec, 290, 28);
    lv_obj_set_pos(btn_dec, 310, 114);
    lv_obj_set_style_bg_color(btn_dec, lv_color_hex(0x2A200A), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_dec, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_dec, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_dec, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_dec, decline_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_dec = lv_label_create(btn_dec);
    lv_label_set_text(lbl_dec, LV_SYMBOL_POWER " STAY OFFLINE");
    lv_obj_set_align(lbl_dec, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lbl_dec, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_dec, &lv_font_montserrat_10, LV_PART_MAIN);

    // 1-second periodic timer for dynamic countdown & rollover
    if (!s_totp_timer) {
        s_totp_timer = lv_timer_create(totp_timer_cb, 1000, NULL);
    }

    // Initial state
    refresh_modal_state();

    // Hidden until checked
    lv_obj_add_flag(s_consent_backdrop, LV_OBJ_FLAG_HIDDEN);
}

void ui_consent_modal_show_if_needed(void) {
    if (!s_consent_backdrop) {
        ui_consent_modal_init(lv_layer_top());
    }
    if (!secrets_has_decided_consent() && s_consent_backdrop) {
        refresh_modal_state();
        lv_obj_clear_flag(s_consent_backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_consent_backdrop);
    }
}

void ui_consent_modal_force_show(void) {
    if (!s_consent_backdrop) {
        ui_consent_modal_init(lv_layer_top());
    }
    if (s_consent_backdrop) {
        refresh_modal_state();
        lv_obj_clear_flag(s_consent_backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_consent_backdrop);
    }
}

void ui_consent_modal_set_pin(const char *pin) {
    if (!pin || strlen(pin) != 4) return;
    s_is_masked = false;
    for (int i = 0; i < 4; i++) {
        if (pin[i] >= '0' && pin[i] <= '9') {
            s_pin_digits[i] = pin[i] - '0';
        }
        if (s_lbl_digits[i]) {
            char txt[4];
            snprintf(txt, sizeof(txt), "%u", s_pin_digits[i]);
            lv_label_set_text(s_lbl_digits[i], txt);
        }
    }
    update_table_preview();
}

void ui_consent_modal_hide(void) {
    if (s_consent_backdrop) {
        lv_obj_add_flag(s_consent_backdrop, LV_OBJ_FLAG_HIDDEN);
    }
}
