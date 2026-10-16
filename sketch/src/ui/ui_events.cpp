#include <stdint.h>
void updateInfoPanel(uint8_t pages);
#include "battery/battery.h"
#include "ui.h"
#include <Arduino.h>
#include <Preferences.h>
#include <stdlib.h> // สำหรับ atoi
Preferences pref;
#include "network/network.h"

// #include "record.h"
#include "file/file.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "pcf85063/pcf85063.h"
#include "task_msg/task_msg.h"
#include "updater/updater.h"
#include "weather/weather.h"
#include <LittleFS.h>
#include "lvgl_port/lvgl_port.h"
#include "screens/ui_Screen_Consent.h"
#include "../secrets/secrets_manager.h"
#include "power/power_profile.h"
#include "imu/imu_service.h"
#include "tca9554/tca9554.h"

#include "ESP32-audioI2S-master/Audio.h"
#include "es7210/es7210.h"
#include "lan_stream/lan_stream.h"
extern Audio audio;
uint8_t audio_volume = 10;
extern ES7210 mic;
int lan_track_idx = 0;
extern void start_ai_voice_recording();
extern void stop_ai_voice_recording_and_process();
extern volatile bool ai_recording_active;
extern volatile bool ai_upload_in_progress;
uint8_t ai_rec_duration_sec = 6;
static lv_timer_t *menu_audio_anim_timer = NULL;
static void menu_audio_anim_cb(lv_timer_t *t);

PCF85063 rtc;

uint8_t timeout_index = 0; // backlight timeout index
bool paused = false;
uint8_t wallpaperIndex = 0;
bool mute = false;

// Music Player source & index tracking
static uint8_t s_active_player_source = 0; // 0 = LOCAL (SD Card), 1 = LAN
static int s_last_local_track_idx = 0;
static int s_last_lan_track_idx = 0;
static uint8_t s_settings_music_mode = 0;  // 0 = LOCAL, 1 = LAN

uint8_t get_active_player_source(void) { return s_active_player_source; }
void set_active_player_source(uint8_t src) { s_active_player_source = src; }
void set_last_lan_track_idx(int idx) { s_last_lan_track_idx = idx; }
void set_last_local_track_idx(int idx) { s_last_local_track_idx = idx; }

#define WIFI_CHECK_INTERVAL 5000 //wifi connection checking interval 5 second.
#define MAX_WALLPAPER 7 // Number of procedural vector themes

// Procedural vector themes (pure LVGL styling, zero flash footprint)
const char *wallpaper_menu = "Pure Dark\nCyberpunk\nDeep Ocean\nEmerald\nSunset\nCarbon\nMidnight Blue";

struct VectorTheme {
    uint32_t bg_top;
    uint32_t bg_bottom;
};

static const VectorTheme vector_themes[MAX_WALLPAPER] = {
    {0x000000, 0x000000}, // Pure Dark (OLED black)
    {0x1A0933, 0x070114}, // Cyberpunk (Violet to Dark Void)
    {0x041C32, 0x040D1A}, // Deep Ocean (Navy to Abyssal)
    {0x042B1A, 0x02140C}, // Emerald Matrix (Deep forest to shadow)
    {0x331204, 0x140601}, // Sunset Amber (Ember glow)
    {0x181A1F, 0x0C0D10}, // Carbon Slate (Brushed graphite)
    {0x0B192C, 0x1E3E62}  // Midnight Blue (Dual tone blue)
};

// --- Clock Face Breathing Backlight & Power State ---
enum BreathState { BREATH_FULL_BLACK, BREATH_FADE_IN, BREATH_HOLD_HIGH, BREATH_FADE_OUT };
static BreathState breath_state   = BREATH_HOLD_HIGH;
static uint32_t   breath_timer_ms = 0;
extern lv_obj_t  *last_active_screen;
extern "C" void bsp_set_backlight_power(bool enable);
static uint32_t s_clock_interaction_ms = 0;
uint32_t g_last_user_interaction_ms = 0;
bool g_boot_ntp_synced = false;
extern uint8_t infoPageIndex;

extern "C" void record_user_interaction(void) {
  g_last_user_interaction_ms = millis();
  if (getCpuFrequencyMhz() != 240) {
    setCpuFrequencyMhz(240);
    log_i("[POWER] User interaction -> CPU restored to 240 MHz");
  }
  if (wifiEnable && WiFi.getMode() == WIFI_OFF) {
    WiFi.mode(WIFI_STA);
    wifi_need_connect = true;
    if (wifi_check_timer) lv_timer_resume(wifi_check_timer);
    wifiConnect();
    log_i("[POWER] User interaction -> Wi-Fi & Web Remote activated for 2 minutes");
  }
}

extern "C" bool is_on_nixie_clock(void) {
  return (lv_scr_act() == ui_Screen_Info && infoPageIndex == 0);
}

extern "C" bool is_display_deep_black(void) {
  return BL_OFF || (clock_face_active && breath_state == BREATH_FULL_BLACK);
}

void enter_clock_breathing(void) {
  if (clock_face_active) return;
  clock_face_active = 1;
  s_clock_interaction_ms = 0;
  BL_OFF = false;
  bsp_set_backlight_power(true);
  breath_state = BREATH_HOLD_HIGH;
  breath_timer_ms = millis();

  uint8_t max_bl = (backlight_state == 0) ? 105 : ((backlight_state == 1) ? 120 : 150);
  log_i("[BREATH] Clock breathing active (Floor: 91, Max: %u, Inactivity-Tiered)", (unsigned)max_bl);
}

void exit_clock_breathing(void) {
  record_user_interaction();
  s_clock_interaction_ms = millis();
  if (!clock_face_active) return;
  clock_face_active = 0;

  bsp_set_backlight_power(true);
  bsp_set_audio_amp_power(true);
  if (imu_sensor_is_sleeping()) {
    imu_sensor_wake();
  }
  screenPowerOn(); // Restores AP3032 boost rail and sets PWM duty according to backlight_state
}

extern "C" void on_clock_interaction_wake(void) {
  record_user_interaction();
  s_clock_interaction_ms = millis();
  SCREEN_OFF_TIMER = millis();
  BL_OFF = false;
  bsp_set_backlight_power(true);
  bsp_set_audio_amp_power(true);
  if (imu_sensor_is_sleeping()) {
    imu_sensor_wake();
  }
  screenPowerOn();

  if (clock_face_active) {
    clock_face_active = 0;
    log_i("[BREATH] User interaction/rotation -> breathing paused, steady brightness restored");
  } else {
    log_i("[BREATH] User interaction -> idle timeout reset (15s)");
  }
}

void on_clock_touch_or_button(void) {
  on_clock_interaction_wake();
}

extern "C" void on_clock_boot_button_pressed(void) {
  if (!clock_face_active && (millis() - s_clock_interaction_ms < 10000)) return;
  exit_clock_breathing(); // Restores backlight to settings brightness, turns boost rail on, restores WiFi
  SCREEN_OFF_TIMER = millis(); // Reset idle timeout
  BL_OFF = false;

  if (last_active_screen && last_active_screen != ui_Screen_Info) {
    log_i("[BOOT BTN] Returning to previous screen before clock");
    lv_scr_load_anim(last_active_screen, LV_SCR_LOAD_ANIM_FADE_ON, 300, 0, false);
  } else {
    log_i("[BOOT BTN] Remaining on Clock with active settings brightness");
    on_clock_touch_or_button();
  }
}

void update_clock_breathing(void) {
  // Never start breathing on first boot while PIN / Consent setup is undecided
  if (!secrets_has_decided_consent()) {
    return;
  }

  uint32_t now = millis();

  // If we are not in clock breathing (active UI or audio playing), sample battery every 10s so accumulator has data
  static uint32_t s_last_active_sample_ms = 0;
  if (!clock_face_active) {
    if (now - s_last_active_sample_ms >= 10000UL) {
      uint16_t raw_adc = (uint16_t)analogReadMilliVolts(BATTERY_ADC_PIN);
      battery_accumulate_sample(raw_adc, true /* screen on */);
      s_last_active_sample_ms = now;
    }

    if (lv_scr_act() == ui_Screen_Info && infoPageIndex == 0) {
      if (SCREEN_OFF_DELAY == 0) return; // If screen off delay is Never (0), never re-engage breathing!
      if (s_clock_interaction_ms == 0) {
        s_clock_interaction_ms = now;
      }
      uint32_t idle_limit = SCREEN_OFF_DELAY;
      if (now - s_clock_interaction_ms >= idle_limit) {
        log_i("[BREATH] Clock idle timeout reached (%u ms) -> re-engaging breathing cycle", (unsigned)idle_limit);
        enter_clock_breathing();
      }
    }
    return;
  }

  const power_profile_t *pwr = power_profile_get();
  uint8_t min_bl = pwr->bl_start;
  uint8_t max_bl = pwr->bl_end;
  uint8_t bl_black = pwr->bl_black;
  uint16_t ramp_up = pwr->ramp_up_ms;
  uint16_t hold_high = pwr->hold_high_ms;
  uint16_t ramp_down = pwr->ramp_down_ms;
  uint32_t full_black_ms = pwr->hold_dark_ms;

  // Inactivity tiering: after 30 mins of no interaction, extend full black
  if (pwr->inactivity_tier && g_last_user_interaction_ms > 0 && (now - g_last_user_interaction_ms >= 30 * 60 * 1000UL)) {
    full_black_ms *= 2;
  }

  static bool s_sampled_high = false;
  static bool s_sampled_dark = false;

  uint32_t elapsed = millis() - breath_timer_ms;

  switch (breath_state) {
    case BREATH_FULL_BLACK:
      if (bl_black == 0) {
        bsp_set_backlight_power(false); // Cut AP3032 boost rail while dark!
        setUpduty(255); // 0% PWM duty -> 100% full black
      } else {
        bsp_set_backlight_power(true);
        setUpduty(255 - bl_black);
      }

      // Dark Service Gating
      if (pwr->gate_sensors && !imu_sensor_is_sleeping()) {
        imu_sensor_sleep();
      }
      if (pwr->gate_audio && !audio.isRunning() && bsp_get_audio_amp_power()) {
        bsp_set_audio_amp_power(false);
      }

      // Point 2: 2.0s after screen turned dark (stable Vrest)
      if (elapsed >= 2000 && !s_sampled_dark) {
        uint16_t raw_adc = (uint16_t)analogReadMilliVolts(BATTERY_ADC_PIN);
        battery_accumulate_sample(raw_adc, false /* screen off */);
        s_sampled_dark = true;
      }

      if (elapsed >= full_black_ms) {
        bsp_set_backlight_power(true); // Re-enable boost rail!
        if (pwr->gate_sensors && imu_sensor_is_sleeping()) {
          imu_sensor_wake();
        }
        if (pwr->gate_audio && !bsp_get_audio_amp_power()) {
          bsp_set_audio_amp_power(true);
        }
        breath_state    = BREATH_FADE_IN;
        breath_timer_ms = millis();
        s_sampled_dark  = false;
        log_i("[BREATH] State -> FADE_IN | From: %u -> To: %u (%ums)", min_bl, max_bl, ramp_up);
      }
      break;

    case BREATH_FADE_IN:
      bsp_set_backlight_power(true);
      if (elapsed < ramp_up) {
        float progress = (float)elapsed / (float)ramp_up;
        uint8_t current_val = min_bl + (uint8_t)(progress * (float)(max_bl - min_bl));
        setUpduty(255 - current_val);
      } else {
        setUpduty(255 - max_bl);
        breath_state    = BREATH_HOLD_HIGH;
        breath_timer_ms = millis();
        s_sampled_high  = false;
        log_i("[BREATH] State -> HOLD_HIGH | At: %u PWM (%ums)", max_bl, hold_high);
      }
      break;

    case BREATH_HOLD_HIGH:
      bsp_set_backlight_power(true);
      setUpduty(255 - max_bl);

      // Point 1: 2.0s after screen turned on (fade-in + 500ms into hold_high = 2.0s loaded)
      if (elapsed >= 500 && !s_sampled_high) {
        uint16_t raw_adc = (uint16_t)analogReadMilliVolts(BATTERY_ADC_PIN);
        battery_accumulate_sample(raw_adc, true /* screen on */);
        s_sampled_high = true;
      }

      if (elapsed >= hold_high) {
        breath_state    = BREATH_FADE_OUT;
        breath_timer_ms = millis();
        log_i("[BREATH] State -> FADE_OUT | From: %u -> To: %u (%ums)", max_bl, min_bl, ramp_down);
      }
      break;

    case BREATH_FADE_OUT:
      bsp_set_backlight_power(true);
      if (elapsed < ramp_down) {
        float progress = (float)elapsed / (float)ramp_down;
        uint8_t current_val = max_bl - (uint8_t)(progress * (float)(max_bl - min_bl));
        setUpduty(255 - current_val);
      } else {
        setUpduty(255);
        if (bl_black == 0) {
          bsp_set_backlight_power(false); // Cut AP3032 boost rail while dark!
        }
        breath_state    = BREATH_FULL_BLACK;
        breath_timer_ms = millis();
        s_sampled_dark  = false;
        log_i("[BREATH] State -> FULL_BLACK | %u PWM (%u ms dark)", bl_black, (unsigned)full_black_ms);
      }
      break;
  }
}

