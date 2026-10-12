#include "ui_Screen_Consent.h"
#include "../../secrets/secrets_manager.h"
#include <Arduino.h>

static lv_obj_t *s_consent_modal = NULL;

static void accept_click_cb(lv_event_t * e) {
    secrets_set_cloud_consent(true);
    if (s_consent_modal) {
        lv_obj_add_flag(s_consent_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

static void decline_click_cb(lv_event_t * e) {
    secrets_set_cloud_consent(false);
    if (s_consent_modal) {
        lv_obj_add_flag(s_consent_modal, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_consent_modal_init(lv_obj_t *parent) {
    if (s_consent_modal) return;

    s_consent_modal = lv_obj_create(parent);
    lv_obj_set_size(s_consent_modal, 600, 156);
    lv_obj_set_pos(s_consent_modal, 20, 8);
    lv_obj_clear_flag(s_consent_modal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_consent_modal, lv_color_hex(0x0A0F1D), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_consent_modal, 250, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_consent_modal, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_consent_modal, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_consent_modal, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_consent_modal, 10, LV_PART_MAIN);

    // Title
    lv_obj_t *title = lv_label_create(s_consent_modal);
    lv_obj_set_pos(title, 10, 4);
    lv_label_set_text(title, "COMMUNITY CLOUD & PRIVACY NOTICE");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, LV_PART_MAIN);

    // Notice body
    lv_obj_t *body = lv_label_create(s_consent_modal);
    lv_obj_set_pos(body, 10, 24);
    lv_obj_set_width(body, 560);
    char n_buf[320];
    snprintf(n_buf, sizeof(n_buf),
        "TuneBar includes turnkey community cloud endpoints for AI assistant & diagnostics.\n"
        "Diagnostics are segregated into a private hardware table (%s) with no personal data.\n"
        "You can self-host and override endpoints at any time by uploading /secrets.ini via Web Remote.",
        secrets_get_telemetry_table()
    );
    lv_label_set_text(body, n_buf);
    lv_obj_set_style_text_color(body, lv_color_hex(0xE2E8F0), LV_PART_MAIN);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_10, LV_PART_MAIN);

    // Accept Button
    lv_obj_t *btn_acc = lv_btn_create(s_consent_modal);
    lv_obj_set_size(btn_acc, 200, 32);
    lv_obj_set_pos(btn_acc, 60, 96);
    lv_obj_set_style_bg_color(btn_acc, lv_color_hex(0x064E3B), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_acc, lv_color_hex(0x34D399), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_acc, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_acc, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_acc, accept_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_acc = lv_label_create(btn_acc);
    lv_label_set_text(lbl_acc, "[ACCEPT & CONTINUE]");
    lv_obj_set_align(lbl_acc, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lbl_acc, lv_color_hex(0x34D399), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_acc, &lv_font_montserrat_10, LV_PART_MAIN);

    // Decline / Offline Button
    lv_obj_t *btn_dec = lv_btn_create(s_consent_modal);
    lv_obj_set_size(btn_dec, 200, 32);
    lv_obj_set_pos(btn_dec, 320, 96);
    lv_obj_set_style_bg_color(btn_dec, lv_color_hex(0x2A200A), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn_dec, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn_dec, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(btn_dec, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(btn_dec, decline_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_dec = lv_label_create(btn_dec);
    lv_label_set_text(lbl_dec, "[OFFLINE ONLY]");
    lv_obj_set_align(lbl_dec, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(lbl_dec, lv_color_hex(0xFBBF24), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_dec, &lv_font_montserrat_10, LV_PART_MAIN);

    // Hidden until checked
    lv_obj_add_flag(s_consent_modal, LV_OBJ_FLAG_HIDDEN);
}

void ui_consent_modal_show_if_needed(void) {
    if (!secrets_has_cloud_consent() && s_consent_modal) {
        lv_obj_clear_flag(s_consent_modal, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_consent_modal);
    }
}
