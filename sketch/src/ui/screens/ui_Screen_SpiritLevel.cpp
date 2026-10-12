#include "ui_Screen_SpiritLevel.h"
#include "../../imu/imu_service.h"
#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

extern "C" {
    extern uint32_t SCREEN_OFF_TIMER;
    extern lv_obj_t * ui_Screen_Utility;
    extern lv_obj_t * ui_Utility_Button_returnMenu;
}

// -------------------------------------------------------------
// Top Level Container & Tab Switcher
// -------------------------------------------------------------
static lv_obj_t * s_container = NULL;

static lv_obj_t * s_tab_btn_raw = NULL;
static lv_obj_t * s_tab_btn_level = NULL;
static lv_obj_t * s_tab_btn_calib = NULL;

static lv_obj_t * s_panel_raw = NULL;
static lv_obj_t * s_panel_level = NULL;
static lv_obj_t * s_panel_calib = NULL;

static int s_active_tab = 1; // 0=Raw, 1=Spirit Level (Default), 2=Calibration

// -------------------------------------------------------------
// Screen 1: Raw IMU Scores & Named Orientations
// -------------------------------------------------------------
static lv_obj_t * s_lbl_raw_accel_x = NULL;
static lv_obj_t * s_lbl_raw_accel_y = NULL;
static lv_obj_t * s_lbl_raw_accel_z = NULL;
static lv_obj_t * s_raw_sparkline_chart = NULL;
static lv_chart_series_t * s_raw_sparkline_series = NULL;
static lv_obj_t * s_lbl_tolerances_val = NULL;

struct NamedOrientationRow {
    const char *name;
    const char *threshold_str;
    float expected_ax;
    float expected_ay;
    float expected_az;
    lv_obj_t *row_container;
    lv_obj_t *status_label;
};

static NamedOrientationRow s_named_orientations[4] = {
    {"[01] NORMAL LANDSCAPE",   "X[>0.8]",   0.0f,  1.0f,  0.0f, NULL, NULL},
    {"[02] INVERTED LANDSCAPE", "X[<-0.8]",  0.0f, -1.0f,  0.0f, NULL, NULL},
    {"[03] FLAT DESK",          "Z[<1.2]",   0.0f,  0.0f,  1.0f, NULL, NULL},
    {"[04] PORTRAIT",           "Y[>0.8]",   1.0f,  0.0f,  0.0f, NULL, NULL}
};

static float s_tolerance_deg = 2.0f;
static float s_tol_x = 0.05f;
static float s_tol_y = 0.10f;
static float s_tol_z = 0.08f;

// -------------------------------------------------------------
// Screen 2: 2D Bullseye Spirit Level & Dual Sliders (Default)
// -------------------------------------------------------------
static lv_obj_t * s_lbl_pitch_val = NULL;
static lv_obj_t * s_lbl_roll_val = NULL;
static lv_obj_t * s_bar_pitch_status = NULL;
static lv_obj_t * s_bar_roll_status = NULL;
static lv_obj_t * s_lbl_lock_status = NULL;

static lv_obj_t * s_reticle_frame = NULL;
static lv_obj_t * s_bubble_obj = NULL;
static lv_obj_t * s_inner_target_ring = NULL;

static lv_obj_t * s_pitch_slider_track = NULL;
static lv_obj_t * s_pitch_bubble_knob = NULL;

static lv_obj_t * s_roll_slider_track = NULL;
static lv_obj_t * s_roll_bubble_knob = NULL;

// -------------------------------------------------------------
// Screen 3: Guided Multi-Step Calibration Tool
// -------------------------------------------------------------
static lv_obj_t * s_cal_header_label = NULL;
static lv_obj_t * s_cal_chip_1 = NULL;
static lv_obj_t * s_cal_chip_2 = NULL;
static lv_obj_t * s_cal_active_step_lbl = NULL;
static lv_obj_t * s_cal_desc_lbl = NULL;
static lv_obj_t * s_btn_save_cal = NULL;
static lv_obj_t * s_lbl_save_cal = NULL;

static lv_obj_t * s_wireframe_diagram = NULL;
static lv_obj_t * s_wireframe_arrow = NULL;
static lv_obj_t * s_wireframe_usb = NULL;
static lv_obj_t * s_wireframe_usb_lbl = NULL;

struct CalPose {
    const char *title;
    const char *instruction;
    float target_ax;
    float target_ay;
    float target_az;
    bool recorded;
    float sample_ax;
    float sample_ay;
    float sample_az;
};

// Exact 8-step calibration poses per user logical order:
// (1) Screen facing wall, power buttons facing ceiling (Wall Upright)
// (2) Same as 1, but tilted back with screen facing slightly upwards (Wall Tilted Up)
// (3) Vertical flip of 1: Screen facing wall, buttons facing floor (Inverted Upright)
// (4) Vertical flip of 2: Screen tilted back, buttons facing floor (Inverted Tilted Down)
// (5) On left edge: USB port MUST be on top
// (6) On right edge: USB port MUST be at bottom
// (7) Screen facing ceiling (Desk Face-Up / Flat Level)
// (8) Screen facing floor (Desk Face-Down)
static CalPose s_cal_poses[8] = {
    {"STEP 1: WALL UPRIGHT",       "Screen facing wall/user, power buttons facing ceiling.", 0.0f,  1.0f,  0.0f, false, 0, 0, 0},
    {"STEP 2: WALL TILTED UP",     "Tilted back with screen facing slightly upwards.",       0.0f,  0.86f, 0.5f, false, 0, 0, 0},
    {"STEP 3: INVERTED UPRIGHT",   "Vertical flip of Step 1: buttons facing floor.",         0.0f, -1.0f,  0.0f, false, 0, 0, 0},
    {"STEP 4: INVERTED TILTED",    "Vertical flip of Step 2: screen tilted downwards.",      0.0f, -0.86f, -0.5f, false, 0, 0, 0},
    {"STEP 5: LEFT EDGE REST",     "Resting on left edge. USB-C port MUST face top/ceiling.",1.0f,  0.0f,  0.0f, false, 0, 0, 0},
    {"STEP 6: RIGHT EDGE REST",    "Resting on right edge. USB-C port MUST face desk/bottom.",-1.0f, 0.0f, 0.0f, false, 0, 0, 0},
    {"STEP 7: DESK FACE-UP",       "Place TuneBar flat on desk, screen facing ceiling.",    0.0f,  0.0f,  1.0f, false, 0, 0, 0},
    {"STEP 8: DESK FACE-DOWN",     "Place TuneBar flat on desk, screen facing table/desk.",  0.0f,  0.0f, -1.0f, false, 0, 0, 0}
};

static int s_cal_step = 0; // 0 to 7
static uint32_t s_cal_delay_until_ms = 0;
static bool s_is_delaying = false;
static bool s_is_sampling = false;
static int s_sample_count = 0;
static float s_sample_accum_ax = 0.0f;
static float s_sample_accum_ay = 0.0f;
static float s_sample_accum_az = 0.0f;

// Calibration bias offsets (persisted to NVS)
static float s_cal_bias_ax = 0.0f;
static float s_cal_bias_ay = 0.0f;
static float s_cal_bias_az = 0.0f;

static Preferences s_prefs;

// -------------------------------------------------------------
// NVS Storage
// -------------------------------------------------------------
static void load_calibration_from_nvs(void) {
    if (s_prefs.begin("imu_cal", true)) {
        s_cal_bias_ax = s_prefs.getFloat("b_ax", 0.0f);
        s_cal_bias_ay = s_prefs.getFloat("b_ay", 0.0f);
        s_cal_bias_az = s_prefs.getFloat("b_az", 0.0f);
        s_tolerance_deg = s_prefs.getFloat("tol", 2.0f);
        s_tol_x = s_prefs.getFloat("tol_x", 0.05f);
        s_tol_y = s_prefs.getFloat("tol_y", 0.10f);
        s_tol_z = s_prefs.getFloat("tol_z", 0.08f);
        s_prefs.end();
    }
}