//wifi monitor call back from timer
extern bool telemetry_is_flushing(void);

void lvgl_wifi_check_cb(lv_timer_t *t) {
  // If telemetry burst upload is in progress, do not disconnect Wi-Fi
  if (telemetry_is_flushing()) return;

  // If connected to USB charger, keep Wi-Fi connected continuously
  if (bsp_is_battery_charging()) {
    if (wifiEnable) {
      if (WiFi.status() != WL_CONNECTED && wifiTaskHandle == NULL && WiFi.getMode() != WIFI_OFF) {
        wifi_need_connect = true;
        lv_timer_pause(wifi_check_timer);
        wifiConnect();
      }
    }
    return;
  }

  // Allow Wi-Fi to connect and sync NTP time upon startup/reset before suppressing it in clock mode
  if (clock_face_active && g_boot_ntp_synced && (!audio.isRunning() || mediaType >= 2)) {
    // If 2 mins of inactivity reached in clock mode, gate Wi-Fi OFF if enabled in power profile
    if (power_profile_get()->gate_wifi && g_last_user_interaction_ms > 0 && (millis() - g_last_user_interaction_ms >= 120000UL)) {
      if (WiFi.getMode() != WIFI_OFF) {
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        log_i("[POWER] 2 min inactivity in clock mode -> Wi-Fi gated OFF");
      }
      return;
    }
  }
  if (wifiEnable) {//if enable
    if (WiFi.status() != WL_CONNECTED && wifiTaskHandle == NULL && WiFi.getMode() != WIFI_OFF) {//wifi disconnected
        wifi_need_connect = true;
        lv_timer_pause(wifi_check_timer);//pause the timer
        wifiConnect();  // create task ONCE
    }
  }
}
//----------- LOGO SCREEN EVENTS ------------
void lv_create_delayed_task(lv_timer_cb_t callback, uint32_t delay_ms, void *user_data) {
  lv_timer_t *timer = lv_timer_create(callback, delay_ms, user_data); // Create a timer
  lv_timer_set_repeat_count(timer, 1); // Set the timer to run only once
}
// initalize lvgl ui
void init_main_menu_task(lv_timer_t *timer) {
  lv_timer_del(timer);
  lv_scr_load(ui_Screen_Info);
  infoPageIndex = 0;
  updateInfoPanel(0);
  ui_consent_modal_show_if_needed();
  static lv_timer_t *breath_timer = NULL;
  if (!breath_timer) {
    breath_timer = lv_timer_create([](lv_timer_t *t){ update_clock_breathing(); }, 30, NULL);
  }           // Activate clock panel & breathing // delete old screen

  // init widgets
  lv_label_set_text(ui_Player_Label_Label1, LV_SYMBOL_VOLUME_MAX);
  lv_label_set_text(ui_Player_Label_Label2, LV_SYMBOL_VOLUME_MID);
  lv_label_set_text(ui_Player_Label_Label4, LV_SYMBOL_NEXT);
  lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_PLAY);
  lv_label_set_text(ui_Player_Label_Label6, LV_SYMBOL_PREV);
  lv_label_set_text(ui_Player_Label_Label17, LV_SYMBOL_LEFT LV_SYMBOL_LEFT);
  lv_label_set_text(ui_Player_Label_Label18, LV_SYMBOL_RIGHT LV_SYMBOL_RIGHT);
  lv_label_set_text(ui_Player_Label_Label7, LV_SYMBOL_UP);
  lv_label_set_text(ui_Player_Label_Battery, LV_SYMBOL_BATTERY_EMPTY);
  lv_label_set_text(ui_Player_Label_WiFi, LV_SYMBOL_WIFI);
  lv_label_set_text(ui_Player_Label_SDcard, LV_SYMBOL_SD_CARD);
  lv_label_set_text(ui_MainMenu_Label_Label12, LV_SYMBOL_SAVE " Save");
  lv_label_set_text(ui_MainMenu_Label_Label13, LV_SYMBOL_WIFI " Scan");
  lv_label_set_text(ui_MainMenu_Label_Label14, LV_SYMBOL_UPLOAD " Load");
  lv_label_set_text(ui_MainMenu_Label_Label20, LV_SYMBOL_UPLOAD " Load");
  lv_label_set_text(ui_Player_Label_Label16, LV_SYMBOL_LOOP);
  uint8_t vol = 255 - (audio_volume * 255 / 21);
  lv_obj_set_style_bg_main_stop(ui_Player_Container_VolumeControl, vol, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_stop(ui_Player_Container_VolumeControl, vol, LV_PART_MAIN);
  lv_dropdown_set_options(ui_MainMenu_Dropdown_Wallpaper, wallpaper_menu); // set custom wallppaer list
  lv_label_set_text(ui_Info_Label_Label19, LV_SYMBOL_LEFT);
  lv_label_set_text(ui_Info_Label_Label22, LV_SYMBOL_RIGHT);
  lv_label_set_text(ui_Info_Label_Label21, LV_SYMBOL_DOWN);
  lv_label_set_text(ui_Utility_Label_Label27, LV_SYMBOL_UP);

  initSDCard(); // init SD Card
  loadStationList(); // load stations.csv from LittleFS
  initSongList(); // load music library from LittleFS

  // load configuration
  pref.begin("config", true); // read only
  backlight_state = pref.getUChar("bl_state", 1); // brightness (Default 1 = Medium)
  lv_dropdown_set_selected(ui_MainMenu_Dropdown_Brightness, backlight_state);
  setBrightness(NULL);
  timeout_index = pref.getUChar("scroffdelay", 3); // screen off delay (Default 3 = 60s)
  switch (timeout_index) {
  case 0: SCREEN_OFF_DELAY = 0; break;
  case 1: SCREEN_OFF_DELAY = 15000; break;
  case 2: SCREEN_OFF_DELAY = 30000; break;
  case 3: SCREEN_OFF_DELAY = 60000; break;
  }
  if (!secrets_has_decided_consent()) {
    backlight_state = 1; // Default to Medium brightness on first boot
    setBrightness(NULL);
    SCREEN_OFF_DELAY = 120000; // 120s timeout
    s_clock_interaction_ms = millis();
  }
  lv_dropdown_set_selected(ui_MainMenu_Dropdown_SleepTimer, timeout_index);
  playMode = pref.getUChar("playing_mode", 0); // playing mode
  switch (playMode) {
  case 0: lv_label_set_text(ui_Player_Label_Label16, LV_SYMBOL_LOOP); break;
  case 1: lv_label_set_text(ui_Player_Label_Label16, LV_SYMBOL_SHUFFLE); break;
  case 2: lv_label_set_text(ui_Player_Label_Label16, "1"); break;
  }
  wallpaperIndex = pref.getUChar("wallpaper", 1); // wallpaper
  lv_dropdown_set_selected(ui_MainMenu_Dropdown_Wallpaper, wallpaperIndex); // backlight timeout
  setWallpaper(NULL);

  // weater and clock
  size_t len = pref.getBytesLength("query_parameter");
  pref.getBytes("query_parameter", query_parameter, len);
  log_d("Region load: Query parameter: %s", query_parameter);
  if (strcmp(query_parameter, "auto:ip") == 0) { // audo:ip
    lv_obj_add_state(ui_MainMenu_Checkbox_AutoIP, LV_STATE_CHECKED);
    lv_obj_add_flag(ui_MainMenu_Textarea_Latitude, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_MainMenu_Textarea_Longitude, LV_OBJ_FLAG_HIDDEN);
  } else { // lat,long

    char qp_copy[64];
    snprintf(qp_copy, sizeof(qp_copy), "%s", query_parameter);
    char *comma = strchr(qp_copy, ',');
    if (comma != NULL) {
      *comma = '\0';
      const char *lat = qp_copy;
      const char *lon = comma + 1;
      lv_obj_clear_state(ui_MainMenu_Checkbox_AutoIP, LV_STATE_CHECKED);
      lv_textarea_set_text(ui_MainMenu_Textarea_Latitude, lat);
      lv_textarea_set_text(ui_MainMenu_Textarea_Longitude, lon);
      lv_obj_clear_flag(ui_MainMenu_Textarea_Latitude, LV_OBJ_FLAG_HIDDEN);
      lv_obj_clear_flag(ui_MainMenu_Textarea_Longitude, LV_OBJ_FLAG_HIDDEN);
    }
  }

  // utc timezone offset
  offset_hour_index = pref.getUChar("offset_hour", 14); // 0 offset
  lv_roller_set_selected(ui_MainMenu_Roller_Hour, offset_hour_index, LV_ANIM_OFF); // set index
  offset_minute_index = pref.getUChar("offset_minute", 3); // 0 offset
  lv_roller_set_selected(ui_MainMenu_Roller_Minute, offset_minute_index, LV_ANIM_OFF); // set index
  temp_unit = pref.getUChar("temp_unit", 0); // degree C
  lv_roller_set_selected(ui_MainMenu_Roller_Unit, temp_unit, LV_ANIM_OFF); // set index

  //wifi enable
  wifiEnable = pref.getBool("wifi_enable", true);

  // ai recording duration
  ai_rec_duration_sec = pref.getUChar("ai_dur", 6);
  if (ai_rec_duration_sec < 4 || ai_rec_duration_sec > 15) ai_rec_duration_sec = 6;

  pref.end();
  SCREEN_OFF_TIMER = millis(); // reset timer

  if (wifiEnable) {
    lv_obj_add_state(ui_MainMenu_Switch_Wifi, LV_STATE_CHECKED);
    wifi_need_connect = true;
    wifiConnect();
    wifi_check_timer = lv_timer_create(lvgl_wifi_check_cb, WIFI_CHECK_INTERVAL, NULL);//wifi status check interval timer

  } else {//init wifi stack once to prevent Audio library crashed
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    log_d("WiFi stack initialized");
  }

  if (!menu_audio_anim_timer) {
    menu_audio_anim_timer = lv_timer_create(menu_audio_anim_cb, 350, NULL);
  }

  // Check and show Community Cloud Consent Modal on cold first boot
  ui_consent_modal_show_if_needed();
}

