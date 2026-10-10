#include "battery/battery.h"
/**
 * @details
 *  - **Application Name:** TuneBar
 *  - **Developed By:** Va&Cob
 *
 *  **Software Stack**
 *  - PlatformIO pioArduino
 *  - ESP32 Core 3.3.3
 *  - LVGL 8.4.0
 *  - Display Framework: esp32_display_panel (including required dependencies)
 *  - ESP32 Audio I2S 3.4.4 https://github.com/schreibfaul1/ESP32-audioI2S
 *
 * Target Hardware
 *  - **Board Model:** Waveshare ESP32-S3-Touch-LCD-3.49
 *     (3.49″ IPS capacitive touch, 172 × 640 resolution, QSPI display interface)
 *     Reference: https://www.waveshare.com/product/arduino/boards-kits/esp32-s3/esp32-s3-touch-lcd-3.49.htm?sku=32374
 *
 *  - **Flash Size:** 16 MB
    - **Parition -> Custom 16M Flash (6MB APP/3.9MB SPIFFS)
 *  - **PSRAM:** OPI PSRAM (enabled)
 *  - **USB CDC on Boot:** Enabled
 * !!! IMPORTANT !!!  user must upload files in folder "data" to LittleFS parition
 *
 * This documentation block provides an authoritative configuration reference
 * to ensure alignment across development, testing, and system integration workflows.
 */
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "mbedtls/platform.h"

static void *mbedtls_psram_calloc(size_t n, size_t size) {
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void mbedtls_psram_free(void *ptr) {
    heap_caps_free(ptr);
}


#include "i2c_bsp/i2c_bsp.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "lvgl_port/lvgl_port.h"
#include "ui/ui.h"
#include "user_config.h"
#include <Arduino.h>
#include <stdio.h>
#include "ESP32-audioI2S-master/Audio.h" //https://github.com/schreibfaul1/ESP32-audioI2S
#include "es7210/es7210.h" // mic
#include "es8311/es8311.h" // audio codec
#include "file/file.h" // file system
#include "pcf85063/pcf85063.h" // real time clock
#include "tca9554/tca9554.h" // io expander
#include "weather/weather.h" // weather air quality widget
#include "network/network.h" // wifi network
#include "updater/updater.h"
#include "lan_stream/lan_stream.h"
//#include "qmi8658/qmi8658.h" // imu


extern Audio audio;
ES8311 speaker; // ES8322 (DAC)  →  I2S_NUM_0  (TX)
ES7210 mic; // ES7210 (ADC)  →  I2S_NUM_1  (RX)


// expander
extern i2c_master_dev_handle_t tca9554_dev_handle;
TCA9554 *io = nullptr;

#include "xtask.h" //all tasks

// rtos message que handle
QueueHandle_t ui_status_queue = NULL;
QueueHandle_t audio_cmd_queue = NULL;

static void createTaskChecked(TaskFunction_t task,
                              const char *name,
                              const uint32_t stackDepth,
                              UBaseType_t priority,
                              const BaseType_t core) {
  BaseType_t ok = xTaskCreatePinnedToCore(task, name, stackDepth, NULL, priority, NULL, core);
  if (ok != pdPASS) {
    log_e("Failed to create task: %s", name);
    assert(false);
  }
}

void serial_cli_task(void *param);
void serial_debug_beacon_task(void *param);
void web_server_task(void *param);

RTC_NOINIT_ATTR static uint32_t rtc_magic;
RTC_NOINIT_ATTR static uint32_t rtc_boot_count;
RTC_NOINIT_ATTR static uint32_t rtc_last_uptime_s;
RTC_NOINIT_ATTR static char rtc_last_action[48];
RTC_NOINIT_ATTR static esp_reset_reason_t rtc_last_reset_reason;

static const char* reset_reason_to_str(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:   return "Power-on / Cold Boot";
        case ESP_RST_EXT:       return "External Pin Reset";
        case ESP_RST_SW:        return "Software Reset (esp_restart)";
        case ESP_RST_PANIC:     return "Software Exception / Panic Crash";
        case ESP_RST_INT_WDT:   return "Interrupt Watchdog Reset";
        case ESP_RST_TASK_WDT:  return "Task Watchdog Reset";
        case ESP_RST_WDT:       return "Other Watchdog Reset";
        case ESP_RST_DEEPSLEEP: return "Deep-sleep Wake";
        case ESP_RST_BROWNOUT:  return "Brownout Reset (Voltage Dip)";
        case ESP_RST_SDIO:      return "SDIO Reset";
        default:                return "Unknown Reset";
    }
}

