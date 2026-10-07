#include "alarm_ui.h"
#include "alarm.h"
#include "../ui/ui.h"

lv_obj_t *ui_Info_Panel_Alarm = NULL;
static lv_obj_t *ui_Alarm_Switch_Enable = NULL;
static lv_obj_t *ui_Alarm_Label_State = NULL;
static lv_obj_t *ui_Alarm_Label_Next = NULL;

// 4 Digit Labels
static lv_obj_t *ui_Alarm_Label_H1 = NULL;
static lv_obj_t *ui_Alarm_Label_H2 = NULL;
static lv_obj_t *ui_Alarm_Label_M1 = NULL;
static lv_obj_t *ui_Alarm_Label_M2 = NULL;

static lv_obj_t *ui_Alarm_Button_Test = NULL;
static lv_obj_t *ui_Alarm_Label_Test = NULL;
static lv_obj_t *ui_Alarm_Button_Sound = NULL;
static lv_obj_t *ui_Alarm_Label_Sound = NULL;

static bool s_sound_mode_stream = false;

static void update_time_digits(uint8_t h, uint8_t m) {
    if (ui_Alarm_Label_H1) lv_label_set_text_fmt(ui_Alarm_Label_H1, "%d", h / 10);
    if (ui_Alarm_Label_H2) lv_label_set_text_fmt(ui_Alarm_Label_H2, "%d", h % 10);
    if (ui_Alarm_Label_M1) lv_label_set_text_fmt(ui_Alarm_Label_M1, "%d", m / 10);
    if (ui_Alarm_Label_M2) lv_label_set_text_fmt(ui_Alarm_Label_M2, "%d", m % 10);
}

void alarm_ui_refresh(void) {
    if (!ui_Info_Panel_Alarm) return;

    bool en = alarm_is_enabled();
    uint8_t h = 7, m = 0;
    alarm_get_time(&h, &m);

    if (ui_Alarm_Switch_Enable) {
        if (en) lv_obj_add_state(ui_Alarm_Switch_Enable, LV_STATE_CHECKED);
        else lv_obj_clear_state(ui_Alarm_Switch_Enable, LV_STATE_CHECKED);
    }

    if (ui_Alarm_Label_State) {
        lv_label_set_text(ui_Alarm_Label_State, en ? "ARMED" : "OFF");
        lv_obj_set_style_text_color(ui_Alarm_Label_State, 
            en ? lv_color_hex(0x00FF66) : lv_color_hex(0x888888), LV_PART_MAIN);
    }

    if (ui_Alarm_Label_Next) {
        if (en) {
            lv_label_set_text_fmt(ui_Alarm_Label_Next, "Next: %02d:%02d", h, m);
        } else {
            lv_label_set_text(ui_Alarm_Label_Next, "Next: --:--");
        }
    }

    update_time_digits(h, m);

    if (ui_Alarm_Label_Test) {
        lv_label_set_text(ui_Alarm_Label_Test, alarm_is_active() ? "DISMISS" : "TEST");
    }

    if (ui_Alarm_Label_Sound) {
        lv_label_set_text(ui_Alarm_Label_Sound, s_sound_mode_stream ? "Stream" : "Tone");
    }
}

static void event_switch_changed(lv_event_t *e) {
    bool en = lv_obj_has_state(ui_Alarm_Switch_Enable, LV_STATE_CHECKED);
    alarm_set_enabled(en);
    alarm_ui_refresh();
}

static void event_button_test(lv_event_t *e) {
    if (alarm_is_active()) {
        alarm_stop();
    } else {
        alarm_trigger();
    }
    alarm_ui_refresh();
}

static void event_sound_toggle(lv_event_t *e) {
    s_sound_mode_stream = !s_sound_mode_stream;
    alarm_ui_refresh();
}

// Tap Handlers for 4 Columns (H1, H2, M1, M2)
static void adjust_h1(int delta) {
    uint8_t h = 0, m = 0;
    alarm_get_time(&h, &m);
    int tens = h / 10;
    int units = h % 10;
    tens = (tens + delta + 3) % 3; // 0, 1, 2
    if (tens == 2 && units > 3) units = 3;
    h = (uint8_t)(tens * 10 + units);
    alarm_set_time(h, m);
    alarm_ui_refresh();
}