// ################# App start here after screen initialized ##############################
void appStart(lv_event_t *e) {
  initLittleFS(); // iniit LittleFS
  audioSetVolume(audio_volume);
  audioPlayFS(1, "/audio/on.mp3");
  lv_create_delayed_task(init_main_menu_task, 0, NULL); // show display logo for 2.5 second and start initializing widgets
}

//---------------- Volume control ------------------------------
void volumeup(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  audio_volume++;
  if (audio_volume > 20) audio_volume = 20;
  audioSetVolume(audio_volume);
  log_d("Volume %d", audio_volume);
  uint8_t vol = 255 - (audio_volume * 255 / 21);
  lv_obj_set_style_bg_main_stop(ui_Player_Container_VolumeControl, vol, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_stop(ui_Player_Container_VolumeControl, vol, LV_PART_MAIN);
  lv_anim_del(NULL, (lv_anim_exec_xcb_t)_ui_anim_callback_set_opacity); // remove animation
}

void volumemute(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  if (mute) {
    mute = false;
    audioSetVolume(audio_volume);
    lv_anim_del(NULL, (lv_anim_exec_xcb_t)_ui_anim_callback_set_opacity); // remove animation
    lv_obj_set_style_opa(ui_Player_Image_volumemute, LV_OPA_COVER, LV_PART_MAIN);
    log_d("Unmute volume %d", audio_volume);
  } else {
    mute = true;
    audioSetVolume(0);
    blink_Animation(ui_Player_Image_volumemute, 0); // add animation
  }
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
}

void volumedown(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  audio_volume--;
  if (audio_volume == 0) audio_volume = 0;
  audioSetVolume(audio_volume);
  log_d("Volume %d", audio_volume);
  uint8_t vol = 255 - (audio_volume * 255 / 21);
  lv_obj_set_style_bg_main_stop(ui_Player_Container_VolumeControl, vol, LV_PART_MAIN);
  lv_obj_set_style_bg_grad_stop(ui_Player_Container_VolumeControl, vol, LV_PART_MAIN);
  lv_anim_del(NULL, (lv_anim_exec_xcb_t)_ui_anim_callback_set_opacity); // remove animation
}

//----------- PLAY BACK CONTROL EVENTS ------------
// toggle play mode RPT or RND or LOOP
void togglePlayingMode(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  playMode++;
  if (playMode > 2) playMode = 0;
  log_i("Playing mode set to: %d (%s)", playMode, (playMode == 0) ? "LOOP ALL" : (playMode == 1) ? "SHUFFLE" : "REPEAT 1");
  if (ui_Player_Label_Label16) {
    lv_obj_set_style_text_font(ui_Player_Label_Label16, &lv_font_montserrat_14, LV_PART_MAIN);
    switch (playMode) {
    case 0: lv_label_set_text(ui_Player_Label_Label16, LV_SYMBOL_LOOP); break;
    case 1: lv_label_set_text(ui_Player_Label_Label16, LV_SYMBOL_SHUFFLE); break;
    case 2: lv_label_set_text(ui_Player_Label_Label16, "1"); break;
    }
  }
  pref.begin("config", false); // save pref
  pref.putUChar("playing_mode", playMode);
  pref.end();
}

// --- Music Player Source & Style Helpers ---
void updateAlbumCoverStyle(void) {
  if (!ui_Player_Container_albumCover) return;
  if (mediaType != 1) {
    lv_obj_set_style_bg_img_recolor_opa(ui_Player_Container_albumCover, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_Player_Container_albumCover, 0, LV_PART_MAIN);
    if (ui_Player_Label_albumSource) {
      lv_obj_add_flag(ui_Player_Label_albumSource, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }

  // In mediaType == 1 (Music Player): Dedicated crisp card assets for zero overlap & perfect typography
  if (ui_Player_Label_albumSource) {
    lv_obj_add_flag(ui_Player_Label_albumSource, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_style_bg_img_recolor_opa(ui_Player_Container_albumCover, 0, LV_PART_MAIN);

  if (s_active_player_source == 1) { // LAN: Electric Cyan
    lv_obj_set_style_bg_img_src(ui_Player_Container_albumCover, &ui_img_music_lan_png, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_Player_Container_albumCover, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_Player_Container_albumCover, 2, LV_PART_MAIN);
    lv_obj_set_style_border_side(ui_Player_Container_albumCover, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
  } else { // LOCAL: Neon Purple
    lv_obj_set_style_bg_img_src(ui_Player_Container_albumCover, &ui_img_music_local_png, LV_PART_MAIN);
    lv_obj_set_style_border_color(ui_Player_Container_albumCover, lv_color_hex(0xBD00FF), LV_PART_MAIN);
    lv_obj_set_style_border_width(ui_Player_Container_albumCover, 2, LV_PART_MAIN);
    lv_obj_set_style_border_side(ui_Player_Container_albumCover, LV_BORDER_SIDE_FULL, LV_PART_MAIN);
  }
}

void applyPlayerSource(bool start_playback) {
  updateAlbumCoverStyle();
  char status_buffer[50];

  if (s_active_player_source == 1) { // LAN mode
    int lan_count = lan_get_file_count();
    if (lan_count <= 0) {
      if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0 (LAN)");
      if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "No LAN music indexed.\nPlease go to Settings -> Music\nand tap LOAD to index.");
      audioStopSong();
      return;
    }
    if (s_last_lan_track_idx < 0 || s_last_lan_track_idx >= lan_count) s_last_lan_track_idx = 0;
    lan_track_idx = s_last_lan_track_idx;
    snprintf(status_buffer, sizeof(status_buffer), "%d of %d (LAN)", lan_track_idx + 1, lan_count);
    if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
    const LanFileEntry *f = lan_get_file(lan_track_idx);
    if (f && ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, f->name);
    if (start_playback) lan_play(lan_track_idx);
  } else { // LOCAL SD mode
    if (trackListLength <= 0) {
      if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0 (LOCAL)");
      if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "SD music library empty.\nPlease go to Settings -> Music\nand tap LOAD to scan.");
      audioStopSong();
      return;
    }
    if (s_last_local_track_idx < 0 || s_last_local_track_idx >= trackListLength) s_last_local_track_idx = 0;
    trackIndex = s_last_local_track_idx;
    snprintf(status_buffer, sizeof(status_buffer), "%d of %d (LOCAL)", trackIndex + 1, trackListLength);
    if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);

    char trackPath[512];
    if (!getTrackPath(trackIndex, trackPath, sizeof(trackPath))) {
      if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "Track index corrupted.\nPlease go to Settings -> Music\nand tap LOAD to re-index.");
      audioStopSong();
      return;
    }
    const char *base = strrchr(trackPath, '/');
    base = base ? base + 1 : trackPath;
    char cleanTitle[64];
    snprintf(cleanTitle, sizeof(cleanTitle), "%s", base);
    char *dot = strrchr(cleanTitle, '.');
    if (dot) *dot = '\0';
    if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, cleanTitle);
    if (start_playback) audioPlayFS(0, trackPath);
  }
}

void togglePlayerSource(lv_event_t * e) {
  if (mediaType != 1) return; // STRICT GUARD: Only in Music Player mode!
  SCREEN_OFF_TIMER = millis();
  s_active_player_source = (s_active_player_source == 0) ? 1 : 0;
  log_i("[PLAYER] Source toggled to: %s", (s_active_player_source == 1) ? "LAN" : "LOCAL");
  applyPlayerSource(true);
}

// skip previous track
void previoustrack(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_PAUSE);
  lv_obj_set_style_radius(ui_Player_Button_play, 10, LV_PART_MAIN);

  char status_buffer[50];
  switch (mediaType) {

  case 0: { // livestream mode
    if (stationListLength == 0) {
      lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0");
      return;
    }
    // playing mode
    if (playMode == 0) {
      if (stationIndex == 0) {
        stationIndex = stationListLength - 1;
      } else {
        stationIndex--;
      }
    } else if (playMode == 1) {
      stationIndex = randomIndexExcept(stationListLength, stationIndex);

    } else if (mediaType == 2) {
      // play same current trackIndex
    }
    snprintf(status_buffer, sizeof(status_buffer), "%d of %d", stationIndex + 1, stationListLength);
    lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
    audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
    break;
  }

  case 1: { // music player mode
    if (s_active_player_source == 1) { // LAN mode
      int lan_count = lan_get_file_count();
      if (lan_count <= 0) {
        lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0 (LAN)");
        lv_textarea_set_text(ui_Player_Textarea_status, "No LAN music indexed.\nPlease go to Settings -> Music\nand tap LOAD to index.");
        return;
      }
      lan_track_idx = (lan_track_idx == 0) ? (lan_count - 1) : (lan_track_idx - 1);
      s_last_lan_track_idx = lan_track_idx;
      snprintf(status_buffer, sizeof(status_buffer), "%d of %d (LAN)", lan_track_idx + 1, lan_count);
      lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
      const LanFileEntry *f = lan_get_file(lan_track_idx);
      if (f) lv_textarea_set_text(ui_Player_Textarea_status, f->name);
      lan_play(lan_track_idx);
      break;
    }

    // LOCAL SD mode
    if (trackListLength <= 0) {
      lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0 (LOCAL)");
      lv_textarea_set_text(ui_Player_Textarea_status, "SD music library empty.\nPlease go to Settings -> Music\nand tap LOAD to scan.");
      return;
    }
    int pos = audio.getAudioCurrentTime();
    if (pos > 5) { // rewind if pos > 5 sec
      AudioCommandPayload msg = {
          .cmd = CMD_AUDIO_SET_PLAY_TIME,
          .value = 0,
      };
      xQueueSend(audio_cmd_queue, &msg, 0);
      return;
    }
    if (playMode == 0) {
      trackIndex = (trackIndex == 0) ? (trackListLength - 1) : (trackIndex - 1);
    } else if (playMode == 1) {
      trackIndex = randomIndexExcept(trackListLength, trackIndex);
    }
    s_last_local_track_idx = trackIndex;

    char trackPath[512];
    if (!getTrackPath(trackIndex, trackPath, sizeof(trackPath))) {
      lv_textarea_set_text(ui_Player_Textarea_status, "Track index corrupted.\nPlease go to Settings -> Music\nand tap LOAD to re-index.");
      return;
    }
    log_d("Prev track: %s", trackPath);
    snprintf(status_buffer, sizeof(status_buffer), "%d of %d (LOCAL)", trackIndex + 1, trackListLength);
    lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
    audioPlayFS(0, trackPath);
    break;
  }
  case 2: // chatbot mode
    // do nothing (or add specific action for chatbot here)
    break; // Cleaned up: only one break needed

  default:
    // It's good practice to handle unknown modes
    break;
  }
}

