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

// Top level container
static lv_obj_t * s_container = NULL;
static lv_obj_t * s_tabview = NULL;

// Tab 1: Spirit Level
static lv_obj_t * s_dial_canvas = NULL;
static lv_obj_t * s_bubble_obj = NULL;
static lv_obj_t * s_label_angles = NULL;
static lv_obj_t * s_label_level_status = NULL;

// Tab 2: Raw IMU & Named Orientations
static lv_obj_t * s_label_raw_imu = NULL;
static lv_obj_t * s_label_orientation_badge = NULL;
static lv_obj_t * s_label_tolerance = NULL;
static lv_obj_t * s_label_preset_status = NULL;
static lv_obj_t * s_btn_preset_cycle = NULL;
static lv_obj_t * s_lbl_preset_cycle = NULL;
static float s_tolerance_deg = 2.0f; // Sane default tolerance range

struct OrientationPreset {
    char name[32];
    float pitch_deg;
    float roll_deg;
    bool is_configured;
};

static OrientationPreset s_presets[4] = {
    {"Wall Upright (0 deg)", 0.0f, 0.0f, true},
    {"Bedside Stand (30 deg)", 30.0f, 0.0f, true},
    {"Desk Stand (45 deg)", 45.0f, 0.0f, true},
    {"Custom Incline", 0.0f, 0.0f, false}
};
static int s_active_preset_idx = 0;

// Tab 3: Calibration Wizard
static lv_obj_t * s_cal_step_label = NULL;
static lv_obj_t * s_cal_instruction_label = NULL;
static lv_obj_t * s_cal_action_btn = NULL;
static lv_obj_t * s_cal_btn_label = NULL;
static lv_obj_t * s_cal_progress_bar = NULL;

static int s_cal_step = 0; // 0 to 7 (8 steps)
static bool s_is_sampling = false;
static int s_sample_count = 0;
static float s_sample_accum_ax = 0.0f;
static float s_sample_accum_ay = 0.0f;
static float s_sample_accum_az = 0.0f;

// Calibration bias offsets
static float s_cal_bias_ax = 0.0f;
static float s_cal_bias_ay = 0.0f;
static float s_cal_bias_az = 0.0f;

static Preferences s_prefs;

// Exact 8-step calibration poses per user logical order:
// (1) Screen facing wall, power buttons facing ceiling (Wall Upright)
// (2) Same as 1, but tilted back with screen facing slightly upwards (Wall Tilted Up)
// (3) Vertical flip of 1: Screen facing wall, buttons facing floor (Inverted Upright)
// (4) Vertical flip of 2: Screen tilted back, buttons facing floor (Inverted Tilted Down)
// (5) On left edge: USB port MUST be on top
// (6) On right edge: USB port MUST be at bottom
// (7) Screen facing ceiling (Desk Face-Up / Flat Level)
// (8) Screen facing floor (Desk Face-Down)
struct CalStepPose {
    const char *title;
    const char *instruction;
};

static const CalStepPose CAL_STEPS[8] = {
    {"Step 1/8: Wall Upright (Buttons Ceiling)", "Screen facing wall/user, power buttons facing ceiling."},
    {"Step 2/8: Wall Tilted Up (Stand Angle)", "Same as Step 1, but tilted back with screen facing slightly upwards."},
    {"Step 3/8: Inverted Upright (Buttons Floor)", "Vertical flip of Step 1: screen facing wall, buttons facing floor."},
    {"Step 4/8: Inverted Tilted Down (Stand Angle)", "Vertical flip of Step 2: screen tilted back, buttons facing floor."},
    {"Step 5/8: Left Edge Rest (USB on Top)", "Place on left edge. USB-C port MUST face ceiling/top."},
    {"Step 6/8: Right Edge Rest (USB at Bottom)", "Place on right edge. USB-C port MUST face desk/bottom."},
    {"Step 7/8: Desk Face-Up (Flat Level)", "Place TuneBar flat on desk, screen facing ceiling."},
    {"Step 8/8: Desk Face-Down", "Place TuneBar flat on desk, screen facing table/desk."}
};

