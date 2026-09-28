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
#include "mbedtls/platform.h"


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

void serial_debug_beacon_task(void *param);

// ############################################################
void setup() {
  
  // message que init
  ui_status_queue = xQueueCreate(20, sizeof(UIStatusPayload));
  assert(ui_status_queue != NULL);
  audio_cmd_queue = xQueueCreate(20, sizeof(AudioCommandPayload));
  assert(audio_cmd_queue != NULL);

  randomSeed(esp_random());
  Serial.begin(115200);
  delay(100);
  log_i("[TuneBar] by Va&Cob | V%s - %s", current_version, compile_date);

  // input pin
  pinMode(BOOT, INPUT_PULLUP); // BOOT button
  pinMode(SYS_OUT, INPUT_PULLUP); // Check Power Button Pressed

  // setup ADC
  analogReadResolution(12); // 9–13 bits supported
  analogSetAttenuation(ADC_11db); // Full-scale ~3.3V

  i2c_master_Init(); // init i2c

   // audio library
  Audio::audio_info_callback = my_audio_info;
  audio.setAudioTaskCore(1); // audio default run on core 1 (lvgl run on core 0 in lvgl_port.c)
  if (!audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DSOUT, I2S_MCLK, I2S_DSIN))
    log_e("Audio I2S pinout setup failed");
  audio.forceMono(true);
  audio.setConnectionTimeout(2000, 4000); // connection timeout ms, ms_ssl
  // audio.setVolume(audio_volume);  // default 0...21

  // exapnder init
  io = new TCA9554(tca9554_dev_handle);
  bool io_ok = io->begin();
  if (!io_ok) log_e("Power and amplifier controls may not work");
  io_ok &= io->setPinMode(EXIO6_BIT, 0); // set output mode

  // turn on power button
  if (digitalRead(SYS_OUT) == LOW) {
    log_d("< POWER ON >");
    io_ok &= io->digitalWrite(EXIO6_BIT, 1); // hold turn on
  }

  // init lvgl
  lvgl_port_init();
  bsp_set_backlight_power(true); // Enable AP3032 boost rail on EXIO1
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255); // max out the brightness

  // power amp control
  io_ok &= io->setPinMode(EXIO7_BIT, 0); // 0 = OUTPUT
  delay(100);
  io_ok &= io->digitalWrite(EXIO7_BIT, 1); // enable amp
  delay(100);
  if (!io_ok || io->digitalRead(EXIO7_BIT) == 0)
    log_e("Power Amp not turn on!");
  else
    log_i("Power Amp -> ON");

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

  // Free RTOS Task
  createTaskChecked(audio_loop_task, "audio_loop", 6 * 1024, 4, 1);
  createTaskChecked(rtc_read_task, "getDateTimeTask", 3 * 1024, 3, 1);
  createTaskChecked(button_input_task, "buttonInputTask", 2 * 1024, 2, 1);
  createTaskChecked(batt_level_read_task, "readBatteryLevel", 2 * 1024, 1, 1);
  //xTaskCreatePinnedToCore(imu_read_task, "imu_read_task", 2 * 1024, NULL , 1, NULL,1);
  createTaskChecked(serial_debug_beacon_task, "debugBeacon", 2 * 1024, 1, 1);

}

void serial_debug_beacon_task(void *param) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  for (;;) {
    size_t free_heap = esp_get_free_heap_size();
    size_t min_heap = esp_get_minimum_free_heap_size();
    size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    log_i("[BEACON] Heap: %u (min: %u) | PSRAM: %u | BL_OFF: %d | Audio: %s | WiFi: %s",
          (unsigned)free_heap, (unsigned)min_heap, (unsigned)free_psram, (int)BL_OFF,
          audio.isRunning() ? "PLAYING" : "IDLE",
          (WiFi.status() == WL_CONNECTED) ? "CONNECTED" : "OFFLINE");
    vTaskDelay(pdMS_TO_TICKS(2000));
  }
}

// ############################################################
void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
//---------------------------------------------------