// skip back 5 sec
void fastReverse5(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  if (mediaType == 1) audioSetTimeOffset(-5);
}
// when user drag and release progress button
void playseek(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  if (mediaType == 1) {
    seeking_now = false; // disable skip slider knob position update during playing
    lv_obj_t *widget = lv_event_get_target(e);
    int16_t seek_time = lv_slider_get_value(widget);
    audioSetPlayTime(seek_time);
  }
}
// when user press on slider knob.
void SeekingNow(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  if (mediaType == 1) {
    seeking_now = true; // skip slider knob position update during playing
  }
}

void playpause(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer

  if (mediaType == 2) { // AI Assistant mode
    if (audio.isRunning()) {
      audioStopSong();
      UIStatusPayload msg = {.type = STATUS_UPDATE_AI_IDLE};
      xQueueSend(ui_status_queue, &msg, 100);
    } else if (ai_recording_active) {
      // Tap while recording -> stop early and submit audio to AI
      stop_ai_voice_recording_and_process();
    } else if (!ai_upload_in_progress) {
      // Tap to start recording voice from ES7210 microphone
      start_ai_voice_recording();
    }
    return;
  }

  if (audio.isRunning()) {
    audioPauseResume();
    lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_PLAY);
    lv_obj_set_style_radius(ui_Player_Button_play, 25, LV_PART_MAIN);
    paused = true;

  } else {

    lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_PAUSE);
    lv_obj_set_style_radius(ui_Player_Button_play, 10, LV_PART_MAIN);
    char status_buffer[50];
    switch (mediaType) {
    case 0: { // livestream mode
      if (stationListLength == 0) {
        lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0");
        return;
      }
      snprintf(status_buffer, sizeof(status_buffer), "%d of %d", stationIndex + 1, stationListLength);
      lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
      audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
      break; // break should be outside the scope block if not used for variable declaration
    }
    case 1: { // music player mode
      if (paused) {
        audioPauseResume();
        paused = false;
        return;
      }
      if (s_active_player_source == 1) { // LAN mode
        int lan_count = lan_get_file_count();
        if (lan_count <= 0) {
          if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0 (LAN)");
          if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "No LAN music indexed.\nPlease go to Settings -> Music\nand tap LOAD to index.");
          audioStopSong();
          return;
        }
        if (s_last_lan_track_idx < 0 || s_last_lan_track_idx >= lan_count) s_last_lan_track_idx = 0;
        lan_track_idx = s_last_lan_track_idx;
        snprintf(status_buffer, sizeof(status_buffer), "%d of %d (LAN)", lan_track_idx + 1, lan_count);
        if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
        const LanFileEntry *f = lan_get_file(lan_track_idx);
        if (f && ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, f->name);
        lan_play(lan_track_idx);
        break;
      } else { // LOCAL SD mode
        if (trackListLength <= 0) {
          if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0 (LOCAL)");
          if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "SD music library empty.\nPlease go to Settings -> Music\nand tap LOAD to scan.");
          audioStopSong();
          return;
        }
        if (s_last_local_track_idx < 0 || s_last_local_track_idx >= trackListLength) s_last_local_track_idx = 0;
        trackIndex = s_last_local_track_idx;
        char trackPath[512];
        if (!getTrackPath(trackIndex, trackPath, sizeof(trackPath))) {
          if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "Track index corrupted.\nPlease go to Settings -> Music\nand tap LOAD to re-index.");
          audioStopSong();
          return;
        }
        log_d("Play track: %s", trackPath);
        snprintf(status_buffer, sizeof(status_buffer), "%d of %d (LOCAL)", trackIndex + 1, trackListLength);
        if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
        const char *base = strrchr(trackPath, '/');
        base = base ? base + 1 : trackPath;
        char cleanTitle[64];
        snprintf(cleanTitle, sizeof(cleanTitle), "%s", base);
        char *dot = strrchr(cleanTitle, '.');
        if (dot) *dot = '\0';
        if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, cleanTitle);

        audioPlayFS(0, trackPath);
        break;
      }
    }
    }
  }
}

// skip next 5 sec
void fastForward5(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  if (mediaType == 1) audioSetTimeOffset(5);
}

// skip next track
void nexttrack(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset backlight timer
  lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_PAUSE);
  lv_obj_set_style_radius(ui_Player_Button_play, 10, LV_PART_MAIN);
  char status_buffer[50];
  switch (mediaType) {

  case 0: { // livestream mode
    if (stationListLength == 0) {
      lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0");
      return;
    }
    if (playMode == 0) { // normal play
      stationIndex++;
      if (stationIndex == stationListLength) {
        stationIndex = 0;
      }
    } else if (playMode == 1) { // random
      stationIndex = randomIndexExcept(stationListLength, stationIndex);
    } else if (mediaType == 2) { // single
      // play same current trackIndex
    }
    snprintf(status_buffer, sizeof(status_buffer), "%d of %d", stationIndex + 1, stationListLength);
    lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
    audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
    break; // break should be outside the scope block if not used for variable declaration
  }

  case 1: { // music player mode
    if (s_active_player_source == 1) { // LAN mode
      int lan_count = lan_get_file_count();
      if (lan_count <= 0) {
        if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0 (LAN)");
        if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "No LAN music indexed.\nPlease go to Settings -> Music\nand tap LOAD to index.");
        return;
      }
      if (playMode == 0) { // normal loop
        lan_track_idx = (lan_track_idx + 1) % lan_count;
      } else if (playMode == 1) { // random
        lan_track_idx = randomIndexExcept(lan_count, lan_track_idx);
      }
      s_last_lan_track_idx = lan_track_idx;
      snprintf(status_buffer, sizeof(status_buffer), "%d of %d (LAN)", lan_track_idx + 1, lan_count);
      if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
      const LanFileEntry *f = lan_get_file(lan_track_idx);
      if (f && ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, f->name);
      lan_play(lan_track_idx);
      break;
    }

    // LOCAL SD mode
    if (trackListLength <= 0) {
      if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, "0 of 0 (LOCAL)");
      if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "SD music library empty.\nPlease go to Settings -> Music\nand tap LOAD to scan.");
      return;
    }
    if (playMode == 0) { // normal play
      trackIndex++;
      if (trackIndex >= trackListLength) {
        trackIndex = 0;
      }
    } else if (playMode == 1) { // random
      trackIndex = randomIndexExcept(trackListLength, trackIndex);
    }
    s_last_local_track_idx = trackIndex;
    char trackPath[512];
    if (!getTrackPath(trackIndex, trackPath, sizeof(trackPath))) {
      if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, "Track index corrupted.\nPlease go to Settings -> Music\nand tap LOAD to re-index.");
      return;
    }
    log_d("Next track: %s", trackPath);
    snprintf(status_buffer, sizeof(status_buffer), "%d of %d (LOCAL)", trackIndex + 1, trackListLength);
    if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
    const char *base = strrchr(trackPath, '/');
    base = base ? base + 1 : trackPath;
    char cleanTitle[64];
    snprintf(cleanTitle, sizeof(cleanTitle), "%s", base);
    char *dot = strrchr(cleanTitle, '.');
    if (dot) *dot = '\0';
    if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, cleanTitle);

    audioPlayFS(0, trackPath);
    break;
  }

  case 2: // chatbot mode
    // do nothing (or add specific action for chatbot here)
    break; // Cleaned up: only one break needed

  default:
    // It's good practice to handle unknown modes
    break;
  }
}

//  ------- Screen Control -------