static void save_calibration_to_nvs(void) {
    if (s_prefs.begin("imu_cal", false)) {
        s_prefs.putFloat("b_ax", s_cal_bias_ax);
        s_prefs.putFloat("b_ay", s_cal_bias_ay);
        s_prefs.putFloat("b_az", s_cal_bias_az);
        s_prefs.putFloat("tol", s_tolerance_deg);
        s_prefs.putFloat("tol_x", s_tol_x);
        s_prefs.putFloat("tol_y", s_tol_y);
        s_prefs.putFloat("tol_z", s_tol_z);
        s_prefs.end();
    }
    // Synchronize bias offsets with system-wide IMU auto-rotation engine
    imu_sensor_set_calibration_bias(s_cal_bias_ax, s_cal_bias_ay, s_cal_bias_az);
}

// -------------------------------------------------------------
// UI Navigation / Tab Switching
// -------------------------------------------------------------
static void switch_tab(int tab_idx) {
    s_active_tab = tab_idx;
    if (s_panel_raw) {
        if (tab_idx == 0) lv_obj_clear_flag(s_panel_raw, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_panel_raw, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_panel_level) {
        if (tab_idx == 1) lv_obj_clear_flag(s_panel_level, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_panel_level, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_panel_calib) {
        if (tab_idx == 2) lv_obj_clear_flag(s_panel_calib, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_panel_calib, LV_OBJ_FLAG_HIDDEN);
    }

    // Update tab button styles
    if (s_tab_btn_raw) {
        lv_obj_set_style_border_color(s_tab_btn_raw, (tab_idx == 0) ? lv_color_hex(0x00F0FF) : lv_color_hex(0x203548), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_tab_btn_raw, (tab_idx == 0) ? lv_color_hex(0x10283C) : lv_color_hex(0x0B1220), LV_PART_MAIN);
    }
    if (s_tab_btn_level) {
        lv_obj_set_style_border_color(s_tab_btn_level, (tab_idx == 1) ? lv_color_hex(0x00F0FF) : lv_color_hex(0x203548), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_tab_btn_level, (tab_idx == 1) ? lv_color_hex(0x10283C) : lv_color_hex(0x0B1220), LV_PART_MAIN);
    }
    if (s_tab_btn_calib) {
        lv_obj_set_style_border_color(s_tab_btn_calib, (tab_idx == 2) ? lv_color_hex(0x00F0FF) : lv_color_hex(0x203548), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_tab_btn_calib, (tab_idx == 2) ? lv_color_hex(0x10283C) : lv_color_hex(0x0B1220), LV_PART_MAIN);
    }
}

static void tab_btn_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    switch_tab(idx);
}

static void close_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    ui_spirit_level_close();
}

// -------------------------------------------------------------
// Interactive Tolerance Steppers (Screen 1)
// -------------------------------------------------------------
static void tol_minus_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    if (s_tol_x > 0.01f) s_tol_x -= 0.01f;
    if (s_tol_y > 0.01f) s_tol_y -= 0.01f;
    if (s_tol_z > 0.01f) s_tol_z -= 0.01f;
    if (s_tolerance_deg > 0.5f) s_tolerance_deg -= 0.25f;
    save_calibration_to_nvs();
}

static void tol_plus_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    s_tol_x += 0.01f;
    s_tol_y += 0.01f;
    s_tol_z += 0.01f;
    s_tolerance_deg += 0.25f;
    save_calibration_to_nvs();
}

// -------------------------------------------------------------
// Calibration Wizard Handlers (Screen 3)
// -------------------------------------------------------------
static void update_cal_ui_step(void) {
    if (s_cal_header_label) {
        char hbuf[64];
        snprintf(hbuf, sizeof(hbuf), "CALIBRATION STATUS: IN PROGRESS (STEP %d/8)", s_cal_step + 1);
        lv_label_set_text(s_cal_header_label, hbuf);
    }
    if (s_cal_active_step_lbl) {
        char bbuf[64];
        snprintf(bbuf, sizeof(bbuf), "> %s [ACTIVE]", s_cal_poses[s_cal_step].title);
        lv_label_set_text(s_cal_active_step_lbl, bbuf);
    }
    if (s_cal_desc_lbl) {
        lv_label_set_text(s_cal_desc_lbl, s_cal_poses[s_cal_step].instruction);
    }
    if (s_cal_chip_1) {
        int c1_idx = (s_cal_step / 2) * 2;
        bool done1 = s_cal_poses[c1_idx].recorded;
        char buf1[32];
        snprintf(buf1, sizeof(buf1), "S%d: %s %s", c1_idx + 1, (c1_idx == 0 ? "WALL" : c1_idx == 2 ? "INV" : c1_idx == 4 ? "LEFT" : "DESK-UP"), done1 ? "[OK]" : "[..]");
        lv_label_set_text(s_cal_chip_1, buf1);
        lv_obj_set_style_border_color(s_cal_chip_1, done1 ? lv_color_hex(0x00FF88) : (s_cal_step == c1_idx ? lv_color_hex(0xFFA500) : lv_color_hex(0x406080)), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_cal_chip_1, done1 ? lv_color_hex(0x083318) : (s_cal_step == c1_idx ? lv_color_hex(0x282008) : lv_color_hex(0x101B2B)), LV_PART_MAIN);
    }
    if (s_cal_chip_2) {
        int c2_idx = (s_cal_step / 2) * 2 + 1;
        bool done2 = (c2_idx < 8) ? s_cal_poses[c2_idx].recorded : false;
        char buf2[32];
        snprintf(buf2, sizeof(buf2), "S%d: %s %s", c2_idx + 1, (c2_idx == 1 ? "TILT" : c2_idx == 3 ? "INV-T" : c2_idx == 5 ? "RIGHT" : "DESK-DN"), done2 ? "[OK]" : "[..]");
        lv_label_set_text(s_cal_chip_2, buf2);
        lv_obj_set_style_border_color(s_cal_chip_2, done2 ? lv_color_hex(0x00FF88) : (s_cal_step == c2_idx ? lv_color_hex(0xFFA500) : lv_color_hex(0x406080)), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_cal_chip_2, done2 ? lv_color_hex(0x083318) : (s_cal_step == c2_idx ? lv_color_hex(0x282008) : lv_color_hex(0x101B2B)), LV_PART_MAIN);
    }
    if (s_wireframe_usb_lbl) {
        if (s_cal_step == 4) { // STEP 5: Left edge -> USB TOP
            lv_label_set_text(s_wireframe_usb_lbl, "USB: TOP");
            lv_obj_set_style_text_color(s_wireframe_usb_lbl, lv_color_hex(0x00FF88), LV_PART_MAIN);
        } else if (s_cal_step == 5) { // STEP 6: Right edge -> USB BOTTOM
            lv_label_set_text(s_wireframe_usb_lbl, "USB: BOT");
            lv_obj_set_style_text_color(s_wireframe_usb_lbl, lv_color_hex(0xFFB800), LV_PART_MAIN);
        } else {
            lv_label_set_text(s_wireframe_usb_lbl, "USB: LFT");
            lv_obj_set_style_text_color(s_wireframe_usb_lbl, lv_color_hex(0x70A0C0), LV_PART_MAIN);
        }
    }
    if (s_lbl_save_cal) {
        lv_label_set_text(s_lbl_save_cal, "[SAVE CALIBRATION]");
        lv_obj_set_style_text_color(s_lbl_save_cal, lv_color_hex(0x00FF88), LV_PART_MAIN);
    }
}

static void cal_save_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    if (!s_is_delaying && !s_is_sampling) {
        // Enforce 0.5s stabilization delay so user has time to release hands & position device
        s_is_delaying = true;
        s_cal_delay_until_ms = millis() + 500;
        if (s_lbl_save_cal) {
            lv_label_set_text(s_lbl_save_cal, "[HOLD STILL... 0.5s]");
            lv_obj_set_style_text_color(s_lbl_save_cal, lv_color_hex(0xFFA500), LV_PART_MAIN);
        }
    }
}