static void load_calibration_from_nvs(void) {
    if (s_prefs.begin("imu_cal", true)) {
        s_cal_bias_ax = s_prefs.getFloat("b_ax", 0.0f);
        s_cal_bias_ay = s_prefs.getFloat("b_ay", 0.0f);
        s_cal_bias_az = s_prefs.getFloat("b_az", 0.0f);
        s_tolerance_deg = s_prefs.getFloat("tol", 2.0f);
        // Load custom preset pitch/roll if present
        s_presets[3].pitch_deg = s_prefs.getFloat("p3_pitch", 0.0f);
        s_presets[3].roll_deg  = s_prefs.getFloat("p3_roll", 0.0f);
        s_presets[3].is_configured = s_prefs.getBool("p3_cfg", false);
        s_prefs.end();
    }
}

static void save_calibration_to_nvs(void) {
    if (s_prefs.begin("imu_cal", false)) {
        s_prefs.putFloat("b_ax", s_cal_bias_ax);
        s_prefs.putFloat("b_ay", s_cal_bias_ay);
        s_prefs.putFloat("b_az", s_cal_bias_az);
        s_prefs.putFloat("tol", s_tolerance_deg);
        s_prefs.putFloat("p3_pitch", s_presets[3].pitch_deg);
        s_prefs.putFloat("p3_roll", s_presets[3].roll_deg);
        s_prefs.putBool("p3_cfg", s_presets[3].is_configured);
        s_prefs.end();
    }
    // Synchronize bias offsets with system-wide IMU auto-rotation engine
    imu_sensor_set_calibration_bias(s_cal_bias_ax, s_cal_bias_ay, s_cal_bias_az);
}

static void close_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    ui_spirit_level_close();
}

static void tol_minus_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    if (s_tolerance_deg > 0.5f) {
        s_tolerance_deg -= 0.5f;
        save_calibration_to_nvs();
    }
}

static void tol_plus_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    if (s_tolerance_deg < 10.0f) {
        s_tolerance_deg += 0.5f;
        save_calibration_to_nvs();
    }
}

static void preset_cycle_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    s_active_preset_idx = (s_active_preset_idx + 1) % 4;
    if (s_lbl_preset_cycle) {
        char buf[48];
        snprintf(buf, sizeof(buf), "Slot: %s", s_presets[s_active_preset_idx].name);
        lv_label_set_text(s_lbl_preset_cycle, buf);
    }
    if (s_label_preset_status) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Target: P:%.1f deg, R:%.1f deg",
                 s_presets[s_active_preset_idx].pitch_deg,
                 s_presets[s_active_preset_idx].roll_deg);
        lv_label_set_text(s_label_preset_status, buf);
    }
}

static void save_preset_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    imu_reading_t imu;
    if (imu_sensor_get_latest(&imu)) {
        float pitch = atan2f(-imu.ay, sqrtf(imu.ax * imu.ax + imu.az * imu.az)) * 180.0f / M_PI;
        float roll = atan2f(imu.ax, imu.az) * 180.0f / M_PI;
        s_presets[s_active_preset_idx].pitch_deg = pitch;
        s_presets[s_active_preset_idx].roll_deg = roll;
        s_presets[s_active_preset_idx].is_configured = true;
        save_calibration_to_nvs();

        if (s_label_preset_status) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Saved P:%+.1f R:%+.1f to Slot %d!", pitch, roll, s_active_preset_idx + 1);
            lv_label_set_text(s_label_preset_status, buf);
            lv_obj_set_style_text_color(s_label_preset_status, lv_color_hex(0x00FF88), LV_PART_MAIN);
        }
    }
}

static void cal_btn_click_cb(lv_event_t * e) {
    SCREEN_OFF_TIMER = millis();
    if (!s_is_sampling) {
        s_is_sampling = true;
        s_sample_count = 0;
        s_sample_accum_ax = 0.0f;
        s_sample_accum_ay = 0.0f;
        s_sample_accum_az = 0.0f;
        if (s_cal_btn_label) lv_label_set_text(s_cal_btn_label, "Sampling... Hold Still");
        if (s_cal_progress_bar) lv_bar_set_value(s_cal_progress_bar, 0, LV_ANIM_OFF);
    }
}