//  Set backlight timeout event
void setTimer(lv_event_t *e) {
  lv_obj_t *widget = lv_event_get_target(e);
  timeout_index = lv_dropdown_get_selected(widget);
  switch (timeout_index) {
  case 0: SCREEN_OFF_DELAY = 0; break;
  case 1: SCREEN_OFF_DELAY = 15000; break;
  case 2: SCREEN_OFF_DELAY = 30000; break;
  case 3: SCREEN_OFF_DELAY = 60000; break;
  }
  SCREEN_OFF_TIMER = millis(); // reset timer
}
//  Set backlight brightness event
void setBrightness(lv_event_t *e) {
  if (e != NULL && ui_MainMenu_Dropdown_Brightness != NULL) {
    uint16_t sel = lv_dropdown_get_selected(ui_MainMenu_Dropdown_Brightness);
    if (sel <= 2) backlight_state = (uint8_t)sel;
  }
  if (backlight_state > 2) backlight_state = 1;
  switch (backlight_state) {
    case 0: setUpduty(LCD_BL_LOW); break;
    case 1: setUpduty(LCD_BL_MED); break;
    case 2: default: setUpduty(LCD_BL_HIGH); break;
  }
  SCREEN_OFF_TIMER = millis(); // reset timer
}

// turn on screen by double tap
#define DOUBLE_TAP_SPEED 1000
void turnonScreen(lv_event_t *e) {
  static uint32_t last_click_time = 0;
  uint32_t current_time = lv_tick_get();
  if(current_time - last_click_time < DOUBLE_TAP_SPEED) {
       last_click_time = 0;
      log_d("screen on");
      switch (backlight_state) {
      case 0: setUpduty(LCD_PWM_MODE_100); break;
      case 1: setUpduty(LCD_PWM_MODE_150); break;
      case 2: setUpduty(LCD_PWM_MODE_255); break;
      }
      UIStatusPayload msg = {
        .type = STATUS_SCREEN_UNLOCK,
      };
      xQueueSend(ui_status_queue, &msg, 100); // send message
    resetScreenOffTimer(NULL);
  }
  last_click_time = current_time;  
}

// Apply procedural vector theme with zero flash overhead
void setWallpaper(lv_event_t *e) {
  lv_obj_t *widget = ui_MainMenu_Dropdown_Wallpaper;
  if (widget) {
    wallpaperIndex = lv_dropdown_get_selected(widget);
  }
  if (wallpaperIndex >= MAX_WALLPAPER) {
    wallpaperIndex = 0;
  }

  const VectorTheme &thm = vector_themes[wallpaperIndex];
  if (ui_Screen_Player) {
    lv_obj_set_style_bg_img_src(ui_Screen_Player, NULL, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui_Screen_Player, lv_color_hex(thm.bg_top), LV_PART_MAIN);
    if (thm.bg_top != thm.bg_bottom) {
      lv_obj_set_style_bg_grad_color(ui_Screen_Player, lv_color_hex(thm.bg_bottom), LV_PART_MAIN);
      lv_obj_set_style_bg_grad_dir(ui_Screen_Player, LV_GRAD_DIR_VER, LV_PART_MAIN);
    } else {
      lv_obj_set_style_bg_grad_dir(ui_Screen_Player, LV_GRAD_DIR_NONE, LV_PART_MAIN);
    }
  }
  SCREEN_OFF_TIMER = millis(); // reset timer
}

/*

#include <esp_heap_caps.h> // Ensure you include this if using ps_malloc

// Function to load entire file from LittleFS into PSRAM
// The returned pointer MUST be freed when the image is no longer needed.
const uint8_t *load_file_from_fs(const char *lvgl_path, size_t expected_size) {
    if (lvgl_path == NULL) return NULL;

    // Strip the "L:" prefix (assuming path is "L:/wallpapers/xmas4.bin")
    const char *fs_path = lvgl_path + 2;

    if (!LittleFS.exists(fs_path)) {
        log_e("Load Fail: File not found at %s", fs_path);
        return NULL;
    }

    File f = LittleFS.open(fs_path, "r");
    if (!f) {
        log_e("Load Fail: Failed to open file: %s", fs_path);
        return NULL;
    }

    size_t size = f.size();
    if (size < expected_size) {
        log_w("Load Warning: File size mismatch. Expected %d, got %d", expected_size, size);
        // We will continue loading, but log the warning.
    }

    // Allocate buffer in PSRAM
    uint8_t *data = (uint8_t *)ps_malloc(size);

    if (data == NULL) {
        log_e("Load Fail: Failed to allocate %d bytes (PSRAM full?)", size);
        f.close();
        return NULL;
    }

    size_t bytesRead = f.read(data, size);
    f.close();

    if (bytesRead != size) {
        log_e("Load Fail: Read incomplete. Expected %d, got %d.", size, bytesRead);
        free(data);
        return NULL;
    }

    log_d("Load Success: %s loaded into PSRAM (%d bytes).", fs_path, size);
    return data;
}

lv_img_dsc_t  wallpaper_img_dsc = {
   {
     0,
        640,
        172,
       LV_IMG_CF_RGB565A8, // Assuming 3 bytes/pixel (640*172*3 = 330240)
    },
    0,
    NULL,
};


void setWallpaper(lv_event_t *e) {
    lv_obj_t *widget = ui_MainMenu_Dropdown_Wallpaper;
    int wallpaperIndex = lv_dropdown_get_selected(widget);

    const char *path = NULL;

    switch (wallpaperIndex) {
        case 0: path = NULL; break;
        case 1: path = "L:/wallpapers/wallpaper1.jpg"; break;
        case 2: path = "S:/wallpapers/wallpaper2.jpg"; break;
        case 3: path = "S:/wallpapers/wallpaper3.jpg"; break;
    }

    // CRITICAL: Free the previous memory allocation before loading new data
    if (wallpaper_img_dsc.data != NULL) {
        free((void *)wallpaper_img_dsc.data);
        wallpaper_img_dsc.data = NULL;
    }

    // Check if we are setting an image (case 0 is NULL)
    if (path != NULL) {
        // CALL THE EXPLICIT LOADING FUNCTION
        wallpaper_img_dsc.data = load_file_from_fs(path, 330240);
        wallpaper_img_dsc.data_size = 330240;
    } else {
        // Clear the source if the path is NULL
        wallpaper_img_dsc.data = NULL;
        wallpaper_img_dsc.data_size = 0;
    }

    log_d("Set Wallpaper: %s", path ? path : "NULL (Default)");

    lv_obj_set_style_bg_img_src(ui_Screen_Player, &wallpaper_img_dsc, LV_PART_MAIN);

    SCREEN_OFF_TIMER = millis();
}

*/
//----------------- Main Menu Selection events ---------------------
// mode 0 = live streaming , 1 = music player, 2 = ai chat
// Livestream mode event
extern "C" void on_radio_language_changed(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis();
  lv_obj_t *dd = lv_event_get_target(e);
  if (!dd) return;
  uint16_t sel = lv_dropdown_get_selected(dd);
  log_i("[RADIO UI] Language changed to: %u (%s)", sel, getRadioLanguageName((uint8_t)sel));
  switchRadioLanguage((uint8_t)sel);
}

extern "C" void on_catalog_button_clicked(void) {
  if (mediaType == 2) {
    // Cycle AI recording duration: 4s -> 6s -> 8s -> 10s -> 15s -> 4s
    if (ai_rec_duration_sec == 4) ai_rec_duration_sec = 6;
    else if (ai_rec_duration_sec == 6) ai_rec_duration_sec = 8;
    else if (ai_rec_duration_sec == 8) ai_rec_duration_sec = 10;
    else if (ai_rec_duration_sec == 10) ai_rec_duration_sec = 15;
    else ai_rec_duration_sec = 4;

    pref.begin("config", false);
    pref.putUChar("ai_dur", ai_rec_duration_sec);
    pref.end();

    char buf[16];
    snprintf(buf, sizeof(buf), LV_SYMBOL_SETTINGS " %ds", ai_rec_duration_sec);
    if (ui_Player_Label_Catalog) lv_label_set_text(ui_Player_Label_Catalog, buf);
    log_i("[AI CONFIG] Recording duration set to %us", (unsigned)ai_rec_duration_sec);
    return;
  }

  if (currentRadioLang == RADIO_LANG_HI) {
    if (currentRadioCatalog == RADIO_CATALOG_ONLINE_RADIO_FM) {
      switchRadioCatalog(RADIO_CATALOG_RADIO_INDIA);
    } else {
      switchRadioCatalog(RADIO_CATALOG_ONLINE_RADIO_FM);
    }
  } else {
    // In other languages (EN, ES, CN, DE, JA), tapping catalog cycles language
    uint8_t nextLang = (currentRadioLang + 1) % 6;
    switchRadioLanguage(nextLang);
    if (ui_MainMenu_Dropdown_RadioLang) {
      lv_dropdown_set_selected(ui_MainMenu_Dropdown_RadioLang, nextLang);
    }
  }

  if (ui_Player_Label_Catalog) {
    if (currentRadioLang == RADIO_LANG_HI) {
      lv_label_set_text(ui_Player_Label_Catalog, 
          (currentRadioCatalog == RADIO_CATALOG_ONLINE_RADIO_FM) ? LV_SYMBOL_AUDIO " FM" : LV_SYMBOL_WIFI " IN");
    } else {
      char cbuf[16];
      snprintf(cbuf, sizeof(cbuf), LV_SYMBOL_AUDIO " %s", getRadioLanguageCode(currentRadioLang));
      lv_label_set_text(ui_Player_Label_Catalog, cbuf);
    }
  }
  char status_buffer[50];
  snprintf(status_buffer, sizeof(status_buffer), "%d of %d", stationIndex + 1, stationListLength);
  if (ui_Player_Label_trackNumber) {
    lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
    lv_obj_set_x(ui_Player_Label_trackNumber, 252);
  }
  if (stationListLength > 0 && stationIndex < stationListLength) {
    if (ui_Player_Textarea_status && stations[stationIndex].name) {
      lv_textarea_set_text(ui_Player_Textarea_status, stations[stationIndex].name);
    }
    audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
  }
}

