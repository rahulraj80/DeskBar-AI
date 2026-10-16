#include "ui_Screen_PowerProfile.h"
#include "../../power/power_profile.h"
#include "../../battery/battery.h"
#include <Arduino.h>

extern "C" {
    extern uint32_t SCREEN_OFF_TIMER;
    extern lv_obj_t * ui_Screen_Utility;
    extern lv_obj_t * ui_Utility_Button_returnMenu;
}

static lv_obj_t * s_root_container = NULL;
static lv_obj_t * s_scroll_box = NULL;

// Stepper and switch UI widgets
static lv_obj_t * s_lbl_bl_start_val = NULL;
static lv_obj_t * s_lbl_bl_end_val = NULL;
static lv_obj_t * s_lbl_bl_black_val = NULL;
static lv_obj_t * s_lbl_ramp_up_val = NULL;
static lv_obj_t * s_lbl_hold_high_val = NULL;
static lv_obj_t * s_lbl_ramp_down_val = NULL;
static lv_obj_t * s_lbl_hold_dark_val = NULL;

static lv_obj_t * s_sw_inactivity = NULL;
static lv_obj_t * s_sw_gate_wifi = NULL;
static lv_obj_t * s_sw_gate_audio = NULL;
static lv_obj_t * s_sw_gate_sensors = NULL;

static lv_obj_t * s_lbl_cal_status = NULL;
static lv_obj_t * s_lbl_cal_source = NULL;
static lv_obj_t * s_lbl_cal_time = NULL;
static lv_obj_t * s_btn_sync_cal = NULL;

static power_profile_t s_local_cfg;

static void update_all_labels(void) {
    if (s_lbl_bl_start_val) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", s_local_cfg.bl_start);
        lv_label_set_text(s_lbl_bl_start_val, buf);
    }
    if (s_lbl_bl_end_val) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", s_local_cfg.bl_end);
        lv_label_set_text(s_lbl_bl_end_val, buf);
    }
    if (s_lbl_bl_black_val) {
        char buf[32];
        if (s_local_cfg.bl_black == 0) {
            snprintf(buf, sizeof(buf), "0 (Cut Rail)");
        } else {
            snprintf(buf, sizeof(buf), "%u PWM", s_local_cfg.bl_black);
        }
        lv_label_set_text(s_lbl_bl_black_val, buf);
    }
    if (s_lbl_ramp_up_val) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1fs (%ums)", (float)s_local_cfg.ramp_up_ms / 1000.0f, s_local_cfg.ramp_up_ms);
        lv_label_set_text(s_lbl_ramp_up_val, buf);
    }
    if (s_lbl_hold_high_val) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1fs (%ums)", (float)s_local_cfg.hold_high_ms / 1000.0f, s_local_cfg.hold_high_ms);
        lv_label_set_text(s_lbl_hold_high_val, buf);
    }
    if (s_lbl_ramp_down_val) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1fs (%ums)", (float)s_local_cfg.ramp_down_ms / 1000.0f, s_local_cfg.ramp_down_ms);
        lv_label_set_text(s_lbl_ramp_down_val, buf);
    }
    if (s_lbl_hold_dark_val) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%.1fs (%ums)", (float)s_local_cfg.hold_dark_ms / 1000.0f, s_local_cfg.hold_dark_ms);
        lv_label_set_text(s_lbl_hold_dark_val, buf);
    }

    if (s_lbl_cal_status) {
        float volts[BATTERY_CURVE_PTS];
        uint8_t pcts[BATTERY_CURVE_PTS];
        uint8_t count = 0;
        battery_get_curve_points(volts, pcts, &count);
        char cbuf[96];
        snprintf(cbuf, sizeof(cbuf), "Curve: 100%%=%.2fV | 0%%=%.2fV (%u pts)",
                 volts[0], volts[count - 1], count);
        lv_label_set_text(s_lbl_cal_status, cbuf);
        lv_obj_set_style_text_color(s_lbl_cal_status, lv_color_hex(0x00E5FF), 0);
    }
    if (s_lbl_cal_source) {
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), "Source: %s", battery_get_cal_source_label());
        lv_label_set_text(s_lbl_cal_source, sbuf);
    }
    if (s_lbl_cal_time) {
        char tbuf[64];
        snprintf(tbuf, sizeof(tbuf), "Last Pull: %s", battery_get_cal_timestamp());
        lv_label_set_text(s_lbl_cal_time, tbuf);
    }
}

static void on_close_click(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    ui_power_profile_close();
}

