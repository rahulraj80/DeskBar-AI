/* For RTOS Task function.
All task setting  Stack size and Priority and Core

CORE 1:
  m_audioTaskHandle = xTaskCreateStaticPinnedToCore(
        &Audio::taskWrapper, "PeriodicTask", 3300, this, 6, xAudioStack,  &xAudioTaskBuffer, 1);
  xTaskCreatePinnedToCore(audio_loop_task, "audio_loop", 6 * 1024, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(rtc_read_task, "getDateTimeTask", 3 * 1024, NULL, 3, NULL, 1);
  xTaskCreatePinnedToCore(button_input_task, "buttonInputTask", 2 * 1024, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(batt_level_read_task, "readBatteryLevel", 2 * 1024, NULL, 1, NULL, 1);
  

  xTaskCreatePinnedToCore(scan_music_task, "SD_Scan_Task", 6 * 1024, NULL, 1, NULL, 1);(create and delete)
  xTaskCreatePinnedToCore(updateWeatherPanelTask,"To update weather panel", 2 * 1024, NULL,3, NULL, 1);(create and delete)
  xTaskCreatePinnedToCore(ota_task, "ota_task", 10 * 1024, NULL, 4, &otaTaskHandle, 1);(create and delete)

CORE 0:
  xTaskCreatePinnedToCore(WAVESHARE_349_lvgl_port_task, "LVGL", 6 * 1024, NULL, 5, NULL, 0); // Run Core 0
  xTaskCreatePinnedToCore(wifi_connect_task, "wifi_connect_task", 6 * 1024, NULL, 1, &wifiTask, 0);(create and delete)
*/
#include "file/file.h"
#include "task_msg/task_msg.h"
#include <LittleFS.h>
#include <WiFi.h>
#include "esp_wifi.h"
#include "alarm/alarm.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

//==============================================
// BUTTON INPUT TASK:
void button_input_task(void *param) {
  vTaskDelay(pdMS_TO_TICKS(3000));
  bool powerBTN_pressed = false;
  long hold_timer = 0;
  static UIStatusPayload ui_msg = {};
  for (;;) {
    // turn off power button
    if (digitalRead(SYS_OUT) == LOW) {
      if (!powerBTN_pressed) {
        powerBTN_pressed = true;
        hold_timer = millis();
      } else if (millis() - hold_timer > 2000) {
        hold_timer = millis();
        audioSetVolume(20);
        audioPlayFS(1, "/audio/off.mp3");
        log_i("< POWER OFF TRIGGERED >");
        vTaskDelay(pdMS_TO_TICKS(1000));
        io->digitalWrite(EXIO6_BIT, 0); // turn off power latch
      }
    } else if (powerBTN_pressed)
      powerBTN_pressed = false;

    //-----------------------
    // screen on button (edge-triggered + debounce)
    static bool boot_btn_prev = HIGH;
    bool boot_btn_curr = digitalRead(BOOT);
    if (boot_btn_prev == HIGH && boot_btn_curr == LOW) {
      vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
      if (digitalRead(BOOT) == LOW) {
        if (alarm_is_active()) {
          log_i("Button: Alarm dismissed!");
          alarm_stop();
          while (digitalRead(BOOT) == LOW) {
            vTaskDelay(pdMS_TO_TICKS(20));
          }
          boot_btn_prev = boot_btn_curr;
          continue;
        }
        if (clock_face_active) {
          if (BL_OFF) {
            screenPowerOn();
            bsp_set_audio_amp_power(true);
            ui_msg.type = STATUS_SCREEN_UNLOCK;
            xQueueSend(ui_status_queue, &ui_msg, 100);
          }
          extern void on_clock_touch_or_button(void);
          on_clock_touch_or_button();
        } else {
          if (BL_OFF) {
            log_i("Button: Unlock Screen");
            screenPowerOn();
            bsp_set_audio_amp_power(true);
            ui_msg.type = STATUS_SCREEN_UNLOCK;
            xQueueSend(ui_status_queue, &ui_msg, 100);
          } else {
            log_i("Button: Lock Screen");
            ui_msg.type = STATUS_SCREEN_LOCK;
            xQueueSend(ui_status_queue, &ui_msg, 100);
            screenPowerOff();
            if (!audio.isRunning()) {
              bsp_set_audio_amp_power(false);
            }
          }
        }
        while (digitalRead(BOOT) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(20));
        }
      }
    }
    boot_btn_prev = boot_btn_curr;
    //-----------------------
    uint32_t now = millis();
    uint32_t start = SCREEN_OFF_TIMER;
    uint32_t delay = SCREEN_OFF_DELAY;

    if (delay != 0 && !BL_OFF && !clock_face_active) {
      if ((now - start) >= delay) {
        log_d("< Lock Screen >");
        ui_msg.type = STATUS_SCREEN_LOCK;
        xQueueSend(ui_status_queue, &ui_msg, 100);

        screenPowerOff();
        if (!audio.isRunning()) {
          bsp_set_audio_amp_power(false);
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10)); // yield CPU
  } // for {;;}
}