// ############################################################
void setup() {
  
  // message que init — depth 10 is ample (LVGL processes every 5ms)
  ui_status_queue = xQueueCreate(10, sizeof(UIStatusPayload));
  assert(ui_status_queue != NULL);
  audio_cmd_queue = xQueueCreate(10, sizeof(AudioCommandPayload));
  assert(audio_cmd_queue != NULL);

  randomSeed(esp_random());
  Serial.begin(115200);
  delay(100);
  mbedtls_platform_set_calloc_free(mbedtls_psram_calloc, mbedtls_psram_free);

  esp_reset_reason_t rst_reason = esp_reset_reason();
  if (rtc_magic != 0x54554E45) { // "TUNE"
    rtc_magic = 0x54554E45;
    rtc_boot_count = 1;
    rtc_last_uptime_s = 0;
    rtc_last_reset_reason = rst_reason;
    strncpy(rtc_last_action, "Cold Power-On", sizeof(rtc_last_action));
  } else {
    rtc_boot_count++;
    log_w("[REBOOT MONITOR] Boot #%u | Reset Cause: %d (%s) | Prior Uptime: %us | Prior Action: %s",
          (unsigned)rtc_boot_count, (int)rst_reason, reset_reason_to_str(rst_reason),
          (unsigned)rtc_last_uptime_s, rtc_last_action);
    rtc_last_reset_reason = rst_reason;
  }
  strncpy(rtc_last_action, "Booting", sizeof(rtc_last_action));

  log_i("[TuneBar] by Va&Cob | V%s - %s", current_version, compile_date);

  // input pin
  pinMode(BOOT, INPUT_PULLUP); // BOOT button
  pinMode(SYS_OUT, INPUT_PULLUP); // Check Power Button Pressed

  // setup ADC
  analogReadResolution(12); // 9–13 bits supported
  analogSetAttenuation(ADC_11db); // Full-scale ~3.3V

  i2c_master_Init(); // init i2c
  initLittleFS(); // Mount LittleFS early so /rec.wav and /wifi.json are always accessible

   // audio library
  Audio::audio_info_callback = my_audio_info;
  audio.setAudioTaskCore(1); // audio default run on core 1 (lvgl run on core 0 in lvgl_port.c)
  if (!audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DSOUT, I2S_MCLK, I2S_DSIN))
    log_e("Audio I2S pinout setup failed");
  audio.forceMono(true);
  audio.setInBufferSize(1024 * 1024); // 1MB Unified PSRAM Ring Buffer
  audio.setConnectionTimeout(2000, 4000); // connection timeout ms, ms_ssl
  // audio.setVolume(audio_volume);  // default 0...21

  // expander init
  io = new TCA9554(tca9554_dev_handle);
  bool io_ok = io->begin();
  if (!io_ok) log_e("Power and amplifier controls may not work");

  // UNCONDITIONALLY LATCH POWER ON (SYS_EN = 1) across V1 and V2 hardware
  io_ok &= io->setPinMode(EXIO1_BIT | EXIO6_BIT, 0); // set output mode
  io_ok &= io->digitalWrite(EXIO1_BIT | EXIO6_BIT, 1); // hold turn on
  log_i("Power hold latch (SYS_EN on EXIO1/EXIO6) -> ON");

  // init lvgl
  lvgl_port_init();
  initLittleFS(); // Register LVGL LittleFS driver ('L:') now that LVGL is initialized
  bsp_set_backlight_power(true); // Enable AP3032 boost rail
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255); // max out the brightness

  // power amp control (NS_MODE = 1) across V1 and V2 hardware
  bsp_set_audio_amp_power(true);
  log_i("Power Amp (NS_MODE on EXIO2/EXIO7) -> ON");

  // es8311 audio codec
  if (!speaker.begin()) {
    log_e("ES8311 begin failed");
  } else {
    speaker.setVolume(80); // 80 is best max
    log_i("ES8311 OK");
  }


 if (mic.init()) {
    log_i("ES7210 OK");
  } else {
    log_e("ES7210 FAILED to Initialize");
  }

  // Alarm engine init (Preferences NVS on main thread)
  alarm_init();
  lan_stream_init();

  // Free RTOS Tasks
  // Core 1 (Audio decoding & RTC/hardware application processing)
  createTaskChecked(audio_loop_task, "audio_loop", 14 * 1024, 6, 1);
  createTaskChecked(rtc_read_task, "getDateTimeTask", 3 * 1024, 3, 1);
  createTaskChecked(button_input_task, "buttonInputTask", 4 * 1024, 2, 1);
  createTaskChecked(batt_level_read_task, "readBatteryLevel", 2 * 1024, 1, 1);
  createTaskChecked(serial_debug_beacon_task, "debugBeacon", 4 * 1024, 1, 1);

  // Core 0 (System Core - lower priority than LVGL priority 5)
  createTaskChecked(serial_cli_task, "serial_cli", 6 * 1024, 2, 0);
  createTaskChecked(web_server_task, "web_server", 8 * 1024, 2, 0);

}

#include "telemetry/telemetry.h"
#include "web/web_remote.h"