static void on_sync_cal_click(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    if (s_lbl_cal_status) {
        lv_label_set_text(s_lbl_cal_status, "Syncing calibration from cloud...");
        lv_obj_set_style_text_color(s_lbl_cal_status, lv_color_hex(0x00E5FF), 0);
    }
    bool ok = battery_sync_from_cloud(NULL);
    if (ok) {
        update_all_labels();
        if (s_lbl_cal_status) {
            float volts[BATTERY_CURVE_PTS];
            uint8_t count = 0;
            battery_get_curve_points(volts, NULL, &count);
            char cbuf[96];
            snprintf(cbuf, sizeof(cbuf), "Synced: 100%%=%.2fV | 0%%=%.2fV (%u pts)",
                     volts[0], volts[count - 1], count);
            lv_label_set_text(s_lbl_cal_status, cbuf);
            lv_obj_set_style_text_color(s_lbl_cal_status, lv_color_hex(0x00FF88), 0);
        }
    } else {
        if (s_lbl_cal_status) {
            lv_label_set_text(s_lbl_cal_status, "Sync failed! Check Wi-Fi connection.");
            lv_obj_set_style_text_color(s_lbl_cal_status, lv_color_hex(0xFF5555), 0);
        }
    }
}

// Stepper Helper: creates [Label] [ - ] [ Value ] [ + ]
static lv_obj_t* create_stepper_row(lv_obj_t *parent, const char *title, lv_obj_t **out_val_lbl,
                                    lv_event_cb_t minus_cb, lv_event_cb_t plus_cb) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 608, 44);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161C28), 0);
    lv_obj_set_style_bg_opa(row, 255, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x232D3F), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_t = lv_label_create(row);
    lv_label_set_text(lbl_t, title);
    lv_obj_align(lbl_t, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(lbl_t, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_text_font(lbl_t, &lv_font_montserrat_14, 0);

    // Minus Button
    lv_obj_t *btn_m = lv_btn_create(row);
    lv_obj_set_size(btn_m, 44, 32);
    lv_obj_align(btn_m, LV_ALIGN_RIGHT_MID, -170, 0);
    lv_obj_set_style_bg_color(btn_m, lv_color_hex(0x222E42), 0);
    lv_obj_set_style_border_color(btn_m, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_border_width(btn_m, 1, 0);
    lv_obj_set_style_radius(btn_m, 4, 0);
    lv_obj_add_event_cb(btn_m, minus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_m = lv_label_create(btn_m);
    lv_label_set_text(lbl_m, "-");
    lv_obj_center(lbl_m);
    lv_obj_set_style_text_color(lbl_m, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_text_font(lbl_m, &lv_font_montserrat_16, 0);

    // Value Label
    lv_obj_t *lbl_v = lv_label_create(row);
    lv_obj_set_width(lbl_v, 110);
    lv_obj_align(lbl_v, LV_ALIGN_RIGHT_MID, -54, 0);
    lv_obj_set_style_text_align(lbl_v, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(lbl_v, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_text_font(lbl_v, &lv_font_montserrat_14, 0);
    *out_val_lbl = lbl_v;

    // Plus Button
    lv_obj_t *btn_p = lv_btn_create(row);
    lv_obj_set_size(btn_p, 44, 32);
    lv_obj_align(btn_p, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_set_style_bg_color(btn_p, lv_color_hex(0x222E42), 0);
    lv_obj_set_style_border_color(btn_p, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_border_width(btn_p, 1, 0);
    lv_obj_set_style_radius(btn_p, 4, 0);
    lv_obj_add_event_cb(btn_p, plus_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_p = lv_label_create(btn_p);
    lv_label_set_text(lbl_p, "+");
    lv_obj_center(lbl_p);
    lv_obj_set_style_text_color(lbl_p, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_text_font(lbl_p, &lv_font_montserrat_16, 0);

    return row;
}

// Switch Helper: creates [Label] [Switch]
static lv_obj_t* create_switch_row(lv_obj_t *parent, const char *title, lv_obj_t **out_sw, lv_event_cb_t sw_cb) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 608, 44);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161C28), 0);
    lv_obj_set_style_bg_opa(row, 255, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x232D3F), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 6, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_t = lv_label_create(row);
    lv_label_set_text(lbl_t, title);
    lv_obj_align(lbl_t, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_set_style_text_color(lbl_t, lv_color_hex(0xEEEEEE), 0);
    lv_obj_set_style_text_font(lbl_t, &lv_font_montserrat_14, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_event_cb(sw, sw_cb, LV_EVENT_VALUE_CHANGED, NULL);
    *out_sw = sw;

    return row;
}

// Event callbacks for steppers
static void cb_bl_start_minus(lv_event_t * e) {
    if (s_local_cfg.bl_start > 5) s_local_cfg.bl_start -= 5;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}
static void cb_bl_start_plus(lv_event_t * e) {
    if (s_local_cfg.bl_start < s_local_cfg.bl_end - 5) s_local_cfg.bl_start += 5;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}

static void cb_bl_end_minus(lv_event_t * e) {
    if (s_local_cfg.bl_end > s_local_cfg.bl_start + 5) s_local_cfg.bl_end -= 5;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}
static void cb_bl_end_plus(lv_event_t * e) {
    if (s_local_cfg.bl_end < 255) s_local_cfg.bl_end += 5;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}

static void cb_bl_black_minus(lv_event_t * e) {
    if (s_local_cfg.bl_black > 0) s_local_cfg.bl_black -= 1;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}
static void cb_bl_black_plus(lv_event_t * e) {
    if (s_local_cfg.bl_black < 50) s_local_cfg.bl_black += 1;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}

static void cb_ramp_up_minus(lv_event_t * e) {
    if (s_local_cfg.ramp_up_ms > 500) s_local_cfg.ramp_up_ms -= 250;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}
static void cb_ramp_up_plus(lv_event_t * e) {
    if (s_local_cfg.ramp_up_ms < 5000) s_local_cfg.ramp_up_ms += 250;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}

static void cb_hold_high_minus(lv_event_t * e) {
    if (s_local_cfg.hold_high_ms > 500) s_local_cfg.hold_high_ms -= 500;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}
static void cb_hold_high_plus(lv_event_t * e) {
    if (s_local_cfg.hold_high_ms < 10000) s_local_cfg.hold_high_ms += 500;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}

static void cb_ramp_down_minus(lv_event_t * e) {
    if (s_local_cfg.ramp_down_ms > 500) s_local_cfg.ramp_down_ms -= 250;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}
static void cb_ramp_down_plus(lv_event_t * e) {
    if (s_local_cfg.ramp_down_ms < 5000) s_local_cfg.ramp_down_ms += 250;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}

static void cb_hold_dark_minus(lv_event_t * e) {
    if (s_local_cfg.hold_dark_ms > 1000) s_local_cfg.hold_dark_ms -= 1000;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}
static void cb_hold_dark_plus(lv_event_t * e) {
    if (s_local_cfg.hold_dark_ms < 30000) s_local_cfg.hold_dark_ms += 1000;
    power_profile_set(&s_local_cfg);
    update_all_labels();
}

static void cb_sw_inactivity(lv_event_t * e) {
    s_local_cfg.inactivity_tier = lv_obj_has_state(s_sw_inactivity, LV_STATE_CHECKED);
    power_profile_set(&s_local_cfg);
}
static void cb_sw_gate_wifi(lv_event_t * e) {
    s_local_cfg.gate_wifi = lv_obj_has_state(s_sw_gate_wifi, LV_STATE_CHECKED);
    power_profile_set(&s_local_cfg);
}
static void cb_sw_gate_audio(lv_event_t * e) {
    s_local_cfg.gate_audio = lv_obj_has_state(s_sw_gate_audio, LV_STATE_CHECKED);
    power_profile_set(&s_local_cfg);
}
static void cb_sw_gate_sensors(lv_event_t * e) {
    s_local_cfg.gate_sensors = lv_obj_has_state(s_sw_gate_sensors, LV_STATE_CHECKED);
    power_profile_set(&s_local_cfg);
}

void ui_power_profile_init(lv_obj_t *parent_screen) {
    if (s_root_container) return;

    // Root full-screen container (640x172)
    s_root_container = lv_obj_create(parent_screen);
    lv_obj_set_size(s_root_container, 640, 172);
    lv_obj_center(s_root_container);
    lv_obj_set_style_bg_color(s_root_container, lv_color_hex(0x0C101A), 0);
    lv_obj_set_style_bg_opa(s_root_container, 255, 0);
    lv_obj_set_style_border_width(s_root_container, 0, 0);
    lv_obj_set_style_radius(s_root_container, 0, 0);
    lv_obj_set_style_pad_all(s_root_container, 0, 0);
    lv_obj_add_flag(s_root_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_root_container, LV_OBJ_FLAG_SCROLLABLE);

    // Header Title
    lv_obj_t *title = lv_label_create(s_root_container);
    lv_label_set_text(title, "Power & Breathing Profile");
    lv_obj_set_style_text_color(title, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(title, 14, 10);

    // Floating Close Button [X] matching Settings standard
    lv_obj_t *close_btn = lv_btn_create(s_root_container);
    lv_obj_set_size(close_btn, 48, 32);
    lv_obj_set_pos(close_btn, 582, 4);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x1B2338), 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_border_width(close_btn, 1, 0);
    lv_obj_set_style_radius(close_btn, 4, 0);
    lv_obj_add_event_cb(close_btn, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_center(close_lbl);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0x00F0FF), 0);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_16, 0);

    // Scrollable Box for touch controls
    s_scroll_box = lv_obj_create(s_root_container);
    lv_obj_set_size(s_scroll_box, 640, 132);
    lv_obj_set_pos(s_scroll_box, 0, 40);
    lv_obj_set_style_bg_opa(s_scroll_box, 0, 0);
    lv_obj_set_style_border_width(s_scroll_box, 0, 0);
    lv_obj_set_style_radius(s_scroll_box, 0, 0);
    lv_obj_set_style_pad_hor(s_scroll_box, 12, 0);
    lv_obj_set_style_pad_ver(s_scroll_box, 6, 0);
    lv_obj_set_style_pad_row(s_scroll_box, 8, 0);
    lv_obj_set_flex_flow(s_scroll_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_scroll_box, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll_box, LV_SCROLLBAR_MODE_AUTO);

    // 1. Min Brightness Floor (BL Start)
    create_stepper_row(s_scroll_box, "BL Start (Floor)", &s_lbl_bl_start_val, cb_bl_start_minus, cb_bl_start_plus);

    // 2. Max Brightness Ceiling (BL End)
    create_stepper_row(s_scroll_box, "BL End (Ceiling)", &s_lbl_bl_end_val, cb_bl_end_minus, cb_bl_end_plus);

    // 3. Dark Level (BL for Black)
    create_stepper_row(s_scroll_box, "BL for Black", &s_lbl_bl_black_val, cb_bl_black_minus, cb_bl_black_plus);

    // 4. Fade In (Ramp Up) Time
    create_stepper_row(s_scroll_box, "Ramp Up (Fade In)", &s_lbl_ramp_up_val, cb_ramp_up_minus, cb_ramp_up_plus);

    // 5. Full Brightness (Hold High) Time
    create_stepper_row(s_scroll_box, "Peak (Full Bright)", &s_lbl_hold_high_val, cb_hold_high_minus, cb_hold_high_plus);

    // 6. Fade Out (Ramp Down) Time
    create_stepper_row(s_scroll_box, "Ramp Down (Fade Out)", &s_lbl_ramp_down_val, cb_ramp_down_minus, cb_ramp_down_plus);

    // 7. Full Dark (Hold Dark) Time
    create_stepper_row(s_scroll_box, "Dark Rest Time", &s_lbl_hold_dark_val, cb_hold_dark_minus, cb_hold_dark_plus);

    // 8. Inactivity Tiering Switch
    create_switch_row(s_scroll_box, "Double Dark after 30m Idle", &s_sw_inactivity, cb_sw_inactivity);

    // 9. Wi-Fi Power Gating Switch
    create_switch_row(s_scroll_box, "Wi-Fi Duty Cycle (Dark Gating)", &s_sw_gate_wifi, cb_sw_gate_wifi);

    // 10. Audio Amp Gating Switch
    create_switch_row(s_scroll_box, "Mute Audio Amp when Dark", &s_sw_gate_audio, cb_sw_gate_audio);

    // 11. IMU Sensor Sleep Gating Switch
    create_switch_row(s_scroll_box, "Sleep IMU Sensor when Dark", &s_sw_gate_sensors, cb_sw_gate_sensors);

    // 12. Cloud Battery Calibration Card
    lv_obj_t *cal_card = lv_obj_create(s_scroll_box);
    lv_obj_set_size(cal_card, 608, 92);
    lv_obj_set_style_bg_color(cal_card, lv_color_hex(0x131A26), 0);
    lv_obj_set_style_border_color(cal_card, lv_color_hex(0x00A3FF), 0);
    lv_obj_set_style_border_width(cal_card, 1, 0);
    lv_obj_set_style_radius(cal_card, 6, 0);
    lv_obj_set_style_pad_all(cal_card, 6, 0);
    lv_obj_clear_flag(cal_card, LV_OBJ_FLAG_SCROLLABLE);

    s_lbl_cal_status = lv_label_create(cal_card);
    lv_obj_align(s_lbl_cal_status, LV_ALIGN_TOP_LEFT, 6, 2);
    lv_obj_set_style_text_color(s_lbl_cal_status, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(s_lbl_cal_status, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_lbl_cal_status, "Battery Calibration: Loading...");

    // Left info labels: Source & Last Pull Timestamp
    s_lbl_cal_source = lv_label_create(cal_card);
    lv_obj_align(s_lbl_cal_source, LV_ALIGN_TOP_LEFT, 6, 26);
    lv_obj_set_style_text_color(s_lbl_cal_source, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_text_font(s_lbl_cal_source, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_lbl_cal_source, "Source: Loading...");

    s_lbl_cal_time = lv_label_create(cal_card);
    lv_obj_align(s_lbl_cal_time, LV_ALIGN_TOP_LEFT, 6, 48);
    lv_obj_set_style_text_color(s_lbl_cal_time, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(s_lbl_cal_time, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_lbl_cal_time, "Last Pull: Never");

    // Right action button: Shorter Sync Button
    s_btn_sync_cal = lv_btn_create(cal_card);
    lv_obj_set_size(s_btn_sync_cal, 170, 42);
    lv_obj_align(s_btn_sync_cal, LV_ALIGN_BOTTOM_RIGHT, -8, -6);
    lv_obj_set_style_bg_color(s_btn_sync_cal, lv_color_hex(0x008855), 0);
    lv_obj_set_style_border_color(s_btn_sync_cal, lv_color_hex(0x00FF88), 0);
    lv_obj_set_style_border_width(s_btn_sync_cal, 1, 0);
    lv_obj_set_style_radius(s_btn_sync_cal, 6, 0);
    lv_obj_add_event_cb(s_btn_sync_cal, on_sync_cal_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *sync_lbl = lv_label_create(s_btn_sync_cal);
    lv_label_set_text(sync_lbl, LV_SYMBOL_DOWNLOAD " Sync Cloud");
    lv_obj_center(sync_lbl);
    lv_obj_set_style_text_color(sync_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(sync_lbl, &lv_font_montserrat_14, 0);
}

void ui_power_profile_open(void) {
    if (!s_root_container) return;

    // Load latest settings from power_profile module
    s_local_cfg = *power_profile_get();

    // Update switch states
    if (s_sw_inactivity) {
        if (s_local_cfg.inactivity_tier) lv_obj_add_state(s_sw_inactivity, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_sw_inactivity, LV_STATE_CHECKED);
    }
    if (s_sw_gate_wifi) {
        if (s_local_cfg.gate_wifi) lv_obj_add_state(s_sw_gate_wifi, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_sw_gate_wifi, LV_STATE_CHECKED);
    }
    if (s_sw_gate_audio) {
        if (s_local_cfg.gate_audio) lv_obj_add_state(s_sw_gate_audio, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_sw_gate_audio, LV_STATE_CHECKED);
    }
    if (s_sw_gate_sensors) {
        if (s_local_cfg.gate_sensors) lv_obj_add_state(s_sw_gate_sensors, LV_STATE_CHECKED);
        else lv_obj_clear_state(s_sw_gate_sensors, LV_STATE_CHECKED);
    }

    update_all_labels();
    if (s_scroll_box) {
        lv_obj_scroll_to_y(s_scroll_box, 0, LV_ANIM_OFF);
    }

    lv_obj_clear_flag(s_root_container, LV_OBJ_FLAG_HIDDEN);
    if (ui_Utility_Button_returnMenu) {
        lv_obj_add_flag(ui_Utility_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_power_profile_close(void) {
    if (s_root_container) {
        lv_obj_add_flag(s_root_container, LV_OBJ_FLAG_HIDDEN);
    }
    if (ui_Utility_Button_returnMenu) {
        lv_obj_clear_flag(ui_Utility_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
    }
    power_profile_set(&s_local_cfg);
}

bool ui_power_profile_is_open(void) {
    if (!s_root_container) return false;
    return !lv_obj_has_flag(s_root_container, LV_OBJ_FLAG_HIDDEN);
}

void ui_power_profile_scroll_to(int16_t y) {
    if (s_scroll_box) {
        lv_obj_scroll_to_y(s_scroll_box, y, LV_ANIM_OFF);
    }
}

void ui_power_profile_refresh(void) {
    update_all_labels();
}