//==============================================
// TASK: Battery Monitoring - run on Core 1
void batt_level_read_task(void *param) {
  static uint8_t last_state = 0;
  uint8_t bat_state = 0;
  vTaskDelay(pdMS_TO_TICKS(5000)); // delay to wait lvgl ready
  for (;;) {

    int rawValue = analogReadMilliVolts(ADC_BATT);
    float vbat = rawValue * 3.0f / 1000.0f;

    if (vbat >= 4.10)
      bat_state = 4;
    else if (vbat >= 3.95)
      bat_state = 3;
    else if (vbat >= 3.80)
      bat_state = 2;
    else if (vbat >= 3.60)
      bat_state = 1;
    else
      bat_state = 0;

    log_d("ADC: %d | Vbat: %.2fV | State: %d,%d", rawValue, vbat, bat_state, last_state);
    if (bat_state != last_state) {
      last_state = bat_state;
      UIStatusPayload msg = {// prepare mesasge
                             .type = STATUS_UPDATE_BATTERY_LEVEL,
                             .battery_state = bat_state};
      xQueueSend(ui_status_queue, &msg, 100); // send message
    }
    vTaskDelay(pdMS_TO_TICKS(10000));
  } // if{;;}

  // ... (HWM Log เดิม)
  UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
  log_d("{ Task stack remaining MIN: %u bytes }", hwm);
}

//==============================================

// TASK: Real Time Clock
extern PCF85063 rtc;
TickType_t lastWake = xTaskGetTickCount();
void rtc_read_task(void *param) {
  rtc.begin();
  for (;;) {
    // read RTC
    if (rtc.getDateTime()) {
      UIStatusPayload msg = {
         .type = STATUS_UPDATE_CLOCK, 
         .hour = now.hour, 
         .minute = now.minute, 
         .second = now.second, 
         .year = now.year, 
         .month = now.month, 
         .dayOfMonth = now.dayOfMonth, 
         .dayOfWeek = now.dayOfWeek};
      xQueueSend(ui_status_queue, &msg, 100); // send message

      alarm_check(now.hour, now.minute, now.second);
    } else {
      log_w("RTC Error");
    }
    // UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    // log_d("{ Task stack remaining MIN: %u bytes }", hwm);
    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(1000)); // 1 second update
  }
}

//==============================================

// TASK: Acceleroometer + Gyro

























// ========================================================================
// TASK: AUDIO

// Helper function convert track time second -> hh:mm:ss
void timeStr(char *buffer, size_t size, uint32_t second) {
  uint8_t h = second / 3600;
  uint8_t m = (second % 3600) / 60;
  uint8_t s = second % 60;
  if (h > 0) {
    snprintf(buffer, size, "%d:%02d:%02d", h, m, s);
  } else {
    snprintf(buffer, size, "%d:%02d", m, s);
  }
}


// ========================================================================
// AI VOICE ASSISTANT PIPELINE (ES7210 ADC -> PSRAM WAV -> HTTPS -> ES8311 DAC)
// ========================================================================
static const size_t AI_REC_SAMPLE_RATE = 8000;
static const size_t AI_REC_DURATION_SEC = 4;
static const size_t AI_REC_PCM_MAX = AI_REC_SAMPLE_RATE * 2 * AI_REC_DURATION_SEC; // 64,000 bytes
static const size_t AI_REC_TOTAL_BUF_SIZE = 44 + AI_REC_PCM_MAX; // 64,044 bytes