static void cal_prev_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    if (s_cal_step > 0) {
        s_cal_step--;
        update_cal_ui_step();
    }
}

static void cal_next_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    if (s_cal_step < 7) {
        s_cal_step++;
        update_cal_ui_step();
    }
}

static void cal_reset_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    s_cal_step = 0;
    for (int i = 0; i < 8; i++) {
        s_cal_poses[i].recorded = false;
    }
    s_is_delaying = false;
    s_is_sampling = false;
    update_cal_ui_step();
}

// -------------------------------------------------------------
// 50Hz Periodic UI Refresh Timer
// -------------------------------------------------------------
static void spirit_level_timer_cb(lv_timer_t * timer) {
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    imu_reading_t imu;
    if (!imu_sensor_get_latest(&imu)) {
        return;
    }

    // Pitch: forward/back angle (-180 to +180)
    // Roll : side-to-side angle (-90 to +90)
    float pitch_deg = atan2f(-imu.ay, sqrtf(imu.ax * imu.ax + imu.az * imu.az)) * 180.0f / M_PI;
    float roll_deg = atan2f(imu.ax, imu.az) * 180.0f / M_PI;

    // Convert acceleration to m/s^2 for Screen 1
    float ax_ms2 = imu.ax * 9.80665f;
    float ay_ms2 = imu.ay * 9.80665f;
    float az_ms2 = imu.az * 9.80665f;

    // =========================================================
    // SCREEN 1: RAW IMU DATA & NAMED ORIENTATIONS
    // =========================================================
    if (s_active_tab == 0) {
        if (s_lbl_raw_accel_x) {
            char buf[48];
            snprintf(buf, sizeof(buf), "ACCEL_X: %+.2f (Range: -19.6..+19.6)", ax_ms2);
            lv_label_set_text(s_lbl_raw_accel_x, buf);
        }
        if (s_lbl_raw_accel_y) {
            char buf[32];
            snprintf(buf, sizeof(buf), "ACCEL_Y: %+.2f", ay_ms2);
            lv_label_set_text(s_lbl_raw_accel_y, buf);
        }
        if (s_lbl_raw_accel_z) {
            char buf[32];
            snprintf(buf, sizeof(buf), "ACCEL_Z: %+.2f", az_ms2);
            lv_label_set_text(s_lbl_raw_accel_z, buf);
        }
        if (s_raw_sparkline_chart && s_raw_sparkline_series) {
            // Push magnitude fluctuation into chart
            int16_t val = (int16_t)(sqrtf(ax_ms2 * ax_ms2 + ay_ms2 * ay_ms2 + az_ms2 * az_ms2) * 1.5f);
            lv_chart_set_next_value(s_raw_sparkline_chart, s_raw_sparkline_series, val);
        }
        if (s_lbl_tolerances_val) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%0.2f / %0.2f / %0.2f", s_tol_x, s_tol_y, s_tol_z);
            lv_label_set_text(s_lbl_tolerances_val, buf);
        }

        // Update Named Orientations Rows
        for (int i = 0; i < 4; i++) {
            bool matches = false;
            if (i == 0 && fabsf(imu.ax) < 0.35f && imu.ay > 0.70f) matches = true; // Normal Landscape
            else if (i == 1 && fabsf(imu.ax) < 0.35f && imu.ay < -0.70f) matches = true; // Inverted Landscape
            else if (i == 2 && fabsf(imu.az) > 0.80f && fabsf(imu.ay) < 0.35f) matches = true; // Flat Desk
            else if (i == 3 && fabsf(imu.ax) > 0.70f && fabsf(imu.ay) < 0.40f) matches = true; // Portrait

            if (s_named_orientations[i].row_container) {
                if (matches) {
                    lv_obj_set_style_border_color(s_named_orientations[i].row_container, lv_color_hex(0x00FF88), LV_PART_MAIN);
                    lv_obj_set_style_bg_color(s_named_orientations[i].row_container, lv_color_hex(0x0A2B18), LV_PART_MAIN);
                } else {
                    lv_obj_set_style_border_color(s_named_orientations[i].row_container, lv_color_hex(0x182838), LV_PART_MAIN);
                    lv_obj_set_style_bg_color(s_named_orientations[i].row_container, lv_color_hex(0x0A1220), LV_PART_MAIN);
                }
            }
            if (s_named_orientations[i].status_label) {
                lv_label_set_text(s_named_orientations[i].status_label, matches ? "[ACTIVE]" : "[IDLE]");
                lv_obj_set_style_text_color(s_named_orientations[i].status_label, matches ? lv_color_hex(0x00FF88) : lv_color_hex(0x607890), LV_PART_MAIN);
            }
        }
    }

    // =========================================================
    // SCREEN 2: 2D BULLSEYE SPIRIT LEVEL & DUAL SLIDERS
    // =========================================================
    if (s_active_tab == 1) {
        bool pitch_level = (fabsf(pitch_deg) <= s_tolerance_deg);
        bool roll_level  = (fabsf(roll_deg) <= s_tolerance_deg);
        bool perfectly_level = (pitch_level && roll_level);

        // Numeric Readouts & Indicators
        if (s_lbl_pitch_val) {
            char buf[32];
            snprintf(buf, sizeof(buf), "PITCH: %+.2f deg", pitch_deg);
            lv_label_set_text(s_lbl_pitch_val, buf);
        }
        if (s_lbl_roll_val) {
            char buf[32];
            snprintf(buf, sizeof(buf), "ROLL : %+.2f deg", roll_deg);
            lv_label_set_text(s_lbl_roll_val, buf);
        }
        if (s_bar_pitch_status) {
            lv_obj_set_style_bg_color(s_bar_pitch_status, pitch_level ? lv_color_hex(0x00FF88) : lv_color_hex(0xFF3344), LV_PART_MAIN);
        }
        if (s_bar_roll_status) {
            lv_obj_set_style_bg_color(s_bar_roll_status, roll_level ? lv_color_hex(0x00FF88) : lv_color_hex(0xFF3344), LV_PART_MAIN);
        }
        if (s_lbl_lock_status) {
            if (perfectly_level) {
                lv_label_set_text(s_lbl_lock_status, "[LOCKED] LEVEL");
                lv_obj_set_style_text_color(s_lbl_lock_status, lv_color_hex(0x00FF88), LV_PART_MAIN);
            } else {
                lv_label_set_text(s_lbl_lock_status, "[TILTED]");
                lv_obj_set_style_text_color(s_lbl_lock_status, lv_color_hex(0xFFA500), LV_PART_MAIN);
            }
        }

        // Center Bullseye Reticle Bubble Physics
        if (s_bubble_obj) {
            float max_disp = 44.0f;
            float dx = (roll_deg / 15.0f) * max_disp;
            float dy = (-pitch_deg / 15.0f) * max_disp;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > max_disp) {
                dx = (dx / dist) * max_disp;
                dy = (dy / dist) * max_disp;
            }

            int center_x = 86; // center inside s_reticle_frame
            int center_y = 66;
            lv_obj_set_pos(s_bubble_obj, center_x + (int)dx - 11, center_y + (int)dy - 11);

            if (s_inner_target_ring) {
                lv_obj_set_style_border_color(s_inner_target_ring, perfectly_level ? lv_color_hex(0x00FF88) : lv_color_hex(0xFF007F), LV_PART_MAIN);
            }
        }

        // Right Dual Horizontal Inclinometer Sliders
        if (s_pitch_bubble_knob) {
            // Track width is 200px, range -180 to +180 deg. Center (0 deg) is at x = 100 - 8 = 92
            float norm_p = (pitch_deg + 180.0f) / 360.0f;
            if (norm_p < 0.0f) norm_p = 0.0f;
            if (norm_p > 1.0f) norm_p = 1.0f;
            int kx = (int)(norm_p * 184.0f);
            lv_obj_set_pos(s_pitch_bubble_knob, kx, 1);
        }
        if (s_roll_bubble_knob) {
            // Track width is 200px, range -90 to +90 deg. Center (0 deg) is at x = 100 - 8 = 92
            float norm_r = (roll_deg + 90.0f) / 180.0f;
            if (norm_r < 0.0f) norm_r = 0.0f;
            if (norm_r > 1.0f) norm_r = 1.0f;
            int kx = (int)(norm_r * 184.0f);
            lv_obj_set_pos(s_roll_bubble_knob, kx, 1);
        }
    }

    // =========================================================
    // SCREEN 3: GUIDED MULTI-STEP CALIBRATION WIZARD
    // =========================================================
    if (s_active_tab == 2) {
        // Handle 0.5s Stabilization Delay
        if (s_is_delaying) {
            uint32_t now = millis();
            if (now >= s_cal_delay_until_ms) {
                s_is_delaying = false;
                s_is_sampling = true;
                s_sample_count = 0;
                s_sample_accum_ax = 0.0f;
                s_sample_accum_ay = 0.0f;
                s_sample_accum_az = 0.0f;
                if (s_lbl_save_cal) {
                    lv_label_set_text(s_lbl_save_cal, "[SAMPLING 0/50...]");
                    lv_obj_set_style_text_color(s_lbl_save_cal, lv_color_hex(0x00F0FF), LV_PART_MAIN);
                }
            } else {
                int ms_left = (int)(s_cal_delay_until_ms - now);
                char tbuf[32];
                snprintf(tbuf, sizeof(tbuf), "[HOLD STILL... %0.1fs]", (float)ms_left / 1000.0f);
                if (s_lbl_save_cal) lv_label_set_text(s_lbl_save_cal, tbuf);
            }
        } else if (s_is_sampling) {
            // Sample uncalibrated raw acceleration so true zero-bias is captured
            float raw_ax, raw_ay, raw_az;
            if (imu_sensor_get_uncalibrated_raw(&raw_ax, &raw_ay, &raw_az)) {
                s_sample_accum_ax += raw_ax;
                s_sample_accum_ay += raw_ay;
                s_sample_accum_az += raw_az;
                s_sample_count++;

                if (s_lbl_save_cal && (s_sample_count % 5 == 0)) {
                    char tbuf[32];
                    snprintf(tbuf, sizeof(tbuf), "[SAMPLING %d/50]", s_sample_count);
                    lv_label_set_text(s_lbl_save_cal, tbuf);
                }

                if (s_sample_count >= 50) {
                    s_is_sampling = false;
                    float avg_ax = s_sample_accum_ax / 50.0f;
                    float avg_ay = s_sample_accum_ay / 50.0f;
                    float avg_az = s_sample_accum_az / 50.0f;

                    s_cal_poses[s_cal_step].recorded = true;
                    s_cal_poses[s_cal_step].sample_ax = avg_ax;
                    s_cal_poses[s_cal_step].sample_ay = avg_ay;
                    s_cal_poses[s_cal_step].sample_az = avg_az;

                    // If Step 7 (Desk Face-Up, flat surface), true orientation is ax=0, ay=0, az=+1.0g
                    if (s_cal_step == 6) {
                        s_cal_bias_ax = avg_ax;
                        s_cal_bias_ay = avg_ay;
                        s_cal_bias_az = avg_az - 1.0f;
                        save_calibration_to_nvs();
                    }

                    // Advance step
                    if (s_cal_step < 7) {
                        s_cal_step++;
                        update_cal_ui_step();
                    } else {
                        // Multi-axis zero-bias calculation across opposite poses
                        if (s_cal_poses[4].recorded && s_cal_poses[5].recorded) {
                            s_cal_bias_ax = (s_cal_poses[4].sample_ax + s_cal_poses[5].sample_ax) / 2.0f;
                        }
                        if (s_cal_poses[0].recorded && s_cal_poses[2].recorded) {
                            s_cal_bias_ay = (s_cal_poses[0].sample_ay + s_cal_poses[2].sample_ay) / 2.0f;
                        }
                        if (s_cal_poses[6].recorded && s_cal_poses[7].recorded) {
                            s_cal_bias_az = (s_cal_poses[6].sample_az + s_cal_poses[7].sample_az) / 2.0f;
                        }
                        save_calibration_to_nvs();

                        if (s_cal_header_label) lv_label_set_text(s_cal_header_label, "CALIBRATION STATUS: COMPLETE (8/8 SAVED)");
                        if (s_lbl_save_cal) {
                            lv_label_set_text(s_lbl_save_cal, "[SAVED OK]");
                            lv_obj_set_style_text_color(s_lbl_save_cal, lv_color_hex(0x00FF88), LV_PART_MAIN);
                        }
                    }
                }
            }
        }
    }
}