void web_server_task(void *param) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      web_remote_init();
      web_remote_loop();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void serial_debug_beacon_task(void *param) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  telemetry_init();
  uint32_t last_telemetry_snap = millis();
  uint32_t last_telemetry_flush = millis();

  for (;;) {
    rtc_last_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    log_i("[BEACON] Heap: %u (min: %u) | PSRAM: %u | BL_OFF: %d | Audio: %s | WiFi: %s",
          (unsigned)free_heap, (unsigned)min_heap, (unsigned)free_psram, (int)BL_OFF,
          audio.isRunning() ? "PLAYING" : "IDLE",
          (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "OFFLINE");

    // Collect snapshot every 60s
    if (millis() - last_telemetry_snap >= 60000) {
      last_telemetry_snap = millis();
      telemetry_collect_snapshot();
    }

    // Flush batch every 5 minutes or when >= 5 records are buffered (only when idle)
    if ((millis() - last_telemetry_flush >= 300000 || telemetry_get_buffered_count() >= 5) &&
        (WiFi.status() == WL_CONNECTED) &&
        !ai_recording_active && !ai_upload_in_progress && !audio.isRunning()) {
      last_telemetry_flush = millis();
      telemetry_flush_batch();
    }

    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ############################################################
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
//---------------------------------------------------

#include <Preferences.h>
#include "alarm/alarm.h"
#include "web/web_remote.h"
extern Preferences pref;
// uses extern from lcd_bl_pwm_bsp.h
extern void updateInfoPanel(uint8_t pages);
extern void exit_clock_breathing(void);

void serial_cli_task(void *param) {
  vTaskDelay(pdMS_TO_TICKS(2000));
  log_i("[CLI] USB Serial CLI active. Type 'help' for commands.");
  String cmd = "";
  for (;;) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        cmd.trim();
        if (cmd.length() > 0) {
          log_i("[CLI CMD] '%s'", cmd.c_str());
          if (cmd.equalsIgnoreCase("ping")) {
            Serial.println("[CLI] PONG");
          } else if (cmd.equalsIgnoreCase("help")) {
            Serial.println("[CLI] Available Commands:");
            Serial.println("  ping                  - Connectivity test");
            Serial.println("  status                - Complete system status");
            Serial.println("  batt                  - Battery ADC mV, Voltage, and %");
            Serial.println("  telemetry             - Check buffered telemetry records");
            Serial.println("  telemetry flush       - Manually flush batch telemetry to server");
            Serial.println("  bl <0-255>            - Set raw backlight PWM brightness");
            Serial.println("  bl <low|med|high>     - Set & save brightness preset");
            Serial.println("  clock                 - Switch to Clock Face");
            Serial.println("  weather               - Switch to Weather Panel");
            Serial.println("  menu                  - Switch to Main Menu");
            Serial.println("  touch                 - Simulate touch / restart breathing");
            Serial.println("  alarm                 - Show alarm status and scheduled time");
            Serial.println("  alarm set <HH:MM>     - Set alarm time (24h format)");
            Serial.println("  alarm <on|off>        - Enable or disable alarm");
            Serial.println("  alarm stop            - Stop / dismiss active ringing alarm");
            Serial.println("  alarm test            - Immediately test alarm wake & audio");
            Serial.println("  vol <0-21>            - Set audio volume");
            Serial.println("  ask <text>            - Query AI Voice Assistant and stream audio");
            Serial.println("  wifi <on|off>         - Enable or disable WiFi");
            Serial.println("  reboot                - Restart ESP32-S3");
          } else if (cmd.equalsIgnoreCase("status") || cmd.equalsIgnoreCase("ip")) {
            float volt = 0.0f; uint8_t pct = 0;
            getBatteryStatus(&volt, &pct);
            Serial.printf("[CLI STATUS] Screen: %s | IP: %s | GW: %s | WiFi: %s | Batt: %.2fV (%d%%) | Heap: %u | PSRAM: %u\n",
                          lvgl_port_get_active_screen_name(),
                          WiFi.localIP().toString().c_str(), WiFi.gatewayIP().toString().c_str(),
                          (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "OFFLINE",
                          volt, pct, (unsigned int)ESP.getFreeHeap(), (unsigned int)ESP.getFreePsram());
          } else if (cmd.equalsIgnoreCase("screen") || cmd.equalsIgnoreCase("screen status")) {
            Serial.printf("[CLI SCREEN] Active: %s\n", lvgl_port_get_active_screen_name());
          } else if (cmd.equalsIgnoreCase("clock")) {
            log_i("[CLI] Switching to Clock Face...");
            screenPowerOn();
            if (lvgl_port_lock(500)) {
              infoPageIndex = 0;
              updateInfoPanel(0);
              _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 0, 0, &ui_Screen_Info_screen_init);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to Clock");
          } else if (cmd.equalsIgnoreCase("weather")) {
            log_i("[CLI] Switching to Weather Panel...");
            exit_clock_breathing();
            screenPowerOn();
            resetScreenOffTimer(NULL);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            if (lvgl_port_lock(500)) {
              infoPageIndex = 1;
              updateInfoPanel(1);
              _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 0, 0, &ui_Screen_Info_screen_init);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to Weather");
          } else if (cmd.equalsIgnoreCase("menu")) {
            log_i("[CLI] Switching to Main Menu...");
            exit_clock_breathing();
            screenPowerOn();
            resetScreenOffTimer(NULL);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            if (lvgl_port_lock(500)) {
              _ui_screen_change(&ui_Screen_MainMenu, LV_SCR_LOAD_ANIM_MOVE_TOP, 0, 0, &ui_Screen_MainMenu_screen_init);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to Menu");
          } else if (cmd.equalsIgnoreCase("alarm panel")) {
            log_i("[CLI] Switching to Green Alarm Panel...");
            exit_clock_breathing();
            screenPowerOn();
            resetScreenOffTimer(NULL);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            if (lvgl_port_lock(500)) {
              infoPageIndex = 2;
              updateInfoPanel(2);
              _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_MOVE_BOTTOM, 0, 0, &ui_Screen_Info_screen_init);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to Green Alarm Panel");
          } else if (cmd.equalsIgnoreCase("player") || cmd.equalsIgnoreCase("radio screen")) {
            log_i("[CLI] Switching to Radio Player Screen...");
            exit_clock_breathing();
            screenPowerOn();
            resetScreenOffTimer(NULL);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            if (lvgl_port_lock(500)) {
              _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
              livestreamMode(NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to Radio Player");
          } else if (cmd.equalsIgnoreCase("music") || cmd.equalsIgnoreCase("lan screen")) {
            log_i("[CLI] Switching to Music Player Screen...");
            exit_clock_breathing();
            screenPowerOn();
            resetScreenOffTimer(NULL);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            if (lvgl_port_lock(500)) {
              _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
              musicPlayerMode(NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to Music Player");
          } else if (cmd.equalsIgnoreCase("chat") || cmd.equalsIgnoreCase("assistant screen")) {
            log_i("[CLI] Switching to AI ChatBot Screen...");
            exit_clock_breathing();
            screenPowerOn();
            resetScreenOffTimer(NULL);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            if (lvgl_port_lock(500)) {
              _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
              chatBotMode(NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to AI ChatBot Screen");
          } else if (cmd.equalsIgnoreCase("utility") || cmd.equalsIgnoreCase("sysinfo")) {
            log_i("[CLI] Switching to Utility / System Info Screen...");
            exit_clock_breathing();
            screenPowerOn();
            resetScreenOffTimer(NULL);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            if (lvgl_port_lock(500)) {
              _ui_screen_change(&ui_Screen_Utility, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Utility_screen_init);
              if (ui_Utility_Panel_SystemInfo) {
                lv_obj_clear_flag(ui_Utility_Panel_SystemInfo, LV_OBJ_FLAG_HIDDEN);
              }
              showSystemInfo(NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to Utility / System Info Screen");
          } else if (cmd.equalsIgnoreCase("guide") || cmd.equalsIgnoreCase("user guide")) {
            log_i("[CLI] Switching to User Guide Screen...");
            exit_clock_breathing();
            screenPowerOn();
            resetScreenOffTimer(NULL);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            if (lvgl_port_lock(500)) {
              _ui_screen_change(&ui_Screen_MainMenu, LV_SCR_LOAD_ANIM_MOVE_TOP, 0, 0, &ui_Screen_MainMenu_screen_init);
              if (ui_MainMenu_Tabview_ConfigPanel) {
                lv_obj_clear_flag(ui_MainMenu_Tabview_ConfigPanel, LV_OBJ_FLAG_HIDDEN);
                lv_tabview_set_act(ui_MainMenu_Tabview_ConfigPanel, 5, LV_ANIM_OFF);
              }
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Switched to User Guide Tab");
          } else if (cmd.equalsIgnoreCase("lan") || cmd.equalsIgnoreCase("lan status")) {
            Serial.printf("[CLI LAN] Server: %s | Indexed Files: %d\n", lan_get_server(), lan_get_file_count());
            for (int i = 0; i < lan_get_file_count() && i < 10; i++) {
              const LanFileEntry *f = lan_get_file(i);
              if (f) Serial.printf("  [%d] %s -> %s\n", i + 1, f->name, f->url);
            }
          } else if (cmd.startsWith("lan server ") || cmd.startsWith("lan ip ")) {
            String srv = cmd.startsWith("lan server ") ? cmd.substring(11) : cmd.substring(7);
            srv.trim();
            lan_set_server(srv.c_str());
            Serial.printf("[CLI LAN] Server updated to: %s\n", lan_get_server());
          } else if (cmd.equalsIgnoreCase("lan fetch") || cmd.equalsIgnoreCase("lan scan")) {
            Serial.println("[CLI LAN] Fetching HTTP directory from LAN server...");
            int count = lan_fetch_files();
            Serial.printf("[CLI LAN] Fetched %d audio files from %s\n", count, lan_get_server());
          } else if (cmd.equalsIgnoreCase("crashlog") || cmd.equalsIgnoreCase("reboot log")) {
            Serial.printf("[CLI REBOOT LOG] Boot Count: %u | Last Reset Reason: %d (%s) | Prior Uptime: %us | Prior Action: %s\n",
                          (unsigned)rtc_boot_count, (int)rtc_last_reset_reason, reset_reason_to_str(rtc_last_reset_reason),
                          (unsigned)rtc_last_uptime_s, rtc_last_action);
          } else if (cmd.startsWith("lan play ")) {
            int idx = cmd.substring(9).toInt() - 1;
            strncpy(rtc_last_action, "LAN Play", sizeof(rtc_last_action));
            if (lan_play(idx)) {
              Serial.printf("[CLI LAN] Playing track %d\n", idx + 1);
            } else {
              Serial.printf("[CLI LAN] Failed to play track %d (valid range: 1..%d)\n", idx + 1, lan_get_file_count());
            }
          } else if (cmd.equalsIgnoreCase("batt")) {
            float volt = 0.0f; uint8_t pct = 0;
            getBatteryStatus(&volt, &pct);
            int raw = analogReadMilliVolts(ADC_BATT);
            Serial.printf("[CLI BATT] ADC: %d mV | Vbat: %.2fV | Percent: %d%%\n", raw, volt, pct);
          } else if (cmd.equalsIgnoreCase("telemetry")) {
            Serial.printf("[CLI TELEMETRY] Buffered snapshots: %d / %d | Flushing: %s\n",
                          (int)telemetry_get_buffered_count(), TELEMETRY_MAX_BUFFER,
                          telemetry_is_flushing() ? "YES" : "NO");
          } else if (cmd.equalsIgnoreCase("telemetry snap")) {
            telemetry_collect_snapshot();
            Serial.printf("[CLI TELEMETRY] Snapshot recorded. Total buffered: %d / %d\n",
                          (int)telemetry_get_buffered_count(), TELEMETRY_MAX_BUFFER);
          } else if (cmd.equalsIgnoreCase("telemetry flush")) {
            Serial.println("[CLI TELEMETRY] Triggering batch flush to telemetry server...");
            bool ok = telemetry_flush_batch();
            Serial.printf("[CLI TELEMETRY] Batch flush task: %s\n", ok ? "STARTED" : "FAILED / BUSY / OFFLINE");
          } else if (cmd.equalsIgnoreCase("radio")) {
            Serial.printf("[CLI RADIO] Active Site: %s | Stations: %d\n",
                          getRadioCatalogName(currentRadioCatalog), (int)stationListLength);
            for (uint8_t i = 0; i < stationListLength; i++) {
              Serial.printf("  [%d] %s -> %s\n", i + 1, stations[i].name, stations[i].url);
            }
          } else if (cmd.equalsIgnoreCase("radio onlineradiofm") || cmd.equalsIgnoreCase("radio 0")) {
            switchRadioCatalog(RADIO_CATALOG_ONLINE_RADIO_FM);
            Serial.printf("[CLI RADIO] Switched site to OnlineRadioFM.in (%d stations)\n", (int)stationListLength);
          } else if (cmd.equalsIgnoreCase("radio radioindia") || cmd.equalsIgnoreCase("radio 1")) {
            switchRadioCatalog(RADIO_CATALOG_RADIO_INDIA);
            Serial.printf("[CLI RADIO] Switched site to RadioIndia.in (%d stations)\n", (int)stationListLength);
          } else if (cmd.startsWith("radio lang ")) {
            String larg = cmd.substring(11);
            larg.trim();
            uint8_t lidx = RADIO_LANG_HI;
            if (larg.equalsIgnoreCase("en") || larg.equalsIgnoreCase("english") || larg == "1") lidx = RADIO_LANG_EN;
            else if (larg.equalsIgnoreCase("es") || larg.equalsIgnoreCase("spanish") || larg == "2") lidx = RADIO_LANG_ES;
            else if (larg.equalsIgnoreCase("cn") || larg.equalsIgnoreCase("chinese") || larg == "3") lidx = RADIO_LANG_CN;
            else if (larg.equalsIgnoreCase("de") || larg.equalsIgnoreCase("german") || larg == "4") lidx = RADIO_LANG_DE;
            else if (larg.equalsIgnoreCase("ja") || larg.equalsIgnoreCase("japanese") || larg == "5") lidx = RADIO_LANG_JA;
            else lidx = RADIO_LANG_HI;

            switchRadioLanguage(lidx);
            extern lv_obj_t *ui_MainMenu_Dropdown_RadioLang;
            if (ui_MainMenu_Dropdown_RadioLang && lvgl_port_lock(200)) {
              lv_dropdown_set_selected(ui_MainMenu_Dropdown_RadioLang, lidx);
              lvgl_port_unlock();
            }
            Serial.printf("[CLI RADIO] Language switched to %s (%s) [%d stations]\n",
                          getRadioLanguageCode(lidx), getRadioLanguageName(lidx), (int)stationListLength);
          } else if (cmd.startsWith("radio play ")) {
            int idx = cmd.substring(11).toInt() - 1;
            if (idx >= 0 && idx < stationListLength) {
              stationIndex = idx;
              strncpy(rtc_last_action, "Radio Play", sizeof(rtc_last_action));
              audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
              Serial.printf("[CLI RADIO] Playing [%d] %s\n", idx + 1, stations[idx].name);
            } else {
              Serial.println("[CLI RADIO] Invalid station index");
            }
          } else if (cmd.equalsIgnoreCase("touch") || cmd.equalsIgnoreCase("tap")) {
            exit_clock_breathing();
            screenPowerOn();
            bsp_set_audio_amp_power(true);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            extern void on_clock_touch_or_button(void);
            on_clock_touch_or_button();
            resetScreenOffTimer(NULL);
            extern void lvgl_port_inject_touch(int16_t x, int16_t y, uint32_t duration_ms);
            lvgl_port_inject_touch(320, 86, 150);
            Serial.println("[CLI] OK: Touch/Wake event triggered (center tap)");
          } else if (cmd.startsWith("touch ") || cmd.startsWith("tap ")) {
            String args = cmd.substring(cmd.indexOf(' ') + 1);
            args.trim();
            int space_idx = args.indexOf(' ');
            exit_clock_breathing();
            screenPowerOn();
            bsp_set_audio_amp_power(true);
            UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
            xQueueSend(ui_status_queue, &ui_msg, 100);
            extern void on_clock_touch_or_button(void);
            on_clock_touch_or_button();
            resetScreenOffTimer(NULL);
            extern void lvgl_port_inject_touch(int16_t x, int16_t y, uint32_t duration_ms);
            if (space_idx > 0) {
              int x = args.substring(0, space_idx).toInt();
              int y = args.substring(space_idx + 1).toInt();
              lvgl_port_inject_touch((int16_t)x, (int16_t)y, 150);
              Serial.printf("[CLI] OK: Screen tap injected at (%d, %d)\n", x, y);
            } else if (args.equalsIgnoreCase("radio")) {
              lvgl_port_inject_touch(230, 86, 150);
              Serial.println("[CLI] OK: Tapped Radio Card (230, 86)");
            } else if (args.equalsIgnoreCase("music") || args.equalsIgnoreCase("lan")) {
              lvgl_port_inject_touch(380, 86, 150);
              Serial.println("[CLI] OK: Tapped Music Card (380, 86)");
            } else if (args.equalsIgnoreCase("chat") || args.equalsIgnoreCase("ai") || args.equalsIgnoreCase("mic") || args.equalsIgnoreCase("assistant")) {
              extern void exit_clock_breathing(void);
              exit_clock_breathing();
              if (lvgl_port_lock(200)) {
                chatBotMode(NULL);
                _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_OVER_TOP, 300, 0, &ui_Screen_Player_screen_init);
                lvgl_port_unlock();
              }
              lvgl_port_inject_touch(530, 86, 150);
              Serial.println("[CLI] OK: Navigated to AI Assistant Screen & Injected Touch");
            } else if (args.equalsIgnoreCase("weather") || args.equalsIgnoreCase("clock")) {
              lvgl_port_inject_touch(80, 86, 150);
              Serial.println("[CLI] OK: Tapped Weather/Clock Card (80, 86)");
            } else if (args.equalsIgnoreCase("back") || args.equalsIgnoreCase("return")) {
              lvgl_port_inject_touch(35, 140, 150);
              if (ui_Player_Button_returnMenu && lvgl_port_lock(200)) {
                lv_event_send(ui_Player_Button_returnMenu, LV_EVENT_CLICKED, NULL);
                lvgl_port_unlock();
              }
              Serial.println("[CLI] OK: Tapped Back / Return Button");
            } else if (args.equalsIgnoreCase("play")) {
              lvgl_port_inject_touch(320, 140, 150);
              if (ui_Player_Button_play && lvgl_port_lock(200)) {
                lv_event_send(ui_Player_Button_play, LV_EVENT_CLICKED, NULL);
                lvgl_port_unlock();
              }
              Serial.println("[CLI] OK: Tapped Play Button");
            } else if (args.equalsIgnoreCase("next")) {
              lvgl_port_inject_touch(420, 140, 150);
              if (ui_Player_Button_next && lvgl_port_lock(200)) {
                lv_event_send(ui_Player_Button_next, LV_EVENT_CLICKED, NULL);
                lvgl_port_unlock();
              }
              Serial.println("[CLI] OK: Tapped Next Button");
            } else if (args.equalsIgnoreCase("prev")) {
              lvgl_port_inject_touch(220, 140, 150);
              if (ui_Player_Button_previous && lvgl_port_lock(200)) {
                lv_event_send(ui_Player_Button_previous, LV_EVENT_CLICKED, NULL);
                lvgl_port_unlock();
              }
              Serial.println("[CLI] OK: Tapped Previous Button");
            } else {
              int x = args.toInt();
              lvgl_port_inject_touch((int16_t)x, 86, 150);
              Serial.printf("[CLI] OK: Screen tap injected at (%d, 86)\n", x);
            }
          } else if (cmd.equalsIgnoreCase("chat") || cmd.equalsIgnoreCase("assistant")) {
            screenPowerOn();
            extern void exit_clock_breathing(void);
            exit_clock_breathing();
            if (lvgl_port_lock(200)) {
              chatBotMode(NULL);
              _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_OVER_TOP, 300, 0, &ui_Screen_Player_screen_init);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Navigated to AI Assistant screen");
          } else if (cmd.equalsIgnoreCase("swipe left") || cmd.equalsIgnoreCase("swipe l")) {
            screenPowerOn();
            lvgl_port_inject_swipe(450, 86, 100, 86, 300);
            Serial.println("[CLI] OK: Swiped left (450,86 -> 100,86)");
          } else if (cmd.equalsIgnoreCase("swipe right") || cmd.equalsIgnoreCase("swipe r")) {
            screenPowerOn();
            lvgl_port_inject_swipe(100, 86, 450, 86, 300);
            Serial.println("[CLI] OK: Swiped right (100,86 -> 450,86)");
          } else if (cmd.equalsIgnoreCase("swipe up") || cmd.equalsIgnoreCase("swipe u")) {
            screenPowerOn();
            extern void exit_clock_breathing(void);
            exit_clock_breathing();
            lvgl_port_inject_swipe(320, 150, 320, 20, 300);
            Serial.println("[CLI] OK: Swiped up (320,150 -> 320,20)");
          } else if (cmd.equalsIgnoreCase("swipe down") || cmd.equalsIgnoreCase("swipe d")) {
            screenPowerOn();
            lvgl_port_inject_swipe(320, 20, 320, 150, 300);
            Serial.println("[CLI] OK: Swiped down (320,20 -> 320,150)");
          } else if (cmd.startsWith("swipe ")) {
            String args = cmd.substring(6);
            args.trim();
            int x1, y1, x2, y2, dur = 300;
            int n = sscanf(args.c_str(), "%d %d %d %d %d", &x1, &y1, &x2, &y2, &dur);
            if (n >= 4) {
              screenPowerOn();
              lvgl_port_inject_swipe((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, (uint32_t)dur);
              Serial.printf("[CLI] OK: Swiped (%d,%d -> %d,%d, dur=%dms)\n", x1, y1, x2, y2, dur);
            } else {
              Serial.println("[CLI] Usage: swipe <x1> <y1> <x2> <y2> [duration_ms] OR swipe left/right/up/down");
            }
          } else if (cmd.equalsIgnoreCase("click play") || cmd.equalsIgnoreCase("click mic")) {
            screenPowerOn();
            if (ui_Player_Button_play && lvgl_port_lock(200)) {
              lv_event_send(ui_Player_Button_play, LV_EVENT_CLICKED, NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Clicked Play/MIC button");
          } else if (cmd.equalsIgnoreCase("rec") || cmd.equalsIgnoreCase("mic rec")) {
            log_i("[CLI] Starting 4-second microphone recording...");
            start_ai_voice_recording();
            Serial.println("[CLI] OK: Recording started");
          } else if (cmd.equalsIgnoreCase("rec stop") || cmd.equalsIgnoreCase("mic stop")) {
            log_i("[CLI] Stopping recording and starting playback...");
            stop_ai_voice_recording_and_process();
            Serial.println("[CLI] OK: Recording stopped & playback started");
          } else if (cmd.equalsIgnoreCase("play rec") || cmd.equalsIgnoreCase("rec play") || cmd.equalsIgnoreCase("loopback")) {
            log_i("[CLI] Playing /rec.wav...");
            mediaType = 2;
            speaker.setVolume(90);
            audio.setVolume(21);
            audio.connecttoFS(LittleFS, "/rec.wav");
          } else if (cmd.equalsIgnoreCase("screenshot") || cmd.equalsIgnoreCase("screen dump") || cmd.equalsIgnoreCase("snap")) {
            screenPowerOn();
            lvgl_port_take_screenshot();
            const uint16_t *fb = lvgl_port_get_framebuffer();
            if (!fb) {
              Serial.println("[SCREEN_ERROR] Framebuffer unavailable");
            } else {
              const uint32_t width = WAVESHARE_349_LCD_H_RES;
              const uint32_t height = WAVESHARE_349_LCD_V_RES;
              const uint32_t row_stride = ((width * 3 + 3) / 4) * 4;
              const uint32_t image_size = row_stride * height;
              const uint32_t file_size = 54 + image_size;

              Serial.printf("\n[SCREEN_START] %u %u %u\n", (unsigned)width, (unsigned)height, (unsigned)file_size);
              uint8_t bmp_hdr[54];
              memset(bmp_hdr, 0, sizeof(bmp_hdr));
              bmp_hdr[0] = 'B'; bmp_hdr[1] = 'M';
              bmp_hdr[2] = (uint8_t)(file_size);
              bmp_hdr[3] = (uint8_t)(file_size >> 8);
              bmp_hdr[4] = (uint8_t)(file_size >> 16);
              bmp_hdr[5] = (uint8_t)(file_size >> 24);
              bmp_hdr[10] = 54;
              bmp_hdr[14] = 40;
              bmp_hdr[18] = (uint8_t)(width);
              bmp_hdr[19] = (uint8_t)(width >> 8);
              bmp_hdr[20] = (uint8_t)(width >> 16);
              bmp_hdr[21] = (uint8_t)(width >> 24);
              bmp_hdr[22] = (uint8_t)(height);
              bmp_hdr[23] = (uint8_t)(height >> 8);
              bmp_hdr[24] = (uint8_t)(height >> 16);
              bmp_hdr[25] = (uint8_t)(height >> 24);
              bmp_hdr[26] = 1;
              bmp_hdr[28] = 24;
              bmp_hdr[34] = (uint8_t)(image_size);
              bmp_hdr[35] = (uint8_t)(image_size >> 8);
              bmp_hdr[36] = (uint8_t)(image_size >> 16);
              bmp_hdr[37] = (uint8_t)(image_size >> 24);

              static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
              // Stream header b64
              char hdr_b64[80];
              int hpos = 0;
              for (int i = 0; i < 54; i += 3) {
                uint32_t b0 = bmp_hdr[i];
                uint32_t b1 = (i + 1 < 54) ? bmp_hdr[i + 1] : 0;
                uint32_t b2 = (i + 2 < 54) ? bmp_hdr[i + 2] : 0;
                uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
                hdr_b64[hpos++] = b64_table[(triple >> 18) & 0x3F];
                hdr_b64[hpos++] = b64_table[(triple >> 12) & 0x3F];
                hdr_b64[hpos++] = (i + 1 < 54) ? b64_table[(triple >> 6) & 0x3F] : '=';
                hdr_b64[hpos++] = (i + 2 < 54) ? b64_table[triple & 0x3F] : '=';
              }
              hdr_b64[hpos] = '\0';
              Serial.print("[SCREEN_B64]:");
              Serial.println(hdr_b64);

              // Stream pixel rows
              uint8_t row_buf[row_stride];
              for (int y = (int)height - 1; y >= 0; y--) {
                memset(row_buf, 0, row_stride);
                const uint16_t *src_row = fb + (y * width);
                uint8_t *dst = row_buf;
                for (uint32_t x = 0; x < width; x++) {
                  uint16_t p = src_row[x];
                  uint8_t r = (p >> 11) & 0x1F;
                  uint8_t g = (p >> 5) & 0x3F;
                  uint8_t b = p & 0x1F;
                  *dst++ = (b * 255) / 31;
                  *dst++ = (g * 255) / 63;
                  *dst++ = (r * 255) / 31;
                }
                // Send row encoded as b64 chunks
                for (uint32_t offset = 0; offset < row_stride; ) {
                  uint32_t chunk_len = (row_stride - offset > 120) ? 120 : (row_stride - offset);
                  char line[170];
                  int lpos = 0;
                  for (uint32_t i = 0; i < chunk_len; i += 3) {
                    uint32_t b0 = row_buf[offset + i];
                    uint32_t b1 = (i + 1 < chunk_len) ? row_buf[offset + i + 1] : 0;
                    uint32_t b2 = (i + 2 < chunk_len) ? row_buf[offset + i + 2] : 0;
                    uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
                    line[lpos++] = b64_table[(triple >> 18) & 0x3F];
                    line[lpos++] = b64_table[(triple >> 12) & 0x3F];
                    line[lpos++] = (i + 1 < chunk_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
                    line[lpos++] = (i + 2 < chunk_len) ? b64_table[triple & 0x3F] : '=';
                  }
                  line[lpos] = '\0';
                  Serial.print("[SCREEN_B64]:");
                  Serial.println(line);
                  offset += chunk_len;
                  vTaskDelay(1);
                }
              }
              Serial.println("[SCREEN_END]");
            }
          } else if (cmd.equalsIgnoreCase("b64rec") || cmd.equalsIgnoreCase("b64dump")) {
            const uint8_t *src_buf = nullptr;
            size_t total_sz = 0;
            File f;
            if (ai_wav_buffer && ai_rec_pcm_bytes > 0) {
              src_buf = ai_wav_buffer;
              total_sz = 44 + ai_rec_pcm_bytes;
              log_i("[B64] Streaming directly from PSRAM audio buffer (%u bytes)", (unsigned)total_sz);
            } else {
              f = LittleFS.open("/rec.wav", "r");
              if (f) total_sz = f.size();
            }

            if (!src_buf && !f) {
              Serial.println("[B64_ERROR] No audio recorded yet in PSRAM or LittleFS");
            } else {
              Serial.printf("\n[B64_START] %u\n", (unsigned)total_sz);
              uint8_t raw_chunk[120];
              static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
              size_t offset = 0;
              while (offset < total_sz) {
                int bytes_read = 0;
                if (src_buf) {
                  bytes_read = (total_sz - offset > sizeof(raw_chunk)) ? sizeof(raw_chunk) : (total_sz - offset);
                  memcpy(raw_chunk, src_buf + offset, bytes_read);
                  offset += bytes_read;
                } else {
                  bytes_read = f.read(raw_chunk, sizeof(raw_chunk));
                  if (bytes_read <= 0) break;
                  offset += bytes_read;
                }
                char line[170];
                int line_pos = 0;
                for (int i = 0; i < bytes_read; i += 3) {
                  uint32_t b0 = raw_chunk[i];
                  uint32_t b1 = (i + 1 < bytes_read) ? raw_chunk[i + 1] : 0;
                  uint32_t b2 = (i + 2 < bytes_read) ? raw_chunk[i + 2] : 0;
                  uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
                  line[line_pos++] = b64_table[(triple >> 18) & 0x3F];
                  line[line_pos++] = b64_table[(triple >> 12) & 0x3F];
                  line[line_pos++] = (i + 1 < bytes_read) ? b64_table[(triple >> 6) & 0x3F] : '=';
                  line[line_pos++] = (i + 2 < bytes_read) ? b64_table[triple & 0x3F] : '=';
                }
                line[line_pos] = '\0';
                Serial.print("[B64]:");
                Serial.println(line);
                vTaskDelay(1);
              }
              if (f) f.close();
              Serial.println("[B64_END]");
            }
          } else if (cmd.equalsIgnoreCase("click back") || cmd.equalsIgnoreCase("click return")) {
            screenPowerOn();
            if (ui_Player_Button_returnMenu && lvgl_port_lock(200)) {
              lv_event_send(ui_Player_Button_returnMenu, LV_EVENT_CLICKED, NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Clicked Return to Menu button");
          } else if (cmd.equalsIgnoreCase("click loop") || cmd.equalsIgnoreCase("click mode")) {
            screenPowerOn();
            if (ui_Player_Button_RndRPT && lvgl_port_lock(200)) {
              lv_event_send(ui_Player_Button_RndRPT, LV_EVENT_CLICKED, NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Clicked Loop/Mode button");
          } else if (cmd.equalsIgnoreCase("click next")) {
            screenPowerOn();
            if (ui_Player_Button_next && lvgl_port_lock(200)) {
              lv_event_send(ui_Player_Button_next, LV_EVENT_CLICKED, NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Clicked Next button");
          } else if (cmd.equalsIgnoreCase("click prev")) {
            screenPowerOn();
            if (ui_Player_Button_previous && lvgl_port_lock(200)) {
              lv_event_send(ui_Player_Button_previous, LV_EVENT_CLICKED, NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Clicked Previous button");
          } else if (cmd.equalsIgnoreCase("click cat") || cmd.equalsIgnoreCase("click catalog")) {
            screenPowerOn();
            if (ui_Player_Button_Catalog && lvgl_port_lock(200)) {
              lv_event_send(ui_Player_Button_Catalog, LV_EVENT_CLICKED, NULL);
              lvgl_port_unlock();
            }
            Serial.println("[CLI] OK: Clicked Catalog button");
          } else if (cmd.startsWith("bl ")) {
            String arg = cmd.substring(3);
            arg.trim();
            screenPowerOn();
            if (arg.equalsIgnoreCase("low") || arg == "0") {
              backlight_state = 0;
              pref.begin("config", false); pref.putUChar("bl_state", 0); pref.end();
              lv_dropdown_set_selected(ui_MainMenu_Dropdown_Brightness, 0);
              setUpduty(LCD_BL_LOW);
              Serial.println("[CLI] OK: Brightness set to LOW (120)");
            } else if (arg.equalsIgnoreCase("med") || arg.equalsIgnoreCase("medium") || arg == "1") {
              backlight_state = 1;
              pref.begin("config", false); pref.putUChar("bl_state", 1); pref.end();
              lv_dropdown_set_selected(ui_MainMenu_Dropdown_Brightness, 1);
              setUpduty(LCD_BL_MED);
              Serial.println("[CLI] OK: Brightness set to MED (170)");
            } else if (arg.equalsIgnoreCase("high") || arg == "2") {
              backlight_state = 2;
              pref.begin("config", false); pref.putUChar("bl_state", 2); pref.end();
              lv_dropdown_set_selected(ui_MainMenu_Dropdown_Brightness, 2);
              setUpduty(LCD_BL_HIGH);
              Serial.println("[CLI] OK: Brightness set to HIGH (255)");
            } else {
              int raw_val = arg.toInt();
              if (raw_val < 0) raw_val = 0;
              if (raw_val > 255) raw_val = 255;
              setUpduty(255 - raw_val);
              Serial.printf("[CLI] OK: Raw brightness PWM set to %d (duty %d)\n", raw_val, 255 - raw_val);
            }
          } else if (cmd.startsWith("vol ")) {
            int v = cmd.substring(4).toInt();
            if (v < 0) v = 0;
            if (v > 21) v = 21;
            audioSetVolume(v);
            Serial.printf("[CLI] OK: Volume set to %d\n", v);
          } else if (cmd.equalsIgnoreCase("wifi on")) {
            wifiEnable = true;
            WiFi.mode(WIFI_STA);
            WiFi.begin();
            Serial.println("[CLI] OK: WiFi enabled");
          } else if (cmd.equalsIgnoreCase("wifi off")) {
            wifiEnable = false;
            WiFi.disconnect(false);
            WiFi.mode(WIFI_OFF);
            Serial.println("[CLI] OK: WiFi disabled");
          } else if (cmd.equalsIgnoreCase("reboot")) {
            Serial.println("[CLI] OK: Rebooting ESP32-S3...");
            delay(100);
            ESP.restart();
          } else if (cmd.equalsIgnoreCase("alarm")) {
            uint8_t a_hr = 0, a_min = 0;
            alarm_get_time(&a_hr, &a_min);
            Serial.printf("[CLI ALARM] State: %s | Time: %02d:%02d | Ringing: %s\n",
                          alarm_is_enabled() ? "ENABLED" : "DISABLED",
                          a_hr, a_min,
                          alarm_is_active() ? "YES" : "NO");
          } else if (cmd.equalsIgnoreCase("alarm on") || cmd.equalsIgnoreCase("alarm enable")) {
            alarm_set_enabled(true);
            uint8_t a_hr = 0, a_min = 0;
            alarm_get_time(&a_hr, &a_min);
            Serial.printf("[CLI ALARM] Alarm ENABLED for %02d:%02d\n", a_hr, a_min);
          } else if (cmd.equalsIgnoreCase("alarm off") || cmd.equalsIgnoreCase("alarm disable")) {
            alarm_set_enabled(false);
            Serial.println("[CLI ALARM] Alarm DISABLED");
          } else if (cmd.startsWith("alarm set ")) {
            String time_str = cmd.substring(10);
            time_str.trim();
            int colon_idx = time_str.indexOf(':');
            if (colon_idx > 0) {
              int hr = time_str.substring(0, colon_idx).toInt();
              int mn = time_str.substring(colon_idx + 1).toInt();
              alarm_set_time((uint8_t)hr, (uint8_t)mn);
              alarm_set_enabled(true);
              Serial.printf("[CLI ALARM] Alarm SET to %02d:%02d and ENABLED\n", hr, mn);
            } else {
              Serial.println("[CLI ALARM] Invalid format. Use: alarm set HH:MM (e.g. alarm set 07:30)");
            }
          } else if (cmd.equalsIgnoreCase("alarm stop") || cmd.equalsIgnoreCase("alarm snooze")) {
            alarm_stop();
            Serial.println("[CLI ALARM] Alarm stopped / dismissed");
          } else if (cmd.equalsIgnoreCase("alarm test")) {
            Serial.println("[CLI ALARM] Triggering alarm test sequence...");
            UIStatusPayload msg = { .type = STATUS_ALARM_TRIGGER };
            xQueueSend(ui_status_queue, &msg, 100);
            Serial.println("[CLI ALARM] Alarm sequence triggered! (Type 'alarm stop' to dismiss)");
          } else if (cmd.equalsIgnoreCase("web") || cmd.equalsIgnoreCase("ip")) {
            Serial.printf("[CLI WEB] Web Remote URL: http://%s/\n",
                          (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString().c_str() : "OFFLINE (Connect WiFi first)");
          } else if (cmd.equalsIgnoreCase("mic") || cmd.equalsIgnoreCase("listen")) {
            screenPowerOn();
            if (mediaType != 2) {
              if (lvgl_port_lock(200)) {
                chatBotMode(NULL);
                lvgl_port_unlock();
              }
            }
            start_ai_voice_recording();
            Serial.println("[CLI] OK: AI Voice Recording started from ES7210 microphone");
          } else if (cmd.startsWith("ask ")) {
            String q = cmd.substring(4);
            q.trim();
            mediaType = 2; // AI Assistant mode
            UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
            snprintf(p.trackDesc, sizeof(p.trackDesc), "You asked:\n%s\n\nAI: Streaming response...", q.c_str());
            xQueueSend(ui_status_queue, &p, 100);
            String enc = q;
            enc.replace(" ", "+");
            String url = String(AI_ASSISTANT_URL) + "?q=" + enc;
            audioPlayHOST(url.c_str(), "AI Assistant");
            Serial.printf("[CLI ASSISTANT] Query: '%s' -> Streaming AI speech audio response (HTTPS)...\n", q.c_str());
          } else if (cmd.equalsIgnoreCase("heap") || cmd.equalsIgnoreCase("ram")) {
            size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
            size_t dram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
            size_t dram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
            size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
            size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
            
            Serial.println("\n========== MEMORY BREAKDOWN ==========");
            Serial.printf("  Internal DRAM Free   : %6u KB (Min: %u KB, Largest Block: %u KB)\n",
                          (unsigned)(dram_free / 1024), (unsigned)(dram_min / 1024), (unsigned)(dram_largest / 1024));
            Serial.printf("  DMA-Capable DRAM Free: %6u KB\n", (unsigned)(dma_free / 1024));
            Serial.printf("  External PSRAM Free  : %6u KB (Largest Block: %u KB)\n",
                          (unsigned)(psram_free / 1024), (unsigned)(psram_largest / 1024));
            Serial.printf("  Total Usable DRAM    : %6u / 318 KB (%u%% used)\n",
                          (unsigned)((318 * 1024 - dram_free) / 1024),
                          (unsigned)((318 * 1024 - dram_free) * 100 / (318 * 1024)));
            Serial.println("======================================\n");
          } else {
            Serial.printf("[CLI] Unknown command: '%s' (type 'help')\n", cmd.c_str());
          }
          cmd = "";
        }
      } else {
        cmd += c;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