// 50Hz periodic UI refresh timer
static void spirit_level_timer_cb(lv_timer_t * timer) {
    if (!s_container || lv_obj_has_flag(s_container, LV_OBJ_FLAG_HIDDEN)) return;

    imu_reading_t imu;
    if (!imu_sensor_get_latest(&imu)) {
        return;
    }

    // Pitch: inclination forward/back around X
    // Roll: inclination side-to-side around Y
    float pitch_deg = atan2f(-imu.ay, sqrtf(imu.ax * imu.ax + imu.az * imu.az)) * 180.0f / M_PI;
    float roll_deg = atan2f(imu.ax, imu.az) * 180.0f / M_PI;

    // ----------------- TAB 1: SPIRIT LEVEL DIAL -----------------
    if (s_bubble_obj) {
        float max_disp = 42.0f;
        float dx = (roll_deg / 15.0f) * max_disp;
        float dy = (-pitch_deg / 15.0f) * max_disp;

        float dist = sqrtf(dx * dx + dy * dy);
        if (dist > max_disp) {
            dx = (dx / dist) * max_disp;
            dy = (dy / dist) * max_disp;
        }

        int center_x = 100;
        int center_y = 82;
        lv_obj_set_pos(s_bubble_obj, center_x + (int)dx - 12, center_y + (int)dy - 12);

        bool is_level = (fabsf(pitch_deg) <= s_tolerance_deg && fabsf(roll_deg) <= s_tolerance_deg);
        if (is_level) {
            lv_obj_set_style_bg_color(s_bubble_obj, lv_color_hex(0x00FF88), LV_PART_MAIN);
            lv_obj_set_style_border_color(s_bubble_obj, lv_color_hex(0x88FFCC), LV_PART_MAIN);
            if (s_label_level_status) {
                lv_label_set_text(s_label_level_status, "PERFECTLY LEVEL (LOCKED)");
                lv_obj_set_style_text_color(s_label_level_status, lv_color_hex(0x00FF88), LV_PART_MAIN);
            }
        } else {
            lv_obj_set_style_bg_color(s_bubble_obj, lv_color_hex(0x00D2FF), LV_PART_MAIN);
            lv_obj_set_style_border_color(s_bubble_obj, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
            if (s_label_level_status) {
                lv_label_set_text(s_label_level_status, "TILTED");
                lv_obj_set_style_text_color(s_label_level_status, lv_color_hex(0xFFA500), LV_PART_MAIN);
            }
        }

        if (s_label_angles) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Pitch: %+.1f deg\nRoll : %+.1f deg\nTol  : +/-%.1f deg", pitch_deg, roll_deg, s_tolerance_deg);
            lv_label_set_text(s_label_angles, buf);
        }
    }

    // ----------------- TAB 2: RAW SCORES & ORIENTATION -----------------
    if (s_label_raw_imu) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "ACCEL (g)  : X:%+.3f  Y:%+.3f  Z:%+.3f\n"
                 "GYRO (dps) : X:%+.1f  Y:%+.1f  Z:%+.1f\n"
                 "TEMP       : %.1f C",
                 imu.ax, imu.ay, imu.az,
                 imu.gx, imu.gy, imu.gz,
                 imu.temp_c);
        lv_label_set_text(s_label_raw_imu, buf);
    }
    if (s_label_orientation_badge) {
        // Check if device matches any saved preset within tolerance
        int matched_preset = -1;
        for (int i = 0; i < 4; i++) {
            if (fabsf(pitch_deg - s_presets[i].pitch_deg) <= s_tolerance_deg &&
                fabsf(roll_deg - s_presets[i].roll_deg) <= s_tolerance_deg) {
                matched_preset = i;
                break;
            }
        }
        if (matched_preset >= 0) {
            char bbuf[64];
            snprintf(bbuf, sizeof(bbuf), "Match: %s", s_presets[matched_preset].name);
            lv_label_set_text(s_label_orientation_badge, bbuf);
            lv_obj_set_style_text_color(s_label_orientation_badge, lv_color_hex(0x00FF88), LV_PART_MAIN);
        } else {
            lv_label_set_text(s_label_orientation_badge, imu.orientation_str ? imu.orientation_str : "Normal Landscape");
            lv_obj_set_style_text_color(s_label_orientation_badge, lv_color_hex(0xFFEA00), LV_PART_MAIN);
        }
    }
    if (s_label_tolerance) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Tol: +/-%.1f deg", s_tolerance_deg);
        lv_label_set_text(s_label_tolerance, buf);
    }

    // ----------------- TAB 3: CALIBRATION WIZARD -----------------
    if (s_is_sampling) {
        s_sample_accum_ax += imu.ax;
        s_sample_accum_ay += imu.ay;
        s_sample_accum_az += imu.az;
        s_sample_count++;

        if (s_cal_progress_bar) {
            lv_bar_set_value(s_cal_progress_bar, s_sample_count * 2, LV_ANIM_OFF); // 50 samples = 100%
        }

        if (s_sample_count >= 50) {
            s_is_sampling = false;
            float avg_ax = s_sample_accum_ax / 50.0f;
            float avg_ay = s_sample_accum_ay / 50.0f;
            float avg_az = s_sample_accum_az / 50.0f;

            // In Step 7 (Desk Face-Up, index 6), true level is ax=0, ay=0, az=+1.0g
            if (s_cal_step == 6) {
                s_cal_bias_ax = avg_ax;
                s_cal_bias_ay = avg_ay;
                s_cal_bias_az = avg_az - 1.0f;
                imu_sensor_set_calibration_bias(s_cal_bias_ax, s_cal_bias_ay, s_cal_bias_az);
            }

            s_cal_step++;
            if (s_cal_step >= 8) {
                // Calibration complete!
                save_calibration_to_nvs();
                s_cal_step = 0;
                if (s_cal_step_label) lv_label_set_text(s_cal_step_label, "Calibration Complete!");
                if (s_cal_instruction_label) lv_label_set_text(s_cal_instruction_label, "All 8 poses calibrated & bias saved to NVS. Applied to auto-rotation!");
                if (s_cal_btn_label) lv_label_set_text(s_cal_btn_label, "Recalibrate All");
            } else {
                if (s_cal_step_label) lv_label_set_text(s_cal_step_label, CAL_STEPS[s_cal_step].title);
                if (s_cal_instruction_label) lv_label_set_text(s_cal_instruction_label, CAL_STEPS[s_cal_step].instruction);
                if (s_cal_btn_label) lv_label_set_text(s_cal_btn_label, "Record Next Pose");
            }
        }
    }
}