static void adjust_h2(int delta) {
    uint8_t h = 0, m = 0;
    alarm_get_time(&h, &m);
    int tens = h / 10;
    int units = h % 10;
    int max_u = (tens == 2) ? 3 : 9;
    units += delta;
    if (units > max_u) units = 0;
    else if (units < 0) units = max_u;
    h = (uint8_t)(tens * 10 + units);
    alarm_set_time(h, m);
    alarm_ui_refresh();
}

static void adjust_m1(int delta) {
    uint8_t h = 0, m = 0;
    alarm_get_time(&h, &m);
    int tens = m / 10;
    int units = m % 10;
    tens = (tens + delta + 6) % 6; // 0..5
    m = (uint8_t)(tens * 10 + units);
    alarm_set_time(h, m);
    alarm_ui_refresh();
}

static void adjust_m2(int delta) {
    uint8_t h = 0, m = 0;
    alarm_get_time(&h, &m);
    int tens = m / 10;
    int units = m % 10;
    units = (units + delta + 10) % 10; // 0..9
    m = (uint8_t)(tens * 10 + units);
    alarm_set_time(h, m);
    alarm_ui_refresh();
}

static lv_obj_t* create_arrow_btn(lv_obj_t *parent, const char *symbol, lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 42, 34);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(btn, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x0A2B14), LV_PART_MAIN);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x00FF66), LV_PART_MAIN);
    lv_obj_set_style_border_width(btn, 1, LV_PART_MAIN);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_obj_set_align(lbl, LV_ALIGN_CENTER);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00FF66), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    return btn;
}

static lv_obj_t* create_digit_box(lv_obj_t *parent) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_set_size(box, 42, 54);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(box, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x03170A), LV_PART_MAIN);
    lv_obj_set_style_border_color(box, lv_color_hex(0x00DD55), LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(box);
    lv_obj_set_align(lbl, LV_ALIGN_CENTER);
    lv_label_set_text(lbl, "0");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00FF66), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_32, LV_PART_MAIN);
    return lbl;
}