static uint8_t *ai_wav_buffer = nullptr;
static volatile size_t ai_rec_pcm_bytes = 0;
volatile bool ai_recording_active = false;
volatile bool ai_upload_in_progress = false;
static uint32_t ai_recording_start_ms = 0;
static TaskHandle_t ai_upload_task_handle = NULL;

static void write_wav_header(uint8_t *header, uint32_t pcm_data_size, uint32_t sample_rate, uint16_t num_channels, uint16_t bits_per_sample) {
  uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
  uint16_t block_align = num_channels * (bits_per_sample / 8);
  uint32_t total_chunk_size = 36 + pcm_data_size;

  header[0] = 'R'; header[1] = 'I'; header[2] = 'F'; header[3] = 'F';
  header[4] = (uint8_t)(total_chunk_size & 0xFF);
  header[5] = (uint8_t)((total_chunk_size >> 8) & 0xFF);
  header[6] = (uint8_t)((total_chunk_size >> 16) & 0xFF);
  header[7] = (uint8_t)((total_chunk_size >> 24) & 0xFF);
  header[8] = 'W'; header[9] = 'A'; header[10] = 'V'; header[11] = 'E';

  header[12] = 'f'; header[13] = 'm'; header[14] = 't'; header[15] = ' ';
  header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0; // Subchunk1Size = 16 for PCM
  header[20] = 1; header[21] = 0; // AudioFormat = 1 (PCM)
  header[22] = (uint8_t)(num_channels & 0xFF);
  header[23] = (uint8_t)((num_channels >> 8) & 0xFF);
  header[24] = (uint8_t)(sample_rate & 0xFF);
  header[25] = (uint8_t)((sample_rate >> 8) & 0xFF);
  header[26] = (uint8_t)((sample_rate >> 16) & 0xFF);
  header[27] = (uint8_t)((sample_rate >> 24) & 0xFF);
  header[28] = (uint8_t)(byte_rate & 0xFF);
  header[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
  header[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
  header[31] = (uint8_t)((byte_rate >> 24) & 0xFF);
  header[32] = (uint8_t)(block_align & 0xFF);
  header[33] = (uint8_t)((block_align >> 8) & 0xFF);
  header[34] = (uint8_t)(bits_per_sample & 0xFF);
  header[35] = (uint8_t)((bits_per_sample >> 8) & 0xFF);

  header[36] = 'd'; header[37] = 'a'; header[38] = 't'; header[39] = 'a';
  header[40] = (uint8_t)(pcm_data_size & 0xFF);
  header[41] = (uint8_t)((pcm_data_size >> 8) & 0xFF);
  header[42] = (uint8_t)((pcm_data_size >> 16) & 0xFF);
  header[43] = (uint8_t)((pcm_data_size >> 24) & 0xFF);
}

void ai_upload_task(void *param) {
  ai_upload_in_progress = true;

  // 1. Wait for repeat playback of heard audio to complete so user hears feedback on recording quality
  uint32_t wait_start = millis();
  while (audio.isRunning() && (millis() - wait_start < 10000)) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  vTaskDelay(pdMS_TO_TICKS(250)); // Short natural breather

  size_t wav_len = 44 + ai_rec_pcm_bytes;
  log_i("[AI UPLOAD] Feedback playback finished. Posting %u bytes WAV to https://autobots.my.to/ask.php...", (unsigned)wav_len);

  UIStatusPayload p_proc = {.type = STATUS_UPDATE_AI_THINKING};
  xQueueSend(ui_status_queue, &p_proc, 100);

  UIStatusPayload p_desc = {.type = STATUS_UPDATE_TRACK_DESC_SET};
  snprintf(p_desc.trackDesc, sizeof(p_desc.trackDesc), "AI Assistant:\nAnalyzing your voice query...");
  xQueueSend(ui_status_queue, &p_desc, 100);

  if (WiFi.status() != WL_CONNECTED) {
    log_e("[AI UPLOAD] WiFi offline, cannot upload");
    UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
    snprintf(p.trackDesc, sizeof(p.trackDesc), "AI Assistant Error:\nWiFi offline. Please connect WiFi.");
    xQueueSend(ui_status_queue, &p, 100);
    UIStatusPayload p2 = {.type = STATUS_UPDATE_AI_IDLE};
    xQueueSend(ui_status_queue, &p2, 100);
    ai_upload_in_progress = false;
    vTaskDelete(NULL);
    return;
  }

  {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    if (!http.begin(client, "https://autobots.my.to/ask.php")) {
      log_e("[AI UPLOAD] HTTP client begin failed");
      UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
      snprintf(p.trackDesc, sizeof(p.trackDesc), "AI Assistant Error:\nCannot connect to server.");
      xQueueSend(ui_status_queue, &p, 100);
      UIStatusPayload p2 = {.type = STATUS_UPDATE_AI_IDLE};
      xQueueSend(ui_status_queue, &p2, 100);
    } else {
      http.addHeader("Content-Type", "audio/wav");
      http.addHeader("User-Agent", "ESP32-TuneBar");
      http.setTimeout(15000);

      int httpCode = http.POST(ai_wav_buffer, wav_len);
      log_i("[AI UPLOAD] HTTP POST returned code: %d", httpCode);

      if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        log_i("[AI UPLOAD] JSON payload (%d bytes): %s", (int)payload.length(), payload.c_str());

        JsonDocument doc;
        DeserializationError dErr = deserializeJson(doc, payload);
        if (!dErr) {
          const char *question = doc["question"] | "(Audio Query)";
          const char *answer = doc["answer"] | "Processing complete.";
          const char *audio_url = doc["audio_url"] | "";

          UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
          snprintf(p.trackDesc, sizeof(p.trackDesc), "You: %s\n\nAI: %s", question, answer);
          xQueueSend(ui_status_queue, &p, 100);

          UIStatusPayload p_spk = {.type = STATUS_UPDATE_AI_SPEAKING};
          xQueueSend(ui_status_queue, &p_spk, 100);

          if (audio_url && strlen(audio_url) > 0) {
            log_i("[AI UPLOAD] Streaming answer audio from: %s", audio_url);
            bsp_set_audio_amp_power(true);
            speaker.setVolume(90);
            audio.setVolume(21);
            resetScreenOffTimer(NULL);
            mediaType = 2; // AI Assistant mode
            audioPlayHOST(audio_url, "");
          } else {
            UIStatusPayload p_idle = {.type = STATUS_UPDATE_AI_IDLE};
            xQueueSend(ui_status_queue, &p_idle, 100);
          }
        } else {
          log_e("[AI UPLOAD] JSON parse failed: %s", dErr.c_str());
          UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
          snprintf(p.trackDesc, sizeof(p.trackDesc), "AI Assistant Error:\nBad server response.");
          xQueueSend(ui_status_queue, &p, 100);
          UIStatusPayload p_idle = {.type = STATUS_UPDATE_AI_IDLE};
          xQueueSend(ui_status_queue, &p_idle, 100);
        }
      } else {
        log_e("[AI UPLOAD] POST failed, HTTP error: %d", httpCode);
        UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
        snprintf(p.trackDesc, sizeof(p.trackDesc), "AI Assistant Error (HTTP %d).\nTap [ MIC ] to retry.", httpCode);
        xQueueSend(ui_status_queue, &p, 100);
        UIStatusPayload p_idle = {.type = STATUS_UPDATE_AI_IDLE};
        xQueueSend(ui_status_queue, &p_idle, 100);
      }
      http.end();
      client.stop();
    }
  }

  ai_upload_in_progress = false;
  vTaskDelete(NULL);
}

static bool s_dma_dumped = false;
static size_t s_decim_phase = 0;
static int32_t s_decim_acc = 0;

void start_ai_voice_recording() {
  if (ai_upload_in_progress) {
    log_w("[AI REC] Upload in progress, ignoring mic request");
    return;
  }

  if (audio.isRunning()) {
    audioStopSong();
  }

  audio.setSampleRate(16000);

  if (!ai_wav_buffer) {
    ai_wav_buffer = (uint8_t *)heap_caps_malloc(AI_REC_TOTAL_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!ai_wav_buffer) {
      log_e("[AI REC] PSRAM alloc failed for %u bytes", (unsigned)AI_REC_TOTAL_BUF_SIZE);
      return;
    }
    memset(ai_wav_buffer, 0, AI_REC_TOTAL_BUF_SIZE);
    log_i("[AI REC] PSRAM audio buffer allocated: %u bytes", (unsigned)AI_REC_TOTAL_BUF_SIZE);
  }

  if (!mic.start()) {
    log_e("[AI REC] ES7210 microphone start failed");
    UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
    snprintf(p.trackDesc, sizeof(p.trackDesc), "AI Assistant Error:\nES7210 microphone start failed.");
    xQueueSend(ui_status_queue, &p, 100);
    return;
  }

  ai_rec_pcm_bytes = 0;
  ai_recording_active = true;
  ai_recording_start_ms = millis();
  is_mic_mode = true;
  s_dma_dumped = false;
  s_decim_phase = 0;
  s_decim_acc = 0;

  bsp_set_audio_amp_power(true);
  resetScreenOffTimer(NULL);

  UIStatusPayload msg = {.type = STATUS_UPDATE_AI_LISTENING};
  xQueueSend(ui_status_queue, &msg, 100);
  log_i("[AI REC] Microphone ACTIVE: listening for voice (4.0s @ 8kHz)...");
}

static void normalize_wav_buffer(uint8_t *wav_buf, size_t pcm_bytes) {
  if (!wav_buf || pcm_bytes < 2) return;
  int16_t *samples = (int16_t *)(wav_buf + 44);
  size_t num_samples = pcm_bytes / 2;

  // 1. Remove DC offset
  int64_t sum = 0;
  for (size_t i = 0; i < num_samples; i++) sum += samples[i];
  int16_t dc_offset = (int16_t)(sum / (int64_t)num_samples);
  for (size_t i = 0; i < num_samples; i++) samples[i] -= dc_offset;

  // 2. Mute first 50ms (400 samples @ 8kHz) to prevent codec power-on pop
  size_t mute_count = (num_samples < 400) ? num_samples : 400;
  for (size_t i = 0; i < mute_count; i++) samples[i] = 0;

  // 3. Find acoustic peak from speech portion
  int32_t peak = 0;
  for (size_t i = mute_count; i < num_samples; i++) {
    int32_t v = abs((int32_t)samples[i]);
    if (v > peak) peak = v;
  }

  // 4. Normalize with gain limit
  if (peak > 300 && peak < 25000) {
    float gain = 26000.0f / (float)peak;
    if (gain > 25.0f) gain = 25.0f; // Limit maximum digital boost
    log_i("[AI REC] Normalizing audio: peak was %d (%.1f%%, DC=%d), applying gain x%.2f",
          (int)peak, (peak * 100.0f / 32768.0f), (int)dc_offset, gain);
    for (size_t i = 0; i < num_samples; i++) {
      int32_t s = (int32_t)(samples[i] * gain);
      if (s > 32767) s = 32767;
      if (s < -32768) s = -32768;
      samples[i] = (int16_t)s;
    }
  } else {
    log_i("[AI REC] Audio peak is %d (%.1f%%, DC=%d), no normalization needed",
          (int)peak, (peak * 100.0f / 32768.0f), (int)dc_offset);
  }
}

void stop_ai_voice_recording_and_process() {
  if (!ai_recording_active) return;

  ai_recording_active = false;
  is_mic_mode = false;
  mic.stop();

  log_i("[AI REC] Recording STOPPED: captured %u PCM bytes (~%.1f sec)",
        (unsigned)ai_rec_pcm_bytes, (float)ai_rec_pcm_bytes / 16000.0f);

  // Apply digital speech normalization for robust STT
  normalize_wav_buffer(ai_wav_buffer, ai_rec_pcm_bytes);

  write_wav_header(ai_wav_buffer, ai_rec_pcm_bytes, AI_REC_SAMPLE_RATE, 1, 16);

  // 1. Save recorded audio to LittleFS as /rec.wav for loopback & diagnostic download
  File f = LittleFS.open("/rec.wav", "w");
  if (f) {
    f.write(ai_wav_buffer, 44 + ai_rec_pcm_bytes);
    f.close();
    log_i("[LOOPBACK] Saved /rec.wav (%u bytes) to LittleFS", (unsigned)(44 + ai_rec_pcm_bytes));
  } else {
    log_e("[LOOPBACK] Failed to open /rec.wav for writing!");
  }

  // 2. Play heard audio back over the physical speaker immediately as requested by user
  bsp_set_audio_amp_power(true);
  speaker.setVolume(90);
  audio.setVolume(21);
  resetScreenOffTimer(NULL);

  UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
  snprintf(p.trackDesc, sizeof(p.trackDesc), "Voice Feedback:\nRepeating what was heard...");
  xQueueSend(ui_status_queue, &p, 100);

  UIStatusPayload p_spk = {.type = STATUS_UPDATE_AI_SPEAKING};
  xQueueSend(ui_status_queue, &p_spk, 100);

  mediaType = 2; // AI mode
  audio.connecttoFS(LittleFS, "/rec.wav");

  // 3. Launch upload task (it waits for repeat playback of /rec.wav to finish before server upload & streaming answer)
  if (WiFi.status() == WL_CONNECTED) {
    xTaskCreatePinnedToCore(ai_upload_task, "ai_upload_task", 10240, NULL, 5, &ai_upload_task_handle, 0);
  } else {
    log_w("[AI REC] WiFi offline, completed local loopback playback only");
  }
}
//--------------------------------
// audio information callback
void my_audio_info(Audio::msg_t m) {
  if (!m.msg || strlen(m.msg) == 0) return;

  UIStatusPayload msg = {};
  switch (mediaType) {
  case 0: // show station title / description
  {
    if (m.e == Audio::evt_streamtitle) {
      msg.type = STATUS_UPDATE_TRACK_DESC_SET;
      const char *st_name = (stations && stationIndex < stationListLength && stations[stationIndex].name) 
                            ? stations[stationIndex].name : "Online Radio";
      snprintf(msg.trackDesc, sizeof(msg.trackDesc), "%s\n%s", st_name, m.msg);
      xQueueSend(ui_status_queue, &msg, 100); // send message
      log_i("[RADIO INFO] %s", m.msg);
    }
    break;
  }
  case 1: // show song title/artist/album
  {
    if (m.e == Audio::evt_id3data) {
      if (strstr(m.msg, "Title") || strstr(m.msg, "Artist") || strstr(m.msg, "Album")) {
        msg.type = STATUS_UPDATE_TRACK_DESC_ADD;
        snprintf(msg.trackDesc, sizeof(msg.trackDesc), "%s\n", m.msg);
        xQueueSend(ui_status_queue, &msg, 100); // send message
        log_i("[ID3 INFO] %s", msg.trackDesc);
      }
    }
  } break;
  case 2: // AI Assistant streaming info
  {
    if (m.e == Audio::evt_eof || strstr(m.msg, "MP3Decoder has been destroyed") || strstr(m.msg, "WAVDecoder has been destroyed") || strstr(m.msg, "Closing web file")) {
      msg.type = STATUS_UPDATE_AI_IDLE;
      xQueueSend(ui_status_queue, &msg, 100);
      log_i("[AI INFO] Stream finished -> UI set to IDLE");
    } else if (strstr(m.msg, "MP3Decoder has been initialized") || strstr(m.msg, "WAVDecoder has been initialized") || strstr(m.msg, "stream ready")) {
      msg.type = STATUS_UPDATE_AI_SPEAKING;
      xQueueSend(ui_status_queue, &msg, 100);
      log_i("[AI INFO] Stream decoding -> UI set to SPEAKING");
    } else if (m.e == Audio::evt_streamtitle) {
      msg.type = STATUS_UPDATE_TRACK_DESC_SET;
      snprintf(msg.trackDesc, sizeof(msg.trackDesc), "AI Assistant:\n%s", m.msg);
      xQueueSend(ui_status_queue, &msg, 100);
      log_i("[AI INFO] Subtitle: %s", m.msg);
    }
  } break;
  } // switch
}

//------------------------------------
void audio_loop_task(void *param) {
  const TickType_t period = pdMS_TO_TICKS(5);
  TickType_t lastWakeTime = xTaskGetTickCount();
  static uint32_t last_pos = 0;
  static uint32_t last_total = 0;
  uint32_t current_pos = 0;
  uint32_t current_total = 0;
  char elapse_buf[16];
  char remain_buf[16];
  char status_buffer[50];
  UIStatusPayload msg = {};

  const size_t stereo_chunk_bytes = 512;
  int16_t stereo_temp[stereo_chunk_bytes / 2];
  size_t bytes_read = 0;

  UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
  log_w("{ Task stack remaining MIN: %u bytes }", hwm);

  for (;;) {

    // 1. AI VOICE RECORDING MODE (ES7210 ADC -> PSRAM WAV)
    if (ai_recording_active) {
      auto rx_handle = audio.getRxHandle();
      if (!rx_handle) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      const size_t dma_chunk_bytes = 2048;
      int16_t dma16[dma_chunk_bytes / 2];
      size_t bytes_read = 0;

      esp_err_t err = i2s_channel_read(rx_handle, dma16, sizeof(dma16), &bytes_read, pdMS_TO_TICKS(50));
      if (err == ESP_OK && bytes_read >= 8) {
        size_t frames = bytes_read / 8; // 8 bytes per 32-bit slot stereo frame (Left 32b + Right 32b)

        if (!s_dma_dumped && ai_rec_pcm_bytes > 8000 && frames >= 4) {
          s_dma_dumped = true;
          log_i("[RAW DMA 16K] F0: L=%d R=%d | F1: L=%d R=%d | F2: L=%d R=%d | F3: L=%d R=%d",
                dma16[0], dma16[2], dma16[4], dma16[6], dma16[8], dma16[10], dma16[12], dma16[14]);
        }

        int16_t *pcm_dest = (int16_t *)(ai_wav_buffer + 44 + ai_rec_pcm_bytes);
        size_t max_samples_left = (AI_REC_PCM_MAX - ai_rec_pcm_bytes) / 2;
        size_t converted = 0;

        // Native 16 kHz capture from ES7210 microphone (Mic 1 is on Left slot: dma16[i * 4 + 0])
        for (size_t i = 0; i < frames && converted < max_samples_left; i++) {
          pcm_dest[converted++] = dma16[i * 4 + 0];
        }
        ai_rec_pcm_bytes += converted * 2;
      }

      // Check if finished: buffer full (128 KB) or 4.2 seconds elapsed
      if (ai_rec_pcm_bytes >= AI_REC_PCM_MAX || (millis() - ai_recording_start_ms >= 4200)) {
        stop_ai_voice_recording_and_process();
      }
      continue;
    }

    // 2. PLAYBACK MODE
    if (!is_mic_mode) {
      if (audio.isRunning()) {
        for (int i = 0; i < 6; i++) {
          audio.loop();
        }
        process_audio_cmd_que();
        vTaskDelay(pdMS_TO_TICKS(1)); // snappy 1ms yield prevents socket starvation
      } else {
        audio.loop();
        process_audio_cmd_que();
        vTaskDelay(pdMS_TO_TICKS(10)); // idle yield saves CPU
      }

      static bool s_wifi_sleep_disabled = false;
      if (audio.isRunning() || mediaType == 0 || mediaType == 1) {
        if (!s_wifi_sleep_disabled && WiFi.status() == WL_CONNECTED) {
          WiFi.setSleep(false);
          esp_wifi_set_ps(WIFI_PS_NONE);
          s_wifi_sleep_disabled = true;
          log_i("[AUDIO/WIFI] High-throughput mode enabled (WiFi sleep OFF)");
        }
      } else if (s_wifi_sleep_disabled && !audio.isRunning() && mediaType >= 2) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        s_wifi_sleep_disabled = false;
      }

      if (audio.isRunning()) {
       // log_e("Volume: %u",audio.getVUlevel());
        current_pos = audio.getAudioCurrentTime();
        current_total = audio.getAudioFileDuration();
        if (current_pos != last_pos && current_total > 0) {
          log_d("%u/%u", current_pos, current_total);
          timeStr(elapse_buf, sizeof(elapse_buf), current_pos);
          timeStr(remain_buf, sizeof(remain_buf), current_total - current_pos);
          if (!seeking_now) {
            msg = {// prepare mesasge
                   .type = STATUS_UPDATE_PLAY_POSITION,
                   .current_pos = current_pos,
                   .total = current_total};
            snprintf(msg.elapse_buf, sizeof(msg.elapse_buf), "%s", elapse_buf);
            snprintf(msg.remain_buf, sizeof(msg.remain_buf), "%s", remain_buf);
            xQueueSend(ui_status_queue, &msg, 100); // send message
          }
          // track not 0 length
          if (current_total > 0) {
            // set progress bar status when opena new track (not equal track length)
            if (current_total != last_total) {
              msg = {// prepare mesasge
                     .type = STATUS_UPDATE_PROGRESS_BAR,
                     .total = current_total};
              xQueueSend(ui_status_queue, &msg, 100); // send message
            }

            //---------
            // detect end of file track -> next track
            if ((mediaType == 1) && (current_total - current_pos <= 1)) {
              if (lan_get_file_count() > 0) {
                switch (playMode) {
                case 0: { // normal loop all
                  lan_track_idx = (lan_track_idx + 1) % lan_get_file_count();
                  break;
                }
                case 1: { // random shuffle
                  if (lan_get_file_count() > 1) {
                    int next_idx = lan_track_idx;
                    while (next_idx == lan_track_idx) {
                      next_idx = esp_random() % lan_get_file_count();
                    }
                    lan_track_idx = next_idx;
                  }
                  break;
                }
                case 2: { // repeat single track
                  // keep lan_track_idx
                  break;
                }
                }
                snprintf(msg.trackNumber, sizeof(msg.trackNumber), "%d of %d (LAN)", lan_track_idx + 1, lan_get_file_count());
                msg.type = STATUS_UPDATE_TRACK_NUMBER;
                xQueueSend(ui_status_queue, &msg, 100);

                const LanFileEntry *f = lan_get_file(lan_track_idx);
                msg.type = STATUS_UPDATE_TRACK_DESC_SET;
                snprintf(msg.trackDesc, sizeof(msg.trackDesc), "%s", f ? f->name : "LAN Track");
                xQueueSend(ui_status_queue, &msg, 100);

                lan_play(lan_track_idx);
                last_pos = current_pos;
                last_total = current_total;
                continue;
              }

              if (trackListLength <= 0) {
                log_w("Cannot auto-select next track: empty music library");
                audio.stopSong();
                last_pos = current_pos;
                last_total = current_total;
                continue;
              }
              switch (playMode) {
              case 0: { // normal play mode
                trackIndex++;
                if (trackIndex == trackListLength) trackIndex = 0;
                break;
              }
              case 1: { // random play, avoid same track twice
                trackIndex = randomIndexExcept(trackListLength, trackIndex);
                break;
              }
              case 2: { // repeat
                // do nothing
                break;
              }
              }
              // switch
              //  update track index
              msg.type = STATUS_UPDATE_TRACK_NUMBER;
               snprintf(msg.trackNumber, sizeof(msg.trackNumber), "%d of %d", trackIndex + 1, trackListLength);
              xQueueSend(ui_status_queue, &msg, 100); // send message

              // play track
              char trackPath[512];
              getTrackPath(trackIndex, trackPath, sizeof(trackPath));
              msg.type = STATUS_UPDATE_TRACK_DESC_SET;
              if (!audio.connecttoFS(SD, trackPath)) {
                log_e("Failed to open file: %s", trackPath);
                snprintf(msg.trackDesc, sizeof(msg.trackDesc),  "Cannot access music.\nPlease check the SD Card.\nOr Update music library.");
              } else {
                snprintf(msg.trackDesc, sizeof(msg.trackDesc), "");
              }
              xQueueSend(ui_status_queue, &msg, 100); // send message
            } // detect end of track -> next track
          } // not empty track

          last_pos = current_pos;
          last_total = current_total;
        }
      }
      vTaskDelay(1);
    }
  } // for(;;)
} // audio loop task