static void set_player_ai_chat_mode(bool is_ai) {
  if (is_ai) {
    if (ui_Player_Slider_Progress) lv_obj_add_flag(ui_Player_Slider_Progress, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_RndRPT) lv_obj_add_flag(ui_Player_Button_RndRPT, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_previous) lv_obj_add_flag(ui_Player_Button_previous, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_skipback) lv_obj_add_flag(ui_Player_Button_skipback, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_skipforward) lv_obj_add_flag(ui_Player_Button_skipforward, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_next) lv_obj_add_flag(ui_Player_Button_next, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Label_ElapseTime) lv_obj_add_flag(ui_Player_Label_ElapseTime, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Label_RemainTime) lv_obj_add_flag(ui_Player_Label_RemainTime, LV_OBJ_FLAG_HIDDEN);

    // Hide catalog button in AI mode - VAD silence detection automatically handles stopping!
    if (ui_Player_Button_Catalog) {
      lv_obj_add_flag(ui_Player_Button_Catalog, LV_OBJ_FLAG_HIDDEN);
    }

    // Expand and format text area for multi-line AI chat responses
    if (ui_Player_Textarea_status) {
      lv_obj_set_x(ui_Player_Textarea_status, 175);
      lv_obj_set_y(ui_Player_Textarea_status, 26);
      lv_obj_set_width(ui_Player_Textarea_status, 400);
      lv_obj_set_height(ui_Player_Textarea_status, 96);
      lv_obj_set_style_text_align(ui_Player_Textarea_status, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
      lv_obj_set_style_text_font(ui_Player_Textarea_status, &lv_font_montserrat_14, LV_PART_MAIN);
      lv_obj_add_flag(ui_Player_Textarea_status, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_scrollbar_mode(ui_Player_Textarea_status, LV_SCROLLBAR_MODE_AUTO);
      lv_obj_set_style_pad_left(ui_Player_Textarea_status, 4, LV_PART_MAIN);
      lv_obj_set_style_pad_right(ui_Player_Textarea_status, 4, LV_PART_MAIN);
      lv_obj_set_style_pad_top(ui_Player_Textarea_status, 2, LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(ui_Player_Textarea_status, 2, LV_PART_MAIN);
    }

    if (ui_Player_Button_play) {
      lv_obj_set_size(ui_Player_Button_play, 130, 42);
      lv_obj_set_style_radius(ui_Player_Button_play, 21, LV_PART_MAIN);
      lv_obj_set_style_bg_color(ui_Player_Button_play, lv_color_hex(0x06281D), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(ui_Player_Button_play, 255, LV_PART_MAIN);
      lv_obj_set_style_border_color(ui_Player_Button_play, lv_color_hex(0x00FF99), LV_PART_MAIN);
      lv_obj_set_style_border_width(ui_Player_Button_play, 2, LV_PART_MAIN);
    }
    if (ui_Player_Label_Label5) {
      lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_AUDIO "  MIC");
      lv_obj_set_style_text_color(ui_Player_Label_Label5, lv_color_hex(0x00FF99), LV_PART_MAIN);
      lv_obj_set_style_text_font(ui_Player_Label_Label5, &lv_font_montserrat_14, LV_PART_MAIN);
    }
  } else {
    // Restore Radio and Music Player mode
    if (ui_Player_Slider_Progress) lv_obj_clear_flag(ui_Player_Slider_Progress, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_RndRPT) lv_obj_clear_flag(ui_Player_Button_RndRPT, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_previous) lv_obj_clear_flag(ui_Player_Button_previous, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_skipback) lv_obj_clear_flag(ui_Player_Button_skipback, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_skipforward) lv_obj_clear_flag(ui_Player_Button_skipforward, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Button_next) lv_obj_clear_flag(ui_Player_Button_next, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Label_ElapseTime) lv_obj_clear_flag(ui_Player_Label_ElapseTime, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Label_RemainTime) lv_obj_clear_flag(ui_Player_Label_RemainTime, LV_OBJ_FLAG_HIDDEN);

    if (mediaType == 0 && ui_Player_Button_Catalog) {
      lv_obj_clear_flag(ui_Player_Button_Catalog, LV_OBJ_FLAG_HIDDEN);
      if (ui_Player_Label_Catalog) {
        if (currentRadioLang == RADIO_LANG_HI) {
          lv_label_set_text(ui_Player_Label_Catalog,
              (currentRadioCatalog == RADIO_CATALOG_ONLINE_RADIO_FM) ? LV_SYMBOL_AUDIO " FM" : LV_SYMBOL_WIFI " IN");
        } else {
          char cbuf[16];
          snprintf(cbuf, sizeof(cbuf), LV_SYMBOL_AUDIO " %s", getRadioLanguageCode(currentRadioLang));
          lv_label_set_text(ui_Player_Label_Catalog, cbuf);
        }
      }
      if (ui_Player_Label_trackNumber) lv_obj_set_x(ui_Player_Label_trackNumber, 252);
    } else {
      if (ui_Player_Button_Catalog) lv_obj_add_flag(ui_Player_Button_Catalog, LV_OBJ_FLAG_HIDDEN);
      if (ui_Player_Label_trackNumber) lv_obj_set_x(ui_Player_Label_trackNumber, 180);
    }

    // Optimize text area for Radio & LAN streaming track info
    if (ui_Player_Textarea_status) {
      lv_obj_set_x(ui_Player_Textarea_status, 175);
      lv_obj_set_y(ui_Player_Textarea_status, 26);
      lv_obj_set_width(ui_Player_Textarea_status, 400);
      lv_obj_set_height(ui_Player_Textarea_status, 76);
      lv_obj_set_style_text_align(ui_Player_Textarea_status, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN);
      lv_obj_set_style_text_font(ui_Player_Textarea_status, &ui_font_NotoSanThai20, LV_PART_MAIN);
      lv_obj_add_flag(ui_Player_Textarea_status, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_scrollbar_mode(ui_Player_Textarea_status, LV_SCROLLBAR_MODE_AUTO);
      lv_obj_set_style_pad_left(ui_Player_Textarea_status, 4, LV_PART_MAIN);
      lv_obj_set_style_pad_right(ui_Player_Textarea_status, 4, LV_PART_MAIN);
      lv_obj_set_style_pad_top(ui_Player_Textarea_status, 2, LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(ui_Player_Textarea_status, 2, LV_PART_MAIN);
    }

    if (ui_Player_Button_play) {
      lv_obj_set_size(ui_Player_Button_play, 45, 45);
      lv_obj_set_style_radius(ui_Player_Button_play, 25, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(ui_Player_Button_play, 0, LV_PART_MAIN);
      lv_obj_set_style_border_width(ui_Player_Button_play, 0, LV_PART_MAIN);
    }
    if (ui_Player_Label_Label5) {
      lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_PLAY);
      lv_obj_set_style_text_color(ui_Player_Label_Label5, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    }
  }
}

static void menu_audio_anim_cb(lv_timer_t *t) {
  if (!ui_MainMenu_Panel_Menu || lv_scr_act() != ui_Screen_MainMenu) return;

  bool playing = audio.isRunning();
  lv_obj_t *targets[3] = {ui_MainMenu_Image_LiveStreaming, ui_MainMenu_Image_MusicPlayer, ui_MainMenu_Image_ChatBot};
  uint32_t colors[3]   = {0xFF2A85, 0xBD00FF, 0x00E676}; // Coral Pink (Radio), Purple (Music), Green (Voice)

  for (int i = 0; i < 3; i++) {
    if (!targets[i]) continue;
    lv_obj_set_style_img_recolor_opa(targets[i], 0, LV_PART_MAIN);
    if (playing && mediaType == i) {
      lv_obj_set_style_outline_color(targets[i], lv_color_hex(colors[i]), LV_PART_MAIN);
      lv_obj_set_style_outline_width(targets[i], 2, LV_PART_MAIN);
      lv_obj_set_style_outline_pad(targets[i], 2, LV_PART_MAIN);
      lv_obj_set_style_opa(targets[i], 255, 0);
    } else {
      lv_obj_set_style_outline_width(targets[i], 0, LV_PART_MAIN);
      lv_obj_set_style_opa(targets[i], 255, 0);
    }
  }
}

void livestreamMode(lv_event_t *e) {
  mediaType = 0;
  set_player_ai_chat_mode(false);
  char status_buffer[50];
  snprintf(status_buffer, sizeof(status_buffer), "%d of %d", stationIndex + 1, stationListLength);
  if (ui_Player_Label_trackNumber) {
    lv_label_set_text(ui_Player_Label_trackNumber, status_buffer);
    lv_obj_set_x(ui_Player_Label_trackNumber, 252);
  }
  if (ui_Player_Label_Label5) lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_PLAY);
  if (ui_Player_Button_play) lv_obj_set_style_radius(ui_Player_Button_play, 25, LV_PART_MAIN);
  if (ui_Player_Textarea_status) {
    if (stationListLength > 0 && stationIndex < stationListLength && stations[stationIndex].name) {
      lv_textarea_set_text(ui_Player_Textarea_status, stations[stationIndex].name);
    } else {
      lv_textarea_set_text(ui_Player_Textarea_status, "Online Radio");
    }
  }
  if (ui_Player_Label_ElapseTime) lv_label_set_text(ui_Player_Label_ElapseTime, "0:00");
  if (ui_Player_Label_RemainTime) lv_label_set_text(ui_Player_Label_RemainTime, "0:00");
  if (ui_Player_Slider_Progress) lv_slider_set_value(ui_Player_Slider_Progress, 0, LV_ANIM_OFF);
  audioStopSong();
  if (ui_Player_Button_Catalog) {
    lv_obj_clear_flag(ui_Player_Button_Catalog, LV_OBJ_FLAG_HIDDEN);
    if (ui_Player_Label_Catalog) {
      if (currentRadioLang == RADIO_LANG_HI) {
        lv_label_set_text(ui_Player_Label_Catalog, 
            (currentRadioCatalog == RADIO_CATALOG_ONLINE_RADIO_FM) ? LV_SYMBOL_AUDIO " FM" : LV_SYMBOL_WIFI " IN");
      } else {
        char cbuf[16];
        snprintf(cbuf, sizeof(cbuf), LV_SYMBOL_AUDIO " %s", getRadioLanguageCode(currentRadioLang));
        lv_label_set_text(ui_Player_Label_Catalog, cbuf);
      }
    }
  }
  lv_anim_del(NULL, (lv_anim_exec_xcb_t)_ui_anim_callback_set_opacity);
  if (ui_MainMenu_Image_MusicPlayer) lv_obj_set_style_opa(ui_MainMenu_Image_MusicPlayer, 255, 0);
  if (ui_MainMenu_Image_ChatBot) lv_obj_set_style_opa(ui_MainMenu_Image_ChatBot, 255, 0);
  if (ui_MainMenu_Image_LiveStreaming) lv_obj_set_style_opa(ui_MainMenu_Image_LiveStreaming, 255, 0);
  if (ui_Player_Container_albumCover) lv_obj_set_style_bg_img_src(ui_Player_Container_albumCover, &ui_img_images_livestreaming_png, LV_PART_MAIN);
  updateAlbumCoverStyle();
  SCREEN_OFF_TIMER = millis(); // reset timer
  BL_OFF = false; // auto backlight on
}

// Music player mode event
void musicPlayerMode(lv_event_t *e) {
  set_player_ai_chat_mode(false);
  if (mediaType != 1) { // switch from other mode
    applyPlayerSource(false);
    if (ui_Player_Label_Label5) lv_label_set_text(ui_Player_Label_Label5, LV_SYMBOL_PLAY);
    if (ui_Player_Button_play) lv_obj_set_style_radius(ui_Player_Button_play, 25, LV_PART_MAIN);
    if (ui_Player_Label_Label16) {
      lv_obj_set_style_text_font(ui_Player_Label_Label16, &lv_font_montserrat_14, LV_PART_MAIN);
      switch (playMode) {
      case 0: lv_label_set_text(ui_Player_Label_Label16, LV_SYMBOL_LOOP); break;
      case 1: lv_label_set_text(ui_Player_Label_Label16, LV_SYMBOL_SHUFFLE); break;
      case 2: lv_label_set_text(ui_Player_Label_Label16, "1"); break;
      }
    }
    audioStopSong();
  }
  mediaType = 1;
  if (ui_Player_Button_Catalog) {
    lv_obj_add_flag(ui_Player_Button_Catalog, LV_OBJ_FLAG_HIDDEN);
  }
  lv_anim_del(NULL, (lv_anim_exec_xcb_t)_ui_anim_callback_set_opacity);
  if (ui_MainMenu_Image_LiveStreaming) lv_obj_set_style_opa(ui_MainMenu_Image_LiveStreaming, 255, 0);
  if (ui_MainMenu_Image_ChatBot) lv_obj_set_style_opa(ui_MainMenu_Image_ChatBot, 255, 0);
  if (ui_MainMenu_Image_MusicPlayer) lv_obj_set_style_opa(ui_MainMenu_Image_MusicPlayer, 255, 0);
  if (ui_Player_Container_albumCover) lv_obj_set_style_bg_img_src(ui_Player_Container_albumCover, &ui_img_images_music_png, LV_PART_MAIN);
  updateAlbumCoverStyle();
  SCREEN_OFF_TIMER = millis(); // reset timer
  BL_OFF = false; // auto backlight on
}

// Chatbot mode event
void chatBotMode(lv_event_t *e) {
  set_player_ai_chat_mode(true);
  if (mediaType != 2) {
    if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, "AI ASSISTANT");
    audioStopSong();
  }
  mediaType = 2;
  if (ui_Player_Textarea_status) {
    lv_textarea_set_text(ui_Player_Textarea_status, "AI Voice Assistant Ready\nTap [ MIC ] to speak or ask via Web Remote / CLI!");
  }
  lv_anim_del(NULL, (lv_anim_exec_xcb_t)_ui_anim_callback_set_opacity);
  if (ui_MainMenu_Image_MusicPlayer) lv_obj_set_style_opa(ui_MainMenu_Image_MusicPlayer, 255, 0);
  if (ui_MainMenu_Image_LiveStreaming) lv_obj_set_style_opa(ui_MainMenu_Image_LiveStreaming, 255, 0);
  if (ui_MainMenu_Image_ChatBot) lv_obj_set_style_opa(ui_MainMenu_Image_ChatBot, 255, 0);
  if (ui_Player_Container_albumCover) lv_obj_set_style_bg_img_src(ui_Player_Container_albumCover, &ui_img_images_assistant_png, LV_PART_MAIN);
  updateAlbumCoverStyle();
  SCREEN_OFF_TIMER = millis(); // reset timer
  BL_OFF = false; // auto backlight on
}
// information Mode event
void informationMode(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  BL_OFF = false; // auto backlight on
  updateInfoPanel(infoPageIndex);
}
// Utility Mode event
void utilityMode(lv_event_t *e) {
  // performOnlineUpdate();
}

//-------------SAVE CONFIGURATION  Menu ----------------------
// config screen close and save setting to preference
void saveConfig(lv_event_t *e) {
  pref.begin("config", false);
  pref.putUChar("bl_state", backlight_state);
  pref.putUChar("scroffdelay", timeout_index);
  pref.putUChar("wallpaper", wallpaperIndex);
  // Region
  if (lv_obj_has_state(ui_MainMenu_Checkbox_AutoIP, LV_STATE_CHECKED)) {
    strcpy(query_parameter, "auto:ip");
  } else { // merge lat long into  lag,long
    const char *lat = lv_textarea_get_text(ui_MainMenu_Textarea_Latitude);
    const char *lon = lv_textarea_get_text(ui_MainMenu_Textarea_Longitude);
    snprintf(query_parameter, sizeof(query_parameter), "%s,%s", lat, lon);
  }
  log_d("Region saved: query_parameter = %s", query_parameter);
  pref.putBytes("query_parameter", query_parameter, strlen(query_parameter) + 1);
  //---------
  pref.putUChar("offset_hour", offset_hour_index);
  pref.putUChar("offset_minute", offset_minute_index);
  pref.putUChar("temp_unit", temp_unit);

  //wifienable
  pref.putBool("wifi_enable", wifiEnable);

  pref.end();
  SCREEN_OFF_TIMER = millis(); // reset timer
  if (WiFi.status() == WL_CONNECTED) {//sync time if wifi connected
      rtc.ntp_sync(UTC_offset_hour[offset_hour_index], UTC_offset_minute[offset_minute_index]);
   }
}

// user save wifi credential
void saveWiFiCredential(lv_event_t *e) {
  char newSSID[64];
  lv_dropdown_get_selected_str(ui_MainMenu_Dropdown_NetworkList, newSSID, sizeof(newSSID));
  // read from your input box
  const char *newPWD = lv_textarea_get_text(ui_MainMenu_Textarea_Password);
  // Load → update → save
  uint8_t wifiCount = loadWifiList(wifiList);
  wifiCount = addOrUpdateWifi(newSSID, newPWD, wifiList, wifiCount);
  saveWifiList(wifiList, wifiCount);
  SCREEN_OFF_TIMER = millis(); // reset timer
}

// Discovery wifi network into dropdown
void scanNetwork(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  scanWiFi(true); // scan and update wifi list in dropdown
}
//toggle wifi on/off
void toggleWiFi(lv_event_t * e) {
  SCREEN_OFF_TIMER = millis(); // reset timer

  if (wifiEnable) {//disabled
    wifiEnable = false;
    //delete wifi check timer
    if (wifi_check_timer) lv_timer_del(wifi_check_timer);
    if(wifiTaskHandle != NULL) {//kill wifi_connect_task
      vTaskDelete(wifiTaskHandle);
      wifiTaskHandle = NULL;
    }
    WiFi.disconnectAsync();
    lv_label_set_text(ui_MainMenu_Label_connectStatus, "Disconnected");
    lv_obj_set_style_text_color(ui_MainMenu_Label_connectStatus, lv_color_hex(0xFF0000), LV_PART_MAIN);
    lv_obj_set_style_text_color(ui_Player_Label_WiFi, lv_color_hex(0x777777), LV_PART_MAIN);
    log_d("WiFi Disconnected");

  } else {//enabled

    wifiEnable = true;
    //start wifi check timer
    wifi_check_timer = lv_timer_create(lvgl_wifi_check_cb, WIFI_CHECK_INTERVAL, NULL);//wifi status check interval timer
  }
}



void toggleMusicMode(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis();
  s_settings_music_mode = (s_settings_music_mode == 0) ? 1 : 0;
  char buf[64];

  if (s_settings_music_mode == 1) { // LAN
    if (ui_MainMenu_Label_musicMode) lv_label_set_text(ui_MainMenu_Label_musicMode, "LAN");
    if (ui_MainMenu_Button_musicMode) {
      lv_obj_set_style_bg_color(ui_MainMenu_Button_musicMode, lv_color_hex(0x0088cc), LV_PART_MAIN);
      lv_obj_set_style_border_color(ui_MainMenu_Button_musicMode, lv_color_hex(0x00E5FF), LV_PART_MAIN);
    }
    if (ui_MainMenu_Button_LanUrl) lv_obj_clear_flag(ui_MainMenu_Button_LanUrl, LV_OBJ_FLAG_HIDDEN);
    if (ui_MainMenu_Label_LanUrl) {
      snprintf(buf, sizeof(buf), "URL: %s", lan_get_server());
      lv_label_set_text(ui_MainMenu_Label_LanUrl, buf);
    }
    if (ui_MainMenu_Label_Label15) lv_label_set_text(ui_MainMenu_Label_Label15, "Tap LOAD to fetch LAN audio files");
    if (ui_MainMenu_Label_trackCount) {
      snprintf(buf, sizeof(buf), "LAN Tracks: %d", lan_get_file_count());
      lv_label_set_text(ui_MainMenu_Label_trackCount, buf);
    }
  } else { // LOCAL
    if (ui_MainMenu_Label_musicMode) lv_label_set_text(ui_MainMenu_Label_musicMode, "LOCAL");
    if (ui_MainMenu_Button_musicMode) {
      lv_obj_set_style_bg_color(ui_MainMenu_Button_musicMode, lv_color_hex(0x024564), LV_PART_MAIN);
      lv_obj_set_style_border_color(ui_MainMenu_Button_musicMode, lv_color_hex(0x00D2FF), LV_PART_MAIN);
    }
    if (ui_MainMenu_Button_LanUrl) lv_obj_add_flag(ui_MainMenu_Button_LanUrl, LV_OBJ_FLAG_HIDDEN);
    if (ui_MainMenu_Label_Label15) lv_label_set_text(ui_MainMenu_Label_Label15, "Tap LOAD to index SD card music");
    if (ui_MainMenu_Label_trackCount) {
      snprintf(buf, sizeof(buf), "Tracks: %d", trackListLength);
      lv_label_set_text(ui_MainMenu_Label_trackCount, buf);
    }
  }
}

void openLanUrlPanel(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis();
  if (ui_MainMenu_Textarea_LanUrl) {
    lv_textarea_set_text(ui_MainMenu_Textarea_LanUrl, lan_get_server());
  }
  if (ui_MainMenu_Panel_LanUrl) {
    lv_obj_clear_flag(ui_MainMenu_Panel_LanUrl, LV_OBJ_FLAG_HIDDEN);
  }
  if (ui_MainMenu_Keyboard_Keyboard1 && ui_MainMenu_Textarea_LanUrl) {
    _ui_keyboard_set_target(ui_MainMenu_Keyboard_Keyboard1, ui_MainMenu_Textarea_LanUrl);
    lv_obj_clear_flag(ui_MainMenu_Keyboard_Keyboard1, LV_OBJ_FLAG_HIDDEN);
  }
}

void saveLanUrlAndClose(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis();
  if (ui_MainMenu_Textarea_LanUrl) {
    const char *url = lv_textarea_get_text(ui_MainMenu_Textarea_LanUrl);
    if (url && strlen(url) > 0) {
      lan_set_server(url);
      log_i("[LAN SETTINGS] Updated LAN server: %s", url);
    }
  }
  if (ui_MainMenu_Label_LanUrl) {
    char buf[64];
    snprintf(buf, sizeof(buf), "URL: %s", lan_get_server());
    lv_label_set_text(ui_MainMenu_Label_LanUrl, buf);
  }
  if (ui_MainMenu_Panel_LanUrl) {
    lv_obj_add_flag(ui_MainMenu_Panel_LanUrl, LV_OBJ_FLAG_HIDDEN);
  }
  if (ui_MainMenu_Keyboard_Keyboard1) {
    lv_obj_add_flag(ui_MainMenu_Keyboard_Keyboard1, LV_OBJ_FLAG_HIDDEN);
  }
}

//---------------------------------------
void readKeyboard(lv_event_t *e) {
  lv_obj_t *kb = lv_event_get_target(e);
  // Which button was pressed
  uint16_t btn_id = lv_btnmatrix_get_selected_btn(kb);
  const char *txt = lv_btnmatrix_get_btn_text(kb, btn_id);
  if (txt == NULL) return;
  if (strcmp(txt, LV_SYMBOL_OK) == 0) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN); // ✅ OK pressed → hide keyboard
    if (ui_MainMenu_Textarea_LanUrl && lv_keyboard_get_textarea(kb) == ui_MainMenu_Textarea_LanUrl) {
      saveLanUrlAndClose(NULL);
    }
  }
  SCREEN_OFF_TIMER = millis(); // reset timer
}

// force update song list from sdcard and creaet index file in littleFS
void loadMusicFromSDCARD(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  if (s_settings_music_mode == 1) { // LAN mode
    if (ui_MainMenu_Label_Label15) lv_label_set_text(ui_MainMenu_Label_Label15, "Connecting to LAN server...");
    lan_fetch_files_async();
  } else { // LOCAL mode
    scanMusic();
  }
}

// copy stations.csv from sdcard to littleFS
void loadStationFromSDCARD(lv_event_t *e) {
  // if no file stations.csv in Sd card then load default
  if (copyStationsCSV_SD_to_LittleFS()) {
    loadStationList();
  } else {
    lv_textarea_set_text(ui_MainMenu_Textarea_stationList, LV_SYMBOL_WARNING " Error copy file " MACRO_TO_STRING(STATION_LIST_FILENAME) "\nFile not found\nPlease insert SD Card\nwith file " MACRO_TO_STRING(STATION_LIST_FILENAME));
  }
  SCREEN_OFF_TIMER = millis(); // reset timer
}
// reset SCREEN_OFF_TIMER timer on config panel
void resetScreenOffTimer(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  s_clock_interaction_ms = millis();
  BL_OFF = false; // wake screen explicitly
  if (clock_face_active) {
    on_clock_interaction_wake();
  }
}

//------------ Info screen events ---------------
static const uint8_t maxInfoPages = 3;
// uint8_t infoPageIndex = 0-> clock, 1->weather, 2->alarm

void updateInfoPanel(uint8_t pages) {
  log_d("Info Panel Page: %d", pages);
  switch (pages) {
  case 0: { // nixie clock (Page 0)
    weatherAnimation(false);
    lv_obj_clear_flag(ui_Info_Panel_Clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Info_Panel_Weather, LV_OBJ_FLAG_HIDDEN);
    if (ui_Info_Panel_Alarm) lv_obj_add_flag(ui_Info_Panel_Alarm, LV_OBJ_FLAG_HIDDEN);
    enter_clock_breathing();
    break;
  }
  case 1: { // weather / climate (Page 1)
    if (clock_face_active) exit_clock_breathing();
    lv_obj_clear_flag(ui_Info_Panel_Weather, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Info_Panel_Clock, LV_OBJ_FLAG_HIDDEN);
    if (ui_Info_Panel_Alarm) lv_obj_add_flag(ui_Info_Panel_Alarm, LV_OBJ_FLAG_HIDDEN);
    weatherAnimation(true);
    break;
  }
  case 2: { // green alarm clock panel (Page 2)
    if (clock_face_active) exit_clock_breathing();
    weatherAnimation(false);
    lv_obj_add_flag(ui_Info_Panel_Clock, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ui_Info_Panel_Weather, LV_OBJ_FLAG_HIDDEN);
    if (ui_Info_Panel_Alarm) {
      lv_obj_clear_flag(ui_Info_Panel_Alarm, LV_OBJ_FLAG_HIDDEN);
      alarm_ui_refresh();
    }
    break;
  }
  default: {
    break;
  }
  }
  SCREEN_OFF_TIMER = millis();
}

void changeInfoPanelPrev(lv_event_t *e) {
  if (infoPageIndex == 0)
    infoPageIndex = maxInfoPages - 1;
  else
    infoPageIndex--;
  updateInfoPanel(infoPageIndex);
}
void changeInfoPanelNext(lv_event_t *e) {
  infoPageIndex++;
  if (infoPageIndex >= maxInfoPages) infoPageIndex = 0;
  updateInfoPanel(infoPageIndex);
}
// show and hide arrow button on info panel
bool arrowVisible = false;
void hideArrow(lv_timer_t *timer) {
  log_d("hide arrow");
  lv_timer_del(timer);
  if (!arrowVisible) return;
  arrowVisible = false;
  lv_obj_add_flag(ui_Info_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_Info_Button_infoPanelRight, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_Info_Button_infoPanelLeft, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ui_Utility_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
}
void showArrow(lv_event_t *e) {
  log_d("show arrow");
  if (clock_face_active) {
    on_clock_touch_or_button();
  }
  if (arrowVisible) return;
  arrowVisible = true;
  lv_obj_clear_flag(ui_Info_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ui_Info_Button_infoPanelRight, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ui_Info_Button_infoPanelLeft, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(ui_Utility_Button_returnMenu, LV_OBJ_FLAG_HIDDEN);
  lv_create_delayed_task(hideArrow, 5000, NULL);
  SCREEN_OFF_TIMER = millis();
}

//------------ Weather & AQI pollution update ---------------
void updateAQIwidget(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  updateWeatherPanel();
}
// set query paramter to auto detect IP address
void set_query_para_autoip(lv_event_t *e) {
  strcpy(query_parameter, "auto:ip");
  SCREEN_OFF_TIMER = millis(); // reset timer
}
// number pad
void readNumpad(lv_event_t *e) {
  lv_obj_t *kb = lv_event_get_target(e);
  // Which button was pressed
  uint16_t btn_id = lv_btnmatrix_get_selected_btn(kb);
  const char *txt = lv_btnmatrix_get_btn_text(kb, btn_id);
  if (txt == NULL) return;
  if (strcmp(txt, LV_SYMBOL_OK) == 0) {
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN); // ✅ OK pressed → hide keyboard
  }
  SCREEN_OFF_TIMER = millis(); // reset timer
}
// set UTC offset hour
void setOffsetHour(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  lv_obj_t *widget = lv_event_get_target(e);
  offset_hour_index = lv_roller_get_selected(widget);
}
// set UTC offset minute
void setOffsetMinute(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  lv_obj_t *widget = lv_event_get_target(e);
  offset_minute_index = lv_roller_get_selected(widget);
}
// set temperature unit // 0 = celcius ,1 = farenheit
void setTempUnit(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  lv_obj_t *widget = lv_event_get_target(e);
  temp_unit = lv_roller_get_selected(widget);
}

// Utility -----------------------
void torch_ON(lv_event_t *e) {
  setUpduty(LCD_PWM_MODE_255);
  BL_OFF = false;
  SCREEN_OFF_TIMER = millis();
}
void torch_OFF(lv_event_t *e) {
  BL_OFF = false;
  setBrightness(NULL);
  SCREEN_OFF_TIMER = millis();
}
void showSystemInfo(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer

  // Left column: System & storage info
  static char infoText[200];
  systemInfo(infoText, sizeof(infoText));
  if (ui_Utility_Label_Build) {
    lv_label_set_text(ui_Utility_Label_Build, infoText);
  }

  // Hide old overlapping memory label to prevent text garbling
  if (ui_Utility_Label_Memory) {
    lv_obj_add_flag(ui_Utility_Label_Memory, LV_OBJ_FLAG_HIDDEN);
  }

  // Right column: Network, Uptime, DRAM, and PSRAM metrics
  if (ui_Utility_Label_Metrics) {
    static char metricsText[220];
    uint32_t up_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    uint32_t up_h = up_s / 3600;
    uint32_t up_m = (up_s % 3600) / 60;
    uint32_t up_sec = up_s % 60;

    size_t free_dram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t total_dram = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    size_t used_dram = (total_dram > free_dram) ? (total_dram - free_dram) : 0;
    uint8_t dram_pct = (total_dram > 0) ? (uint8_t)(used_dram * 100 / total_dram) : 0;

    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t used_psram = (total_psram > free_psram) ? (total_psram - free_psram) : 0;
    uint8_t psram_pct = (total_psram > 0) ? (uint8_t)(used_psram * 100 / total_psram) : 0;

    IPAddress ip = WiFi.localIP();
    int8_t rssi = (WiFi.status() == WL_CONNECTED) ? (int8_t)WiFi.RSSI() : 0;

    snprintf(metricsText, sizeof(metricsText),
             "Wi-Fi : %s (%d dBm)\n"
             "Uptime: %uh %um %us\n"
             "DRAM  : %uK free (%u%% used)\n"
             "PSRAM : %.1fM free (%u%% used)\n"
             "Audio : ES8311 (DAC) + ES7210 (Mic)",
             (WiFi.status() == WL_CONNECTED) ? ip.toString().c_str() : "Disconnected",
             (int)rssi,
             (unsigned)up_h, (unsigned)up_m, (unsigned)up_sec,
             (unsigned)(free_dram / 1024), dram_pct,
             (float)free_psram / (1024.0f * 1024.0f), psram_pct);
    lv_label_set_text(ui_Utility_Label_Metrics, metricsText);
  }
}

void ota_update(lv_event_t *e) {
  SCREEN_OFF_TIMER = millis(); // reset timer
  ota_show_popup();
}