void alarm_ui_init(lv_obj_t *parent) {
    if (ui_Info_Panel_Alarm) return;

    // 1. Container Panel (640 x 172 landscape banner)
    ui_Info_Panel_Alarm = lv_obj_create(parent);
    lv_obj_set_width(ui_Info_Panel_Alarm, 640);
    lv_obj_set_height(ui_Info_Panel_Alarm, 172);
    lv_obj_set_align(ui_Info_Panel_Alarm, LV_ALIGN_LEFT_MID);
    lv_obj_add_flag(ui_Info_Panel_Alarm, LV_OBJ_FLAG_HIDDEN); // Hidden by default (Page 2)
    lv_obj_clear_flag(ui_Info_Panel_Alarm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(ui_Info_Panel_Alarm, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_Info_Panel_Alarm, lv_color_hex(0x020B05), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui_Info_Panel_Alarm, 255, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_Info_Panel_Alarm, 0, LV_PART_MAIN);

    // --- LEFT COLUMN: Header, Toggle Switch, Status ---
    lv_obj_t *lbl_title = lv_label_create(ui_Info_Panel_Alarm);
    lv_obj_set_align(lbl_title, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(lbl_title, 15, 12);
    lv_label_set_text(lbl_title, "RETRO ALARM");
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0x00FF66), LV_PART_MAIN);
    lv_obj_set_style_text_font(lbl_title, &lv_font_montserrat_14, LV_PART_MAIN);

    ui_Alarm_Switch_Enable = lv_switch_create(ui_Info_Panel_Alarm);
    lv_obj_set_align(ui_Alarm_Switch_Enable, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ui_Alarm_Switch_Enable, 15, 42);
    lv_obj_set_size(ui_Alarm_Switch_Enable, 56, 28);
    lv_obj_set_style_bg_color(ui_Alarm_Switch_Enable, lv_color_hex(0x00CC44), (lv_style_selector_t)(static_cast<uint32_t>(LV_PART_INDICATOR) | static_cast<uint32_t>(LV_STATE_CHECKED)));
    lv_obj_add_event_cb(ui_Alarm_Switch_Enable, event_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);

    ui_Alarm_Label_State = lv_label_create(ui_Info_Panel_Alarm);
    lv_obj_set_align(ui_Alarm_Label_State, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ui_Alarm_Label_State, 80, 47);
    lv_label_set_text(ui_Alarm_Label_State, "OFF");
    lv_obj_set_style_text_color(ui_Alarm_Label_State, lv_color_hex(0x888888), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_Alarm_Label_State, &lv_font_montserrat_14, LV_PART_MAIN);

    ui_Alarm_Label_Next = lv_label_create(ui_Info_Panel_Alarm);
    lv_obj_set_align(ui_Alarm_Label_Next, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(ui_Alarm_Label_Next, 15, 82);
    lv_label_set_text(ui_Alarm_Label_Next, "Next: --:--");
    lv_obj_set_style_text_color(ui_Alarm_Label_Next, lv_color_hex(0x00DD55), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_Alarm_Label_Next, &lv_font_montserrat_12, LV_PART_MAIN);

    // --- CENTER GRID: 3 Rows x 4 Columns (No Scrolling) ---
    // Positions: Column X coordinates
    const int col_x[4] = {165, 215, 280, 330};
    const int row_y[3] = {12, 54, 116}; // Up Arrow, Digit, Down Arrow

    // Row 1: Up Buttons
    create_arrow_btn(ui_Info_Panel_Alarm, LV_SYMBOL_UP, [](lv_event_t *e){ adjust_h1(1); }, NULL);
    lv_obj_set_pos(lv_obj_get_child(ui_Info_Panel_Alarm, lv_obj_get_child_cnt(ui_Info_Panel_Alarm)-1), col_x[0], row_y[0]);

    create_arrow_btn(ui_Info_Panel_Alarm, LV_SYMBOL_UP, [](lv_event_t *e){ adjust_h2(1); }, NULL);
    lv_obj_set_pos(lv_obj_get_child(ui_Info_Panel_Alarm, lv_obj_get_child_cnt(ui_Info_Panel_Alarm)-1), col_x[1], row_y[0]);

    // Colon Separator
    lv_obj_t *colon_lbl = lv_label_create(ui_Info_Panel_Alarm);
    lv_obj_set_pos(colon_lbl, 260, 60);
    lv_label_set_text(colon_lbl, ":");
    lv_obj_set_style_text_color(colon_lbl, lv_color_hex(0x00FF66), LV_PART_MAIN);
    lv_obj_set_style_text_font(colon_lbl, &lv_font_montserrat_32, LV_PART_MAIN);

    create_arrow_btn(ui_Info_Panel_Alarm, LV_SYMBOL_UP, [](lv_event_t *e){ adjust_m1(1); }, NULL);
    lv_obj_set_pos(lv_obj_get_child(ui_Info_Panel_Alarm, lv_obj_get_child_cnt(ui_Info_Panel_Alarm)-1), col_x[2], row_y[0]);

    create_arrow_btn(ui_Info_Panel_Alarm, LV_SYMBOL_UP, [](lv_event_t *e){ adjust_m2(1); }, NULL);
    lv_obj_set_pos(lv_obj_get_child(ui_Info_Panel_Alarm, lv_obj_get_child_cnt(ui_Info_Panel_Alarm)-1), col_x[3], row_y[0]);

    // Row 2: 4 Large Digits
    ui_Alarm_Label_H1 = create_digit_box(ui_Info_Panel_Alarm);
    lv_obj_set_pos(lv_obj_get_parent(ui_Alarm_Label_H1), col_x[0], row_y[1]);

    ui_Alarm_Label_H2 = create_digit_box(ui_Info_Panel_Alarm);
    lv_obj_set_pos(lv_obj_get_parent(ui_Alarm_Label_H2), col_x[1], row_y[1]);

    ui_Alarm_Label_M1 = create_digit_box(ui_Info_Panel_Alarm);
    lv_obj_set_pos(lv_obj_get_parent(ui_Alarm_Label_M1), col_x[2], row_y[1]);

    ui_Alarm_Label_M2 = create_digit_box(ui_Info_Panel_Alarm);
    lv_obj_set_pos(lv_obj_get_parent(ui_Alarm_Label_M2), col_x[3], row_y[1]);

    // Row 3: Down Buttons
    create_arrow_btn(ui_Info_Panel_Alarm, LV_SYMBOL_DOWN, [](lv_event_t *e){ adjust_h1(-1); }, NULL);
    lv_obj_set_pos(lv_obj_get_child(ui_Info_Panel_Alarm, lv_obj_get_child_cnt(ui_Info_Panel_Alarm)-1), col_x[0], row_y[2]);

    create_arrow_btn(ui_Info_Panel_Alarm, LV_SYMBOL_DOWN, [](lv_event_t *e){ adjust_h2(-1); }, NULL);
    lv_obj_set_pos(lv_obj_get_child(ui_Info_Panel_Alarm, lv_obj_get_child_cnt(ui_Info_Panel_Alarm)-1), col_x[1], row_y[2]);

    create_arrow_btn(ui_Info_Panel_Alarm, LV_SYMBOL_DOWN, [](lv_event_t *e){ adjust_m1(-1); }, NULL);
    lv_obj_set_pos(lv_obj_get_child(ui_Info_Panel_Alarm, lv_obj_get_child_cnt(ui_Info_Panel_Alarm)-1), col_x[2], row_y[2]);

    create_arrow_btn(ui_Info_Panel_Alarm, LV_SYMBOL_DOWN, [](lv_event_t *e){ adjust_m2(-1); }, NULL);
    lv_obj_set_pos(lv_obj_get_child(ui_Info_Panel_Alarm, lv_obj_get_child_cnt(ui_Info_Panel_Alarm)-1), col_x[3], row_y[2]);

    // --- RIGHT COLUMN: Test Trigger Button & Sound Mode ---
    ui_Alarm_Button_Test = lv_btn_create(ui_Info_Panel_Alarm);
    lv_obj_set_align(ui_Alarm_Button_Test, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(ui_Alarm_Button_Test, -25, 20);
    lv_obj_set_size(ui_Alarm_Button_Test, 110, 48);
    lv_obj_set_style_bg_color(ui_Alarm_Button_Test, lv_color_hex(0x00AA44), LV_PART_MAIN);
    lv_obj_set_style_radius(ui_Alarm_Button_Test, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(ui_Alarm_Button_Test, event_button_test, LV_EVENT_CLICKED, NULL);

    ui_Alarm_Label_Test = lv_label_create(ui_Alarm_Button_Test);
    lv_obj_set_align(ui_Alarm_Label_Test, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Alarm_Label_Test, "TEST");
    lv_obj_set_style_text_color(ui_Alarm_Label_Test, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_Alarm_Label_Test, &lv_font_montserrat_14, LV_PART_MAIN);

    ui_Alarm_Button_Sound = lv_btn_create(ui_Info_Panel_Alarm);
    lv_obj_set_align(ui_Alarm_Button_Sound, LV_ALIGN_TOP_RIGHT);
    lv_obj_set_pos(ui_Alarm_Button_Sound, -25, 84);
    lv_obj_set_size(ui_Alarm_Button_Sound, 110, 38);
    lv_obj_set_style_bg_color(ui_Alarm_Button_Sound, lv_color_hex(0x0A3A1A), LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_Alarm_Button_Sound, lv_color_hex(0x00FF66), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_Alarm_Button_Sound, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(ui_Alarm_Button_Sound, 8, LV_PART_MAIN);
    lv_obj_add_event_cb(ui_Alarm_Button_Sound, event_sound_toggle, LV_EVENT_CLICKED, NULL);

    ui_Alarm_Label_Sound = lv_label_create(ui_Alarm_Button_Sound);
    lv_obj_set_align(ui_Alarm_Label_Sound, LV_ALIGN_CENTER);
    lv_label_set_text(ui_Alarm_Label_Sound, "Tone");
    lv_obj_set_style_text_color(ui_Alarm_Label_Sound, lv_color_hex(0x00FF66), LV_PART_MAIN);
    lv_obj_set_style_text_font(ui_Alarm_Label_Sound, &lv_font_montserrat_12, LV_PART_MAIN);

    alarm_ui_refresh();
}

void alarm_ui_show(void) {
    if (ui_Info_Panel_Alarm) {
        lv_obj_clear_flag(ui_Info_Panel_Alarm, LV_OBJ_FLAG_HIDDEN);
        alarm_ui_refresh();
    }
}