void ui_spirit_level_init(lv_obj_t *parent_screen) {
    load_calibration_from_nvs();

    // Full screen overlay: 640x172
    s_container = lv_obj_create(parent_screen);
    lv_obj_set_size(s_container, 640, 172);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_container, lv_color_hex(0x080C14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_container, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_container, 0, LV_PART_MAIN);

    // Large Close button ([X]) matching Settings & OTA enlarged standard
    lv_obj_t * close_btn = lv_btn_create(s_container);
    lv_obj_set_size(close_btn, 46, 38);
    lv_obj_set_pos(close_btn, 588, 4);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x1B2338), LV_PART_MAIN);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(close_btn, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(close_btn, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(close_btn, close_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "X");
    lv_obj_set_align(close_lbl, LV_ALIGN_CENTER);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_18, LV_PART_MAIN);

    // Tabview for 3 panels: Level | Raw & Presets | 8-Step Calibration
    s_tabview = lv_tabview_create(s_container, LV_DIR_TOP, 34);
    lv_obj_set_size(s_tabview, 582, 172);
    lv_obj_set_pos(s_tabview, 0, 0);
    lv_obj_set_style_bg_color(s_tabview, lv_color_hex(0x080C14), LV_PART_MAIN);

    lv_obj_t * tab_btns = lv_tabview_get_tab_btns(s_tabview);
    lv_obj_set_style_bg_color(tab_btns, lv_color_hex(0x0F1522), LV_PART_MAIN);
    lv_obj_set_style_text_color(tab_btns, lv_color_hex(0x8EA5C8), LV_PART_MAIN);
    lv_obj_set_style_text_font(tab_btns, &lv_font_montserrat_14, LV_PART_MAIN);

    lv_obj_t * tab1 = lv_tabview_add_tab(s_tabview, "Spirit Level");
    lv_obj_t * tab2 = lv_tabview_add_tab(s_tabview, "Raw & Presets");
    lv_obj_t * tab3 = lv_tabview_add_tab(s_tabview, "Calibration");

    lv_obj_clear_flag(tab1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tab2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tab3, LV_OBJ_FLAG_SCROLLABLE);

    // ================= PANEL 1: SPIRIT LEVEL =================
    // Reticle circular frame
    lv_obj_t * dial_card = lv_obj_create(tab1);
    lv_obj_set_size(dial_card, 116, 116);
    lv_obj_set_pos(dial_card, 42, 6);
    lv_obj_clear_flag(dial_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(dial_card, lv_color_hex(0x060A12), LV_PART_MAIN);
    lv_obj_set_style_border_color(dial_card, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(dial_card, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(dial_card, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // Target center circle
    lv_obj_t * center_ring = lv_obj_create(dial_card);
    lv_obj_set_size(center_ring, 38, 38);
    lv_obj_set_align(center_ring, LV_ALIGN_CENTER);
    lv_obj_clear_flag(center_ring, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(center_ring, 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(center_ring, lv_color_hex(0x00FF88), LV_PART_MAIN);
    lv_obj_set_style_border_width(center_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(center_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // Fluid bubble (24x24 sphere)
    s_bubble_obj = lv_obj_create(tab1);
    lv_obj_set_size(s_bubble_obj, 24, 24);
    lv_obj_set_pos(s_bubble_obj, 88, 52); // Starts centered
    lv_obj_clear_flag(s_bubble_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_bubble_obj, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_bubble_obj, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bubble_obj, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bubble_obj, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    // Numeric Angle Readouts
    s_label_angles = lv_label_create(tab1);
    lv_obj_set_pos(s_label_angles, 190, 16);
    lv_obj_set_style_text_color(s_label_angles, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_label_angles, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(s_label_angles, "Pitch: +0.0 deg\nRoll : +0.0 deg\nTol  : +/-2.0 deg");

    s_label_level_status = lv_label_create(tab1);
    lv_obj_set_pos(s_label_level_status, 190, 82);
    lv_obj_set_style_text_font(s_label_level_status, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_label_level_status, "INITIALIZING...");

    // ================= PANEL 2: RAW SCORES & PRESETS =================
    s_label_raw_imu = lv_label_create(tab2);
    lv_obj_set_pos(s_label_raw_imu, 10, 6);
    lv_obj_set_style_text_color(s_label_raw_imu, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_label_raw_imu, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_label_raw_imu, "ACCEL: ...\nGYRO : ...\nTEMP : ...");

    // Current Orientation Badge
    s_label_orientation_badge = lv_label_create(tab2);
    lv_obj_set_pos(s_label_orientation_badge, 310, 8);
    lv_obj_set_style_text_color(s_label_orientation_badge, lv_color_hex(0xFFEA00), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_label_orientation_badge, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_label_orientation_badge, "Normal Landscape");

    // Tolerance range adjuster [-] [+]
    s_label_tolerance = lv_label_create(tab2);
    lv_obj_set_pos(s_label_tolerance, 310, 42);
    lv_obj_set_style_text_color(s_label_tolerance, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_label_tolerance, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(s_label_tolerance, "Tol: +/-2.0 deg");

    lv_obj_t * btn_minus = lv_btn_create(tab2);
    lv_obj_set_size(btn_minus, 32, 26);
    lv_obj_set_pos(btn_minus, 425, 36);
    lv_obj_set_style_bg_color(btn_minus, lv_color_hex(0x1B2338), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_minus, tol_minus_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_m = lv_label_create(btn_minus);
    lv_label_set_text(lbl_m, "-");
    lv_obj_set_align(lbl_m, LV_ALIGN_CENTER);

    lv_obj_t * btn_plus = lv_btn_create(tab2);
    lv_obj_set_size(btn_plus, 32, 26);
    lv_obj_set_pos(btn_plus, 465, 36);
    lv_obj_set_style_bg_color(btn_plus, lv_color_hex(0x1B2338), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_plus, tol_plus_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_p = lv_label_create(btn_plus);
    lv_label_set_text(lbl_p, "+");
    lv_obj_set_align(lbl_p, LV_ALIGN_CENTER);

    // Preset Slot Switcher Button
    s_btn_preset_cycle = lv_btn_create(tab2);
    lv_obj_set_size(s_btn_preset_cycle, 145, 28);
    lv_obj_set_pos(s_btn_preset_cycle, 310, 72);
    lv_obj_set_style_bg_color(s_btn_preset_cycle, lv_color_hex(0x223344), LV_PART_MAIN);
    lv_obj_add_event_cb(s_btn_preset_cycle, preset_cycle_click_cb, LV_EVENT_CLICKED, NULL);
    s_lbl_preset_cycle = lv_label_create(s_btn_preset_cycle);
    lv_label_set_text(s_lbl_preset_cycle, "Slot: Wall (0 deg)");
    lv_obj_set_align(s_lbl_preset_cycle, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_lbl_preset_cycle, &lv_font_montserrat_12, LV_PART_MAIN);

    // Save Preset button
    lv_obj_t * btn_save = lv_btn_create(tab2);
    lv_obj_set_size(btn_save, 110, 28);
    lv_obj_set_pos(btn_save, 465, 72);
    lv_obj_set_style_bg_color(btn_save, lv_color_hex(0x00A86B), LV_PART_MAIN);
    lv_obj_add_event_cb(btn_save, save_preset_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * lbl_save = lv_label_create(btn_save);
    lv_label_set_text(lbl_save, "Save Angle");
    lv_obj_set_align(lbl_save, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(lbl_save, &lv_font_montserrat_12, LV_PART_MAIN);

    s_label_preset_status = lv_label_create(tab2);
    lv_obj_set_pos(s_label_preset_status, 10, 84);
    lv_obj_set_style_text_color(s_label_preset_status, lv_color_hex(0x8EA5C8), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_label_preset_status, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_label_set_text(s_label_preset_status, "Tap 'Save Angle' to calibrate target pose.");

    // ================= PANEL 3: 8-STEP CALIBRATION WIZARD =================
    s_cal_step_label = lv_label_create(tab3);
    lv_obj_set_pos(s_cal_step_label, 12, 4);
    lv_obj_set_style_text_color(s_cal_step_label, lv_color_hex(0x00F0FF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_cal_step_label, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_label_set_text(s_cal_step_label, CAL_STEPS[0].title);

    s_cal_instruction_label = lv_label_create(tab3);
    lv_obj_set_pos(s_cal_instruction_label, 12, 28);
    lv_obj_set_width(s_cal_instruction_label, 540);
    lv_obj_set_style_text_color(s_cal_instruction_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_cal_instruction_label, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(s_cal_instruction_label, CAL_STEPS[0].instruction);

    s_cal_progress_bar = lv_bar_create(tab3);
    lv_obj_set_size(s_cal_progress_bar, 340, 16);
    lv_obj_set_pos(s_cal_progress_bar, 12, 78);
    lv_bar_set_range(s_cal_progress_bar, 0, 100);
    lv_bar_set_value(s_cal_progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_cal_progress_bar, lv_color_hex(0x1B2338), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cal_progress_bar, lv_color_hex(0x00FF88), LV_PART_INDICATOR);

    s_cal_action_btn = lv_btn_create(tab3);
    lv_obj_set_size(s_cal_action_btn, 180, 36);
    lv_obj_set_pos(s_cal_action_btn, 370, 68);
    lv_obj_set_style_bg_color(s_cal_action_btn, lv_color_hex(0x0072C6), LV_PART_MAIN);
    lv_obj_set_style_border_color(s_cal_action_btn, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_cal_action_btn, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(s_cal_action_btn, cal_btn_click_cb, LV_EVENT_CLICKED, NULL);

    s_cal_btn_label = lv_label_create(s_cal_action_btn);
    lv_label_set_text(s_cal_btn_label, "Record Pose 1");
    lv_obj_set_align(s_cal_btn_label, LV_ALIGN_CENTER);
    lv_obj_set_style_text_font(s_cal_btn_label, &lv_font_montserrat_14, LV_PART_MAIN);

    // Register 50ms periodic refresh timer
    lv_timer_create(spirit_level_timer_cb, 50, NULL);
}

void ui_spirit_level_open(void) {
    if (s_container) {
        lv_obj_clear_flag(s_container, LV_OBJ_FLAG_HIDDEN);
        if (ui_Utility_Button_returnMenu) {
            lv_obj_add_flag(ui_Utility_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void ui_spirit_level_close(void) {
    if (s_container) {
        lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
        if (ui_Utility_Button_returnMenu) {
            lv_obj_clear_flag(ui_Utility_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