// -------------------------------------------------------------
// UI Builder - Pixel-Accurate Schematic Reproduction
// -------------------------------------------------------------
void ui_spirit_level_init(lv_obj_t *parent_screen) {
    load_calibration_from_nvs();

    // Full screen overlay: 640x172 (Widescreen)
    s_container = lv_obj_create(parent_screen);
    lv_obj_set_size(s_container, 640, 172);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_container, lv_color_hex(0x080C14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_container, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_container, 0, LV_PART_MAIN);

    // =========================================================
    // TOP HEADER BAR & TACTICAL TAB SWITCHER
    // =========================================================
    lv_obj_t * top_bar = lv_obj_create(s_container);
    lv_obj_set_size(top_bar, 640, 28);
    lv_obj_set_pos(top_bar, 0, 0);
    lv_obj_clear_flag(top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(top_bar, lv_color_hex(0x0B1220), LV_PART_MAIN);
    lv_obj_set_style_border_color(top_bar, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(top_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
    lv_obj_set_style_pad_all(top_bar, 2, LV_PART_MAIN);

    // Main Header Title
    lv_obj_t * lbl_title = lv_label_create(top_bar);
    lv_obj_set_pos(lbl_title, 8, 4);
    lv_label_set_text(lbl_title, "TUNEBAR: IMU ANALYSIS MODULE V1.2");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_12, LV_PART_MAIN);

    // Tab 1 Button: [01/03] RAW DATA
    s_tab_btn_raw = lv_btn_create(top_bar);
    lv_obj_set_size(s_tab_btn_raw, 105, 22);
    lv_obj_set_pos(s_tab_btn_raw, 255, 1);
    lv_obj_set_style_bg_color(s_tab_btn_raw, lv_color_hex(0x0B1220), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_tab_btn_raw, lv_color_hex(0x203548), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_tab_btn_raw, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_tab_btn_raw, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_tab_btn_raw, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_tab_btn_raw, tab_btn_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)0);
    lv_obj_t * lbl_t1 = lv_label_create(s_tab_btn_raw);
    lv_label_set_text(lbl_t1, "[01] RAW DATA");
    lv_obj_set_align(lbl_t1, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(lbl_t1, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_t1, lv_color_hex(0x00F0FF), LV_PART_MAIN);

    // Tab 2 Button: [02/03] SPIRIT LEVEL (DEFAULT)
    s_tab_btn_level = lv_btn_create(top_bar);
    lv_obj_set_size(s_tab_btn_level, 115, 22);
    lv_obj_set_pos(s_tab_btn_level, 365, 1);
    lv_obj_set_style_bg_color(s_tab_btn_level, lv_color_hex(0x10283C), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_tab_btn_level, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_tab_btn_level, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_tab_btn_level, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_tab_btn_level, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_tab_btn_level, tab_btn_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)1);
    lv_obj_t * lbl_t2 = lv_label_create(s_tab_btn_level);
    lv_label_set_text(lbl_t2, "[02] SPIRIT LEVEL");
    lv_obj_set_align(lbl_t2, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(lbl_t2, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_t2, lv_color_hex(0x00FF88), LV_PART_MAIN);

    // Tab 3 Button: [03/03] CALIBRATION
    s_tab_btn_calib = lv_btn_create(top_bar);
    lv_obj_set_size(s_tab_btn_calib, 105, 22);
    lv_obj_set_pos(s_tab_btn_calib, 485, 1);
    lv_obj_set_style_bg_color(s_tab_btn_calib, lv_color_hex(0x0B1220), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_tab_btn_calib, lv_color_hex(0x203548), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_tab_btn_calib, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_tab_btn_calib, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_tab_btn_calib, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(s_tab_btn_calib, tab_btn_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)2);
    lv_obj_t * lbl_t3 = lv_label_create(s_tab_btn_calib);
    lv_label_set_text(lbl_t3, "[03] CALIBRATION");
    lv_obj_set_align(lbl_t3, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(lbl_t3, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(lbl_t3, lv_color_hex(0xFFB800), LV_PART_MAIN);

    // Large Close button ([X]) matching Settings & OTA enlarged standard
    lv_obj_t * close_btn = lv_btn_create(s_container);
    lv_obj_set_size(close_btn, 44, 28);
    lv_obj_set_pos(close_btn, 592, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x1B2338), LV_PART_MAIN);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(close_btn, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(close_btn, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, close_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_set_align(close_lbl, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_14, LV_PART_MAIN);

    // =========================================================
    // SCREEN 1: [01/03] RAW IMU DATA & NAMED ORIENTATIONS
    // =========================================================
    s_panel_raw = lv_obj_create(s_container);
    lv_obj_set_size(s_panel_raw, 640, 144);
    lv_obj_set_pos(s_panel_raw, 0, 28);
    lv_obj_clear_flag(s_panel_raw, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel_raw, LV_OBJ_FLAG_HIDDEN); // Hidden by default (tab 1 is default)
    lv_obj_set_style_bg_color(s_panel_raw, lv_color_hex(0x080C14), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel_raw, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_panel_raw, 4, LV_PART_MAIN);

    // Card 1: Left Raw IMU Card
    lv_obj_t * c1 = lv_obj_create(s_panel_raw);
    lv_obj_set_size(c1, 296, 134);
    lv_obj_set_pos(c1, 4, 2);
    lv_obj_clear_flag(c1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(c1, lv_color_hex(0x0A0F1A), LV_PART_MAIN);
    lv_obj_set_style_border_color(c1, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(c1, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c1, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c1, 6, LV_PART_MAIN);

    lv_obj_t * c1_hdr = lv_label_create(c1);
    lv_obj_set_pos(c1_hdr, 4, 2);
    lv_label_set_text(c1_hdr, "RAW IMU SCORES (m/s2)");
    lv_obj_set_style_text_color(c1_hdr, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(c1_hdr, &lv_font_montserrat_12, LV_PART_MAIN);

    s_lbl_raw_accel_x = lv_label_create(c1);
    lv_obj_set_pos(s_lbl_raw_accel_x, 4, 20);
    lv_label_set_text(s_lbl_raw_accel_x, "ACCEL_X: -0.12 (Range: -19.6..+19.6)");
    lv_obj_set_style_text_color(s_lbl_raw_accel_x, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_raw_accel_x, &lv_font_montserrat_10, LV_PART_MAIN);

    s_lbl_raw_accel_y = lv_label_create(c1);
    lv_obj_set_pos(s_lbl_raw_accel_y, 4, 38);
    lv_label_set_text(s_lbl_raw_accel_y, "ACCEL_Y: +9.79");
    lv_obj_set_style_text_color(s_lbl_raw_accel_y, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_raw_accel_y, &lv_font_montserrat_10, LV_PART_MAIN);

    s_lbl_raw_accel_z = lv_label_create(c1);
    lv_obj_set_pos(s_lbl_raw_accel_z, 4, 56);
    lv_label_set_text(s_lbl_raw_accel_z, "ACCEL_Z: +0.03");
    lv_obj_set_style_text_color(s_lbl_raw_accel_z, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_raw_accel_z, &lv_font_montserrat_10, LV_PART_MAIN);

    // Sparkline waveform chart
    s_raw_sparkline_chart = lv_chart_create(c1);
    lv_obj_set_size(s_raw_sparkline_chart, 90, 30);
    lv_obj_set_pos(s_raw_sparkline_chart, 192, 34);
    lv_chart_set_type(s_raw_sparkline_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_raw_sparkline_chart, 18);
    lv_chart_set_range(s_raw_sparkline_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 30);
    lv_obj_set_style_bg_color(s_raw_sparkline_chart, lv_color_hex(0x050810), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_raw_sparkline_chart, lv_color_hex(0x007090), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_raw_sparkline_chart, 1, LV_PART_MAIN);
    lv_obj_set_style_size(s_raw_sparkline_chart, 0, LV_PART_INDICATOR); // no point dots
    s_raw_sparkline_series = lv_chart_add_series(s_raw_sparkline_chart, lv_color_hex(0xFFE600), LV_CHART_AXIS_PRIMARY_Y);

    // Tolerances row
    lv_obj_t * tol_hdr = lv_label_create(c1);
    lv_obj_set_pos(tol_hdr, 4, 82);
    lv_label_set_text(tol_hdr, "TOLERANCES (X/Y/Z):");
    lv_obj_set_style_text_color(tol_hdr, lv_color_hex(0x70A0C0), LV_PART_MAIN);
    lv_obj_set_style_text_font(tol_hdr, &lv_font_montserrat_10, LV_PART_MAIN);

    s_lbl_tolerances_val = lv_label_create(c1);
    lv_obj_set_pos(s_lbl_tolerances_val, 4, 102);
    lv_label_set_text(s_lbl_tolerances_val, "0.05 / 0.10 / 0.08");
    lv_obj_set_style_text_color(s_lbl_tolerances_val, lv_color_hex(0xFFCC00), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_tolerances_val, &lv_font_montserrat_12, LV_PART_MAIN);

    lv_obj_t * b_m = lv_btn_create(c1);
    lv_obj_set_size(b_m, 26, 22);
    lv_obj_set_pos(b_m, 140, 98);
    lv_obj_set_style_bg_color(b_m, lv_color_hex(0x1B2338), LV_PART_MAIN);
    lv_obj_add_event_cb(b_m, tol_minus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lm = lv_label_create(b_m);
    lv_label_set_text(lm, "-");
    lv_obj_set_align(lm, LV_ALIGN_CENTER);

    lv_obj_t * b_step = lv_label_create(c1);
    lv_obj_set_pos(b_step, 172, 102);
    lv_label_set_text(b_step, "0.01");
    lv_obj_set_style_text_color(b_step, lv_color_hex(0x8EA5C8), LV_PART_MAIN);
    lv_obj_set_style_text_font(b_step, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t * b_p = lv_btn_create(c1);
    lv_obj_set_size(b_p, 26, 22);
    lv_obj_set_pos(b_p, 204, 98);
    lv_obj_set_style_bg_color(b_p, lv_color_hex(0x1B2338), LV_PART_MAIN);
    lv_obj_add_event_cb(b_p, tol_plus_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lp = lv_label_create(b_p);
    lv_label_set_text(lp, "+");
    lv_obj_set_align(lp, LV_ALIGN_CENTER);

    lv_obj_t * b_gear = lv_btn_create(c1);
    lv_obj_set_size(b_gear, 28, 22);
    lv_obj_set_pos(b_gear, 246, 98);
    lv_obj_set_style_bg_color(b_gear, lv_color_hex(0x005A7A), LV_PART_MAIN);
    lv_obj_t * lg = lv_label_create(b_gear);
    lv_label_set_text(lg, LV_SYMBOL_SETTINGS);
    lv_obj_set_align(lg, LV_ALIGN_CENTER);

    // Card 2: Right Named Orientations Card
    lv_obj_t * c2 = lv_obj_create(s_panel_raw);
    lv_obj_set_size(c2, 328, 134);
    lv_obj_set_pos(c2, 304, 2);
    lv_obj_clear_flag(c2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(c2, lv_color_hex(0x0A0F1A), LV_PART_MAIN);
    lv_obj_set_style_border_color(c2, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(c2, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(c2, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(c2, 4, LV_PART_MAIN);

    lv_obj_t * c2_hdr = lv_label_create(c2);
    lv_obj_set_pos(c2_hdr, 4, 2);
    lv_label_set_text(c2_hdr, "NAMED ORIENTATIONS          Active    Thresholds");
    lv_obj_set_style_text_color(c2_hdr, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(c2_hdr, &lv_font_montserrat_10, LV_PART_MAIN);

    for (int i = 0; i < 4; i++) {
        lv_obj_t * r = lv_obj_create(c2);
        lv_obj_set_size(r, 316, 24);
        lv_obj_set_pos(r, 2, 22 + (i * 26));
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x0B1220), LV_PART_MAIN);
        lv_obj_set_style_border_color(r, lv_color_hex(0x203548), LV_PART_MAIN);
        lv_obj_set_style_border_width(r, 1, LV_PART_MAIN);
        lv_obj_set_style_radius(r, 4, LV_PART_MAIN);
        lv_obj_set_style_pad_all(r, 2, LV_PART_MAIN);
        s_named_orientations[i].row_container = r;

        lv_obj_t * nm = lv_label_create(r);
        lv_obj_set_pos(nm, 4, 2);
        lv_label_set_text(nm, s_named_orientations[i].name);
        lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_10, LV_PART_MAIN);

        s_named_orientations[i].status_label = lv_label_create(r);
        lv_obj_set_pos(s_named_orientations[i].status_label, 172, 2);
        lv_label_set_text(s_named_orientations[i].status_label, "[IDLE]");
        lv_obj_set_style_text_color(s_named_orientations[i].status_label, lv_color_hex(0x607890), LV_PART_MAIN);
        lv_obj_set_style_text_font(s_named_orientations[i].status_label, &lv_font_montserrat_10, LV_PART_MAIN);

        lv_obj_t * th = lv_label_create(r);
        lv_obj_set_pos(th, 242, 2);
        lv_label_set_text(th, s_named_orientations[i].threshold_str);
        lv_obj_set_style_text_color(th, lv_color_hex(0x00D2FF), LV_PART_MAIN);
        lv_obj_set_style_text_font(th, &lv_font_montserrat_10, LV_PART_MAIN);
    }

    // =========================================================
    // SCREEN 2: [02/03] SPIRIT LEVEL & DUAL INCLINOMETERS (DEFAULT)
    // =========================================================
    s_panel_level = lv_obj_create(s_container);
    lv_obj_set_size(s_panel_level, 640, 144);
    lv_obj_set_pos(s_panel_level, 0, 28);
    lv_obj_clear_flag(s_panel_level, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_panel_level, lv_color_hex(0x080C14), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel_level, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_panel_level, 0, LV_PART_MAIN);

    // Column 1: Left Info Box
    lv_obj_t * p2_left = lv_obj_create(s_panel_level);
    lv_obj_set_size(p2_left, 160, 134);
    lv_obj_set_pos(p2_left, 6, 4);
    lv_obj_clear_flag(p2_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p2_left, lv_color_hex(0x0A0F1A), LV_PART_MAIN);
    lv_obj_set_style_border_color(p2_left, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(p2_left, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(p2_left, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p2_left, 6, LV_PART_MAIN);

    lv_obj_t * p2_hdr = lv_label_create(p2_left);
    lv_obj_set_pos(p2_hdr, 2, 2);
    lv_label_set_text(p2_hdr, "2D BULLSEYE\nINCLINOMETER");
    lv_obj_set_style_text_color(p2_hdr, lv_color_hex(0xFFB800), LV_PART_MAIN);
    lv_obj_set_style_text_font(p2_hdr, &lv_font_montserrat_12, LV_PART_MAIN);

    s_lbl_pitch_val = lv_label_create(p2_left);
    lv_obj_set_pos(s_lbl_pitch_val, 2, 40);
    lv_label_set_text(s_lbl_pitch_val, "PITCH: +0.00 deg");
    lv_obj_set_style_text_color(s_lbl_pitch_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_pitch_val, &lv_font_montserrat_12, LV_PART_MAIN);

    s_bar_pitch_status = lv_obj_create(p2_left);
    lv_obj_set_size(s_bar_pitch_status, 14, 6);
    lv_obj_set_pos(s_bar_pitch_status, 130, 44);
    lv_obj_set_style_bg_color(s_bar_pitch_status, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar_pitch_status, 0, LV_PART_MAIN);

    s_lbl_roll_val = lv_label_create(p2_left);
    lv_obj_set_pos(s_lbl_roll_val, 2, 64);
    lv_label_set_text(s_lbl_roll_val, "ROLL : +0.00 deg");
    lv_obj_set_style_text_color(s_lbl_roll_val, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_roll_val, &lv_font_montserrat_12, LV_PART_MAIN);

    s_bar_roll_status = lv_obj_create(p2_left);
    lv_obj_set_size(s_bar_roll_status, 14, 6);
    lv_obj_set_pos(s_bar_roll_status, 130, 68);
    lv_obj_set_style_bg_color(s_bar_roll_status, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar_roll_status, 0, LV_PART_MAIN);

    s_lbl_lock_status = lv_label_create(p2_left);
    lv_obj_set_pos(s_lbl_lock_status, 2, 94);
    lv_label_set_text(s_lbl_lock_status, "[LOCKED] LEVEL");
    lv_obj_set_style_text_color(s_lbl_lock_status, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_lock_status, &lv_font_montserrat_12, LV_PART_MAIN);

    // Column 2: Center Reticle Radar
    s_reticle_frame = lv_obj_create(s_panel_level);
    lv_obj_set_size(s_reticle_frame, 172, 134);
    lv_obj_set_pos(s_reticle_frame, 172, 4);
    lv_obj_clear_flag(s_reticle_frame, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_reticle_frame, lv_color_hex(0x060A14), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_reticle_frame, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_reticle_frame, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_reticle_frame, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_reticle_frame, 0, LV_PART_MAIN);

    // Concentric outer circle (dia 114)
    lv_obj_t * ring_out = lv_obj_create(s_reticle_frame);
    lv_obj_set_size(ring_out, 114, 114);
    lv_obj_set_pos(ring_out, 29, 9);
    lv_obj_clear_flag(ring_out, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ring_out, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring_out, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring_out, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(ring_out, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // Middle circle (dia 74)
    lv_obj_t * ring_mid = lv_obj_create(s_reticle_frame);
    lv_obj_set_size(ring_mid, 74, 74);
    lv_obj_set_pos(ring_mid, 49, 29);
    lv_obj_clear_flag(ring_mid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ring_mid, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(ring_mid, lv_color_hex(0x007090), LV_PART_MAIN);
    lv_obj_set_style_border_width(ring_mid, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ring_mid, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // Inner Target Area circle (dia 36)
    s_inner_target_ring = lv_obj_create(s_reticle_frame);
    lv_obj_set_size(s_inner_target_ring, 36, 36);
    lv_obj_set_pos(s_inner_target_ring, 68, 48);
    lv_obj_clear_flag(s_inner_target_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(s_inner_target_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_inner_target_ring, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_inner_target_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_inner_target_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // Crosshairs
    lv_obj_t * axis_h = lv_obj_create(s_reticle_frame);
    lv_obj_set_size(axis_h, 114, 1);
    lv_obj_set_pos(axis_h, 29, 66);
    lv_obj_set_style_bg_color(axis_h, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(axis_h, 0, LV_PART_MAIN);

    lv_obj_t * axis_v = lv_obj_create(s_reticle_frame);
    lv_obj_set_size(axis_v, 1, 114);
    lv_obj_set_pos(axis_v, 86, 9);
    lv_obj_set_style_bg_color(axis_v, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(axis_v, 0, LV_PART_MAIN);

    // Target callout label
    lv_obj_t * lbl_tgt = lv_label_create(s_reticle_frame);
    lv_obj_set_pos(lbl_tgt, 108, 108);
    lv_label_set_text(lbl_tgt, "Target Area");
    lv_obj_set_style_text_color(lbl_tgt, lv_color_hex(0xFF007F), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_tgt, &lv_font_montserrat_10, LV_PART_MAIN);

    // Liquid Bubble Sphere (dia 22px neon pink/magenta)
    s_bubble_obj = lv_obj_create(s_reticle_frame);
    lv_obj_set_size(s_bubble_obj, 22, 22);
    lv_obj_set_pos(s_bubble_obj, 75, 55); // centered
    lv_obj_clear_flag(s_bubble_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_bubble_obj, lv_color_hex(0xFF1493), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bubble_obj, lv_color_hex(0xFFA0D0), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bubble_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bubble_obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // Column 3: Right Dual Inclinometer Sliders
    lv_obj_t * p2_right = lv_obj_create(s_panel_level);
    lv_obj_set_size(p2_right, 280, 134);
    lv_obj_set_pos(p2_right, 352, 4);
    lv_obj_clear_flag(p2_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p2_right, lv_color_hex(0x0A0F1A), LV_PART_MAIN);
    lv_obj_set_style_border_color(p2_right, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(p2_right, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(p2_right, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p2_right, 4, LV_PART_MAIN);

    // PITCH Slider Row
    lv_obj_t * lbl_pitch_hdr = lv_label_create(p2_right);
    lv_obj_set_pos(lbl_pitch_hdr, 4, 18);
    lv_label_set_text(lbl_pitch_hdr, "PITCH  >");
    lv_obj_set_style_text_color(lbl_pitch_hdr, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_pitch_hdr, &lv_font_montserrat_12, LV_PART_MAIN);

    lv_obj_t * p_lim_l = lv_label_create(p2_right);
    lv_obj_set_pos(p_lim_l, 68, 2);
    lv_label_set_text(p_lim_l, "LIMIT -180");
    lv_obj_set_style_text_color(p_lim_l, lv_color_hex(0x70A0C0), LV_PART_MAIN);
    lv_obj_set_style_text_font(p_lim_l, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t * p_lim_r = lv_label_create(p2_right);
    lv_obj_set_pos(p_lim_r, 206, 2);
    lv_label_set_text(p_lim_r, "LIMIT +180");
    lv_obj_set_style_text_color(p_lim_r, lv_color_hex(0x70A0C0), LV_PART_MAIN);
    lv_obj_set_style_text_font(p_lim_r, &lv_font_montserrat_10, LV_PART_MAIN);

    s_pitch_slider_track = lv_obj_create(p2_right);
    lv_obj_set_size(s_pitch_slider_track, 200, 18);
    lv_obj_set_pos(s_pitch_slider_track, 68, 16);
    lv_obj_clear_flag(s_pitch_slider_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_pitch_slider_track, lv_color_hex(0x061828), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pitch_slider_track, lv_color_hex(0x00A0C0), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pitch_slider_track, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pitch_slider_track, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_pitch_slider_track, 0, LV_PART_MAIN);

    lv_obj_t * tri_p = lv_label_create(s_pitch_slider_track);
    lv_label_set_text(tri_p, LV_SYMBOL_UP);
    lv_obj_set_pos(tri_p, 92, 1);
    lv_obj_set_style_text_color(tri_p, lv_color_hex(0xFFCC00), LV_PART_MAIN);
    lv_obj_set_style_text_font(tri_p, &lv_font_montserrat_10, LV_PART_MAIN);

    s_pitch_bubble_knob = lv_obj_create(s_pitch_slider_track);
    lv_obj_set_size(s_pitch_bubble_knob, 14, 14);
    lv_obj_set_pos(s_pitch_bubble_knob, 92, 2);
    lv_obj_clear_flag(s_pitch_bubble_knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_pitch_bubble_knob, lv_color_hex(0xFF1493), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_pitch_bubble_knob, lv_color_hex(0xFFA0D0), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_pitch_bubble_knob, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_pitch_bubble_knob, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // ROLL Slider Row
    lv_obj_t * lr = lv_label_create(p2_right);
    lv_obj_set_pos(lr, 4, 80);
    lv_label_set_text(lr, "ROLL   >");
    lv_obj_set_style_text_color(lr, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(lr, &lv_font_montserrat_12, LV_PART_MAIN);

    lv_obj_t * r_lim_l = lv_label_create(p2_right);
    lv_obj_set_pos(r_lim_l, 68, 64);
    lv_label_set_text(r_lim_l, "LIMIT -90");
    lv_obj_set_style_text_color(r_lim_l, lv_color_hex(0x70A0C0), LV_PART_MAIN);
    lv_obj_set_style_text_font(r_lim_l, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t * r_lim_r = lv_label_create(p2_right);
    lv_obj_set_pos(r_lim_r, 206, 64);
    lv_label_set_text(r_lim_r, "LIMIT +90");
    lv_obj_set_style_text_color(r_lim_r, lv_color_hex(0x70A0C0), LV_PART_MAIN);
    lv_obj_set_style_text_font(r_lim_r, &lv_font_montserrat_10, LV_PART_MAIN);

    s_roll_slider_track = lv_obj_create(p2_right);
    lv_obj_set_size(s_roll_slider_track, 200, 18);
    lv_obj_set_pos(s_roll_slider_track, 68, 78);
    lv_obj_clear_flag(s_roll_slider_track, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_roll_slider_track, lv_color_hex(0x061828), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_roll_slider_track, lv_color_hex(0x00A0C0), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_roll_slider_track, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_roll_slider_track, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_roll_slider_track, 0, LV_PART_MAIN);

    lv_obj_t * tri_r = lv_label_create(s_roll_slider_track);
    lv_label_set_text(tri_r, LV_SYMBOL_UP);
    lv_obj_set_pos(tri_r, 92, 1);
    lv_obj_set_style_text_color(tri_r, lv_color_hex(0xFFCC00), LV_PART_MAIN);
    lv_obj_set_style_text_font(tri_r, &lv_font_montserrat_10, LV_PART_MAIN);

    s_roll_bubble_knob = lv_obj_create(s_roll_slider_track);
    lv_obj_set_size(s_roll_bubble_knob, 14, 14);
    lv_obj_set_pos(s_roll_bubble_knob, 92, 2);
    lv_obj_clear_flag(s_roll_bubble_knob, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_roll_bubble_knob, lv_color_hex(0xFF1493), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_roll_bubble_knob, lv_color_hex(0xFFA0D0), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_roll_bubble_knob, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_roll_bubble_knob, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // =========================================================
    // SCREEN 3: [03/03] GUIDED MULTI-STEP CALIBRATION TOOL
    // =========================================================
    s_panel_calib = lv_obj_create(s_container);
    lv_obj_set_size(s_panel_calib, 640, 144);
    lv_obj_set_pos(s_panel_calib, 0, 28);
    lv_obj_clear_flag(s_panel_calib, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel_calib, LV_OBJ_FLAG_HIDDEN); // Hidden by default
    lv_obj_set_style_bg_color(s_panel_calib, lv_color_hex(0x080C14), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel_calib, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_panel_calib, 4, LV_PART_MAIN);

    // Top Header Banner
    s_cal_header_label = lv_label_create(s_panel_calib);
    lv_obj_set_pos(s_cal_header_label, 10, 4);
    lv_label_set_text(s_cal_header_label, "CALIBRATION STATUS: IN PROGRESS (STEP 1/8)");
    lv_obj_set_style_text_color(s_cal_header_label, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_cal_header_label, &lv_font_montserrat_12, LV_PART_MAIN);

    // Left Flow & Navigation Area
    lv_obj_t * p3_left = lv_obj_create(s_panel_calib);
    lv_obj_set_size(p3_left, 240, 110);
    lv_obj_set_pos(p3_left, 8, 26);
    lv_obj_clear_flag(p3_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p3_left, lv_color_hex(0x0A0F1A), LV_PART_MAIN);
    lv_obj_set_style_border_color(p3_left, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(p3_left, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(p3_left, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p3_left, 4, LV_PART_MAIN);

    // Step Breadcrumbs
    s_cal_chip_1 = lv_label_create(p3_left);
    lv_obj_set_size(s_cal_chip_1, 108, 22);
    lv_obj_set_pos(s_cal_chip_1, 4, 4);
    lv_label_set_text(s_cal_chip_1, "STEP 1: WALL");
    lv_obj_set_style_text_font(s_cal_chip_1, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_cal_chip_1, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_cal_chip_1, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cal_chip_1, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_cal_chip_1, 3, LV_PART_MAIN);

    s_cal_chip_2 = lv_label_create(p3_left);
    lv_obj_set_size(s_cal_chip_2, 108, 22);
    lv_obj_set_pos(s_cal_chip_2, 118, 4);
    lv_label_set_text(s_cal_chip_2, "STEP 2: TILT");
    lv_obj_set_style_text_font(s_cal_chip_2, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_cal_chip_2, lv_color_hex(0x8EA5C8), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_cal_chip_2, lv_color_hex(0x203548), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cal_chip_2, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_cal_chip_2, 3, LV_PART_MAIN);

    // Navigation buttons row
    lv_obj_t * b_prev = lv_btn_create(p3_left);
    lv_obj_set_size(b_prev, 68, 26);
    lv_obj_set_pos(b_prev, 4, 70);
    lv_obj_set_style_bg_color(b_prev, lv_color_hex(0x162030), LV_PART_MAIN);
    lv_obj_add_event_cb(b_prev, cal_prev_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l_prv = lv_label_create(b_prev);
    lv_label_set_text(l_prv, "PREV");
    lv_obj_set_align(l_prv, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(l_prv, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t * b_nxt = lv_btn_create(p3_left);
    lv_obj_set_size(b_nxt, 68, 26);
    lv_obj_set_pos(b_nxt, 76, 70);
    lv_obj_set_style_bg_color(b_nxt, lv_color_hex(0x0A3858), LV_PART_MAIN);
    lv_obj_set_style_border_color(b_nxt, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(b_nxt, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(b_nxt, cal_next_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l_nxt = lv_label_create(b_nxt);
    lv_label_set_text(l_nxt, "NEXT");
    lv_obj_set_align(l_nxt, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(l_nxt, &lv_font_montserrat_10, LV_PART_MAIN);

    lv_obj_t * b_re = lv_btn_create(p3_left);
    lv_obj_set_size(b_re, 78, 26);
    lv_obj_set_pos(b_re, 148, 70);
    lv_obj_set_style_bg_color(b_re, lv_color_hex(0x3A2410), LV_PART_MAIN);
    lv_obj_set_style_border_color(b_re, lv_color_hex(0xFF8C00), LV_PART_MAIN);
    lv_obj_set_style_border_width(b_re, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(b_re, cal_reset_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * l_re = lv_label_create(b_re);
    lv_label_set_text(l_re, "RESET");
    lv_obj_set_align(l_re, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(l_re, &lv_font_montserrat_10, LV_PART_MAIN);

    // Right Visual Diagram & Action Area
    lv_obj_t * p3_right = lv_obj_create(s_panel_calib);
    lv_obj_set_size(p3_right, 376, 110);
    lv_obj_set_pos(p3_right, 254, 26);
    lv_obj_clear_flag(p3_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(p3_right, lv_color_hex(0x0A0F1A), LV_PART_MAIN);
    lv_obj_set_style_border_color(p3_right, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(p3_right, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(p3_right, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_all(p3_right, 4, LV_PART_MAIN);

    // Isometric Device Wireframe
    s_wireframe_diagram = lv_obj_create(p3_right);
    lv_obj_set_size(s_wireframe_diagram, 100, 44);
    lv_obj_set_pos(s_wireframe_diagram, 6, 26);
    lv_obj_clear_flag(s_wireframe_diagram, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_wireframe_diagram, lv_color_hex(0x081525), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_wireframe_diagram, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wireframe_diagram, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wireframe_diagram, 4, LV_PART_MAIN);

    // Top screen representation inside chassis
    lv_obj_t * wf_screen = lv_obj_create(s_wireframe_diagram);
    lv_obj_set_size(wf_screen, 84, 18);
    lv_obj_set_pos(wf_screen, 6, 4);
    lv_obj_set_style_bg_color(wf_screen, lv_color_hex(0x00334D), LV_PART_MAIN);
    lv_obj_set_style_border_width(wf_screen, 0, LV_PART_MAIN);

    s_wireframe_arrow = lv_label_create(s_wireframe_diagram);
    lv_obj_set_pos(s_wireframe_arrow, 38, 22);
    lv_label_set_text(s_wireframe_arrow, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(s_wireframe_arrow, lv_color_hex(0xFFA500), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_wireframe_arrow, &lv_font_montserrat_12, LV_PART_MAIN);

    // USB Port orientation indicator badge
    s_wireframe_usb = lv_obj_create(p3_right);
    lv_obj_set_size(s_wireframe_usb, 64, 18);
    lv_obj_set_pos(s_wireframe_usb, 24, 76);
    lv_obj_clear_flag(s_wireframe_usb, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_wireframe_usb, lv_color_hex(0x081525), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_wireframe_usb, lv_color_hex(0x00A0C0), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_wireframe_usb, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_wireframe_usb, 3, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_wireframe_usb, 0, LV_PART_MAIN);

    s_wireframe_usb_lbl = lv_label_create(s_wireframe_usb);
    lv_label_set_text(s_wireframe_usb_lbl, "USB: LFT");
    lv_obj_set_align(s_wireframe_usb_lbl, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_wireframe_usb_lbl, &lv_font_montserrat_10, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wireframe_usb_lbl, lv_color_hex(0x70A0C0), LV_PART_MAIN);

    // Step Title & Instruction Text
    s_cal_active_step_lbl = lv_label_create(p3_right);
    lv_obj_set_pos(s_cal_active_step_lbl, 114, 4);
    lv_label_set_text(s_cal_active_step_lbl, "> STEP 1: WALL UPRIGHT [ACTIVE]");
    lv_obj_set_style_text_color(s_cal_active_step_lbl, lv_color_hex(0xFFA500), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_cal_active_step_lbl, &lv_font_montserrat_10, LV_PART_MAIN);

    s_cal_desc_lbl = lv_label_create(p3_right);
    lv_obj_set_pos(s_cal_desc_lbl, 114, 22);
    lv_obj_set_width(s_cal_desc_lbl, 250);
    lv_label_set_text(s_cal_desc_lbl, "Screen facing wall/user, power buttons facing ceiling.");
    lv_obj_set_style_text_color(s_cal_desc_lbl, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_cal_desc_lbl, &lv_font_montserrat_10, LV_PART_MAIN);

    // Big Prominent Neon Green Button: [SAVE CALIBRATION]
    s_btn_save_cal = lv_btn_create(p3_right);
    lv_obj_set_size(s_btn_save_cal, 175, 34);
    lv_obj_set_pos(s_btn_save_cal, 185, 66);
    lv_obj_set_style_bg_color(s_btn_save_cal, lv_color_hex(0x003318), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_btn_save_cal, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_btn_save_cal, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_btn_save_cal, 6, LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_save_cal, cal_save_click_cb, LV_EVENT_CLICKED, NULL);

    s_lbl_save_cal = lv_label_create(s_btn_save_cal);
    lv_label_set_text(s_lbl_save_cal, "[SAVE CALIBRATION]");
    lv_obj_set_align(s_lbl_save_cal, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_lbl_save_cal, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_save_cal, lv_color_hex(0x00FF88), LV_PART_MAIN);

    // Initial state: Show Tab 2 (Spirit Level default)
    switch_tab(1);

    // 50Hz periodic timer (20ms) for smooth liquid bubble physics and telemetry
    lv_timer_create(spirit_level_timer_cb, 20, NULL);
}

void ui_spirit_level_open(void) {
    if (s_container) {
        lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
        switch_tab(1); // Always default to Screen 2: Spirit Level
        SCREEN_OFF_TIMER = millis();
    }
}

void ui_spirit_level_close(void) {
    if (s_container) {
        lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
        SCREEN_OFF_TIMER = millis();
    }
}
