#include "web_remote.h"
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"
#include "ESP32-audioI2S-master/Audio.h"
#include "file/file.h"
#include <LittleFS.h>
#include "task_msg/task_msg.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "battery/battery.h"
#include "pcf85063/pcf85063.h"
#include "alarm/alarm.h"
#include "telemetry/telemetry.h"
#include "lan_stream/lan_stream.h"
#include "lvgl_port/lvgl_port.h"
#include "ui/ui.h"
#include "ui/ui_events.h"
#include "user_config.h"
#include "secrets/secrets_manager.h"
#include "imu/imu_service.h"
#include "updater/updater.h"
#include "version.h"
#include "generated_catalog.h"
#include "power/power_profile.h"
#include "ui/screens/ui_Screen_PowerProfile.h"

extern Audio audio;
extern Preferences pref;
extern "C" void exit_clock_breathing(void);
extern "C" void record_user_interaction(void);
extern void screenPowerOn(void);
extern void audioSetVolume(uint8_t vol);
extern void updateInfoPanel(uint8_t pages);
extern void on_clock_touch_or_button(void);
extern void alarm_ui_refresh(void);
extern uint8_t mediaType;
extern int lan_track_idx;
extern const char* getRadioCatalogName(uint8_t cat);
extern void switchRadioCatalog(uint8_t cat);
extern void start_ai_voice_recording();
extern void stop_ai_voice_recording_and_process();
extern volatile bool ai_recording_active;
#include "es8311/es8311.h"
extern ES8311 speaker;

static WebServer server(80);
static bool server_running = false;

// The page lives in web/index.html; tools/gzip_web_page.py gzips it at build time.
#include "web_page_gz.h"

static void handle_root() {
    record_user_interaction();
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char *)WEB_PAGE_GZ, WEB_PAGE_GZ_LEN);
}

static void handle_status() {
    float volt = 0.0f; uint8_t pct = 0;
    getBatteryStatus(&volt, &pct);

    uint8_t a_hr = 0, a_min = 0;
    alarm_get_time(&a_hr, &a_min);
    char alarm_str[8];
    snprintf(alarm_str, sizeof(alarm_str), "%02d:%02d", a_hr, a_min);

    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t dram_total = 318 * 1024;
    uint8_t dram_used_pct = (dram_free < dram_total) ? (uint8_t)((dram_total - dram_free) * 100 / dram_total) : 0;
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    // Build JSON buffer in PSRAM (8KB)
    const size_t json_sz = 8192;
    char *buf = (char *)heap_caps_malloc(json_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        server.send(500, "application/json", "{\"error\":\"out_of_mem\"}");
        return;
    }

    int offset = snprintf(buf, json_sz,
        "{\"active_screen\":\"%s\",\"clock_active\":%d,\"bl_off\":%d,\"bl_state\":%d,\"vol\":%d,"
        "\"batt_v\":%.2f,\"batt_pct\":%d,\"heap\":%u,\"psram\":%u,"
        "\"dram_free\":%u,\"dram_pct\":%d,\"uptime_s\":%u,"
        "\"rssi\":%d,\"audio_running\":%d,\"alarm_enabled\":%d,"
        "\"alarm_time\":\"%s\",\"alarm_active\":%d,\"catalog\":%d,"
        "\"catalog_name\":\"%s\",\"radio_lang\":%d,\"radio_lang_code\":\"%s\",\"radio_lang_name\":\"%s\","
        "\"station_idx\":%d,\"station_name\":\"%s\","
        "\"media_type\":%d,\"lan_server\":\"%s\",\"lan_count\":%d,\"lan_track_idx\":%d,"
        "\"telem_table\":\"%s\",\"telem_url\":\"https://autobots.my.to/telemetry.php?table=%s\","
        "\"telem_endpoint\":\"%s\",\"telem_interval\":%u,\"ai_url\":\"%s\","
        "\"consent\":%d,\"consent_decided\":%d,\"custom_ini\":%d,\"stations\":[",
        lvgl_port_get_active_screen_name(),
        (int)clock_face_active, (int)BL_OFF, (int)backlight_state,
        (int)audio.getVolume(), volt, pct,
        (unsigned int)dram_free, (unsigned int)ESP.getFreePsram(),
        (unsigned int)(dram_free / 1024), (int)dram_used_pct, uptime_s,
        (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0,
        audio.isRunning() ? 1 : 0,
        alarm_is_enabled() ? 1 : 0,
        alarm_str,
        alarm_is_active() ? 1 : 0,
        (int)currentRadioCatalog,
        getRadioCatalogName(currentRadioCatalog),
        (int)currentRadioLang,
        getRadioLanguageCode(currentRadioLang),
        getRadioLanguageName(currentRadioLang),
        (int)stationIndex,
        (stationIndex < stationListLength) ? stations[stationIndex].name : "",
        (int)mediaType,
        lan_get_server(),
        lan_get_file_count(),
        lan_track_idx,
        secrets_get_telemetry_table(),
        secrets_get_telemetry_table(),
        secrets_get_telemetry_endpoint(),
        (unsigned)secrets_get_telemetry_interval(),
        secrets_get_ai_url(),
        secrets_has_cloud_consent() ? 1 : 0,
        secrets_has_decided_consent() ? 1 : 0,
        secrets_has_custom_ini() ? 1 : 0
    );

    for (uint8_t i = 0; i < stationListLength; i++) {
        offset += snprintf(buf + offset, json_sz - offset,
            "%s{\"name\":\"%s\"}",
            (i > 0 ? "," : ""), stations[i].name);
        if (offset >= (int)json_sz - 200) break;
    }
    offset += snprintf(buf + offset, json_sz - offset, "],\"lan_files\":[");
    for (int i = 0; i < lan_get_file_count(); i++) {
        const LanFileEntry *f = lan_get_file(i);
        if (f) {
            offset += snprintf(buf + offset, json_sz - offset,
                "%s{\"name\":\"%s\"}",
                (i > 0 ? "," : ""), f->name);
            if (offset >= (int)json_sz - 50) break;
        }
    }
    snprintf(buf + offset, json_sz - offset, "]}");

    server.send(200, "application/json", buf);
    heap_caps_free(buf);
}

static void handle_vol() {
    if (server.hasArg("val")) {
        int v = server.arg("val").toInt();
        if (v < 0) v = 0;
        if (v > 21) v = 21;
        audioSetVolume(v);
    }
    handle_status();
}

static void handle_radio_lang() {
    if (server.hasArg("set")) {
        String lang_str = server.arg("set");
        uint8_t lang = RADIO_LANG_HI;
        if (lang_str.equalsIgnoreCase("hi")) lang = RADIO_LANG_HI;
        else if (lang_str.equalsIgnoreCase("en")) lang = RADIO_LANG_EN;
        else if (lang_str.equalsIgnoreCase("es")) lang = RADIO_LANG_ES;
        else if (lang_str.equalsIgnoreCase("cn")) lang = RADIO_LANG_CN;
        else if (lang_str.equalsIgnoreCase("de")) lang = RADIO_LANG_DE;
        else if (lang_str.equalsIgnoreCase("ja")) lang = RADIO_LANG_JA;
        switchRadioLanguage(lang);
    } else if (server.hasArg("id")) {
        int id = server.arg("id").toInt();
        if (id >= 0 && id <= 5) {
            switchRadioLanguage((uint8_t)id);
        }
    }
    handle_status();
}

static void handle_radio_catalog() {
    if (server.hasArg("id")) {
        int id = server.arg("id").toInt();
        switchRadioCatalog((uint8_t)id);
    }
    handle_status();
}

static void handle_radio_play() {
    if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        if (idx >= 0 && idx < stationListLength) {
            stationIndex = idx;
            audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
        }
    }
    handle_status();
}

static void handle_radio_resume() {
    if (stationIndex < stationListLength) {
        audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
    }
    handle_status();
}

static void handle_radio_stop() {
    if (audio.isRunning()) {
        audio.stopSong();
    }
    handle_status();
}

static void handle_screen() {
    exit_clock_breathing();
    screenPowerOn();
    resetScreenOffTimer(NULL);
    UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
    xQueueSend(ui_status_queue, &ui_msg, 100);
    if (server.hasArg("action")) {
        String act = server.arg("action");
        if (lvgl_port_lock(500)) {
            if (act == "clock") {
                infoPageIndex = 0;
                updateInfoPanel(0);
                _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Info_screen_init);
            } else if (act == "weather") {
                infoPageIndex = 1;
                updateInfoPanel(1);
                _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Info_screen_init);
            } else if (act == "alarm") {
                infoPageIndex = 2;
                updateInfoPanel(2);
                _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Info_screen_init);
            } else if (act == "menu") {
                _ui_screen_change(&ui_Screen_MainMenu, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_MainMenu_screen_init);
                if (server.hasArg("scroll")) {
                    int sc = server.arg("scroll").toInt();
                    lv_obj_scroll_to_x(ui_MainMenu_Panel_Menu, sc, LV_ANIM_OFF);
                }
            } else if (act == "settings") {
                _ui_screen_change(&ui_Screen_MainMenu, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_MainMenu_screen_init);
                _ui_flag_modify(ui_MainMenu_Button_closeConfig, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
                _ui_flag_modify(ui_MainMenu_Tabview_ConfigPanel, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
                if (server.hasArg("tab")) {
                    int tab_idx = server.arg("tab").toInt();
                    lv_tabview_set_act(ui_MainMenu_Tabview_ConfigPanel, tab_idx, LV_ANIM_OFF);
                }
            } else if (act == "utility") {
                _ui_screen_change(&ui_Screen_Utility, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Utility_screen_init);
                utilityMode(NULL);
            } else if (act == "power_profile") {
                _ui_screen_change(&ui_Screen_Utility, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Utility_screen_init);
                utilityMode(NULL);
                ui_power_profile_open();
                if (server.hasArg("scroll")) {
                    int sc = server.arg("scroll").toInt();
                    ui_power_profile_scroll_to((int16_t)sc);
                }
            } else if (act == "music" || act == "lan") {
                _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
                musicPlayerMode(NULL);
            } else if (act == "player" || act == "radio") {
                _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
                livestreamMode(NULL);
            } else if (act == "chat") {
                _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
                chatBotMode(NULL);
            } else if (act == "touch") {
                on_clock_touch_or_button();
            }
            lvgl_port_unlock();
        }
    }
    handle_status();
}

static void handle_touch() {
    exit_clock_breathing();
    screenPowerOn();
    on_clock_touch_or_button();
    resetScreenOffTimer(NULL);

    if (server.hasArg("x") && server.hasArg("y")) {
        int x = server.arg("x").toInt();
        int y = server.arg("y").toInt();
        lvgl_port_inject_touch((int16_t)x, (int16_t)y, 150);
        server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"tap\"}");
        return;
    }
    if (server.hasArg("x1") && server.hasArg("y1") && server.hasArg("x2") && server.hasArg("y2")) {
        int x1 = server.arg("x1").toInt();
        int y1 = server.arg("y1").toInt();
        int x2 = server.arg("x2").toInt();
        int y2 = server.arg("y2").toInt();
        uint32_t dur = server.hasArg("dur") ? server.arg("dur").toInt() : 350;
        lvgl_port_inject_swipe((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, dur);
        server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"swipe\"}");
        return;
    }
    if (server.hasArg("swipe")) {
        String dir = server.arg("swipe");
        if (dir == "up") {
            lvgl_port_inject_swipe(320, 150, 320, 20, 350);
        } else if (dir == "down") {
            lvgl_port_inject_swipe(320, 20, 320, 150, 350);
        } else if (dir == "left") {
            lvgl_port_inject_swipe(550, 86, 90, 86, 350);
        } else if (dir == "right") {
            lvgl_port_inject_swipe(90, 86, 550, 86, 350);
        }
        server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"swipe\"}");
        return;
    }
    handle_status();
}

static void handle_bl() {
    if (server.hasArg("state")) {
        int s = server.arg("state").toInt();
        if (s == 0) {
            backlight_state = 0;
            pref.begin("config", false); pref.putUChar("bl_state", 0); pref.end();
            setUpduty(LCD_BL_LOW);
        } else if (s == 1) {
            backlight_state = 1;
            pref.begin("config", false); pref.putUChar("bl_state", 1); pref.end();
            setUpduty(LCD_BL_MED);
        } else if (s == 2) {
            backlight_state = 2;
            pref.begin("config", false); pref.putUChar("bl_state", 2); pref.end();
            setUpduty(LCD_BL_HIGH);
        }
    }
    handle_status();
}

static void handle_alarm() {
    if (server.hasArg("enabled")) {
        alarm_set_enabled(server.arg("enabled").toInt() == 1);
    }
    if (server.hasArg("time")) {
        String t = server.arg("time");
        int c = t.indexOf(':');
        if (c > 0) {
            int h = t.substring(0, c).toInt();
            int m = t.substring(c + 1).toInt();
            alarm_set_time(h, m);
        }
    }
    if (lvgl_port_lock(200)) {
        alarm_ui_refresh();
        lvgl_port_unlock();
    }
    handle_status();
}

static void handle_alarm_stop() {
    alarm_stop();
    handle_status();
}

static void handle_alarm_test() {
    alarm_trigger();
    handle_status();
}

static void handle_telemetry() {
    if (server.hasArg("action")) {
        String act = server.arg("action");
        if (act == "snap") {
            telemetry_collect_snapshot();
        } else if (act == "flush") {
            telemetry_flush_batch();
        }
    }
    handle_status();
}

static void handle_consent() {
    if (server.hasArg("action")) {
        String act = server.arg("action");
        if (act == "accept") {
            secrets_set_cloud_consent(true);
            telemetry_collect_snapshot();
            telemetry_flush_batch();
        } else if (act == "decline") {
            secrets_set_cloud_consent(false);
        } else if (act == "reset") {
            secrets_reset_consent();
        }
    }
    handle_status();
}

static void handle_secrets() {
    if (server.method() == HTTP_POST) {
        String ai_url = server.hasArg("ai_url") ? server.arg("ai_url") : "";
        String telem_url = server.hasArg("telem_url") ? server.arg("telem_url") : "";
        String table = server.hasArg("table") ? server.arg("table") : "";
        uint32_t interval = server.hasArg("interval") ? (uint32_t)server.arg("interval").toInt() : 300;
        String pin = server.hasArg("pin") ? server.arg("pin") : "";
        bool consent = server.hasArg("consent") ? (server.arg("consent") == "1" || server.arg("consent") == "true") : true;

        if (server.hasArg("action") && server.arg("action") == "restore") {
            secrets_delete_ini_file();
            server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Restored firmware defaults\"}");
            return;
        }

        bool ok = secrets_write_ini_file(
            ai_url.length() ? ai_url.c_str() : secrets_get_ai_url(),
            telem_url.length() ? telem_url.c_str() : secrets_get_telemetry_endpoint(),
            table.length() ? table.c_str() : secrets_get_telemetry_table(),
            interval > 0 ? interval : 300,
            pin.length() ? pin.c_str() : secrets_get_master_pin(),
            consent
        );

        if (ok) {
            server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Secrets saved to /secrets.ini\"}");
        } else {
            server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to write /secrets.ini\"}");
        }
        return;
    }

    // GET: return current secrets configuration
    char json_buf[1024];
    snprintf(json_buf, sizeof(json_buf),
        "{\"ai_url\":\"%s\",\"telem_endpoint\":\"%s\",\"telem_table\":\"%s\",\"interval\":%u,"
        "\"custom_ini\":%d,\"consent\":%d,\"consent_decided\":%d,\"pin\":\"%s\"}",
        secrets_get_ai_url(),
        secrets_get_telemetry_endpoint(),
        secrets_get_telemetry_table(),
        (unsigned)secrets_get_telemetry_interval(),
        secrets_has_custom_ini() ? 1 : 0,
        secrets_has_cloud_consent() ? 1 : 0,
        secrets_has_decided_consent() ? 1 : 0,
        secrets_get_master_pin()
    );
    server.send(200, "application/json", json_buf);
}

static void handle_ask() {
    if (server.hasArg("q")) {
        String q = server.arg("q");
        log_i("[ASSISTANT] Prompt received: %s", q.c_str());
        q.trim();
        q.replace(" ", "+");
        mediaType = 2; // AI Assistant mode
        String bridgeUrl = String(secrets_get_ai_url()) + "?device=" + secrets_get_telemetry_table() + "&q=" + q;
        audioPlayHOST(bridgeUrl.c_str(), "AI Assistant");
    }
    handle_status();
}

static void handle_lan_server() {
    if (server.hasArg("srv")) {
        String srv = server.arg("srv");
        srv.trim();
        lan_set_server(srv.c_str());
    }
    handle_status();
}

static void handle_lan_fetch() {
    lan_fetch_files();
    handle_status();
}

static void handle_lan_play() {
    if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        lan_play(idx);
    }
    handle_status();
}

static void handle_screenshot() {
    exit_clock_breathing();
    screenPowerOn();
    resetScreenOffTimer(NULL);
    UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
    xQueueSend(ui_status_queue, &ui_msg, 100);
    lvgl_port_take_screenshot();
    const uint16_t *fb = lvgl_port_get_framebuffer();
    if (!fb) {
        server.send(500, "text/plain", "Framebuffer unavailable");
        return;
    }

    const uint32_t width = WAVESHARE_349_LCD_H_RES;
    const uint32_t height = WAVESHARE_349_LCD_V_RES;
    const uint32_t row_stride = ((width * 3 + 3) / 4) * 4;
    const uint32_t image_size = row_stride * height;
    const uint32_t file_size = 54 + image_size;

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

    WiFiClient client = server.client();
    server.setContentLength(file_size);
    server.send(200, "image/bmp", "");
    client.write(bmp_hdr, 54);

    uint8_t *row_buf = (uint8_t *)heap_caps_malloc(row_stride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row_buf) {
        server.send(500, "text/plain", "Out of memory for screenshot buffer");
        return;
    }
    const bool is_flipped = lvgl_port_is_inverted_landscape();
    for (int y = (int)height - 1; y >= 0; y--) {
        memset(row_buf, 0, row_stride);
        int src_y = is_flipped ? ((int)height - 1 - y) : y;
        const uint16_t *src_row = fb + (src_y * width);
        uint8_t *dst = row_buf;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t src_x = is_flipped ? (width - 1 - x) : x;
            uint16_t raw = src_row[src_x];
#if LV_COLOR_16_SWAP
            uint16_t p = (uint16_t)((raw >> 8) | (raw << 8));
#else
            uint16_t p = raw;
#endif
            uint8_t r = (p >> 11) & 0x1F;
            uint8_t g = (p >> 5) & 0x3F;
            uint8_t b = p & 0x1F;
            *dst++ = (b * 255) / 31;
            *dst++ = (g * 255) / 63;
            *dst++ = (r * 255) / 31;
        }
        client.write(row_buf, row_stride);
    }
    heap_caps_free(row_buf);
}

static void handle_rec_wav() {
    if (LittleFS.exists("/rec.wav")) {
        File f = LittleFS.open("/rec.wav", "r");
        if (f) {
            server.streamFile(f, "audio/wav");
            f.close();
            return;
        }
    }
    server.send(404, "text/plain", "No recorded audio found.");
}

static void handle_rec_action() {
    String act = server.hasArg("action") ? server.arg("action") : "";
    if (act == "start" || act == "rec") {
        start_ai_voice_recording();
        server.send(200, "application/json", "{\"status\":\"recording\"}");
    } else if (act == "stop") {
        stop_ai_voice_recording_and_process();
        server.send(200, "application/json", "{\"status\":\"playback\"}");
    } else if (act == "play") {
        mediaType = 2;
        speaker.setVolume(90);
        audio.setVolume(21);
        audio.connecttoFS(LittleFS, "/rec.wav");
        server.send(200, "application/json", "{\"status\":\"playing\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"invalid action\"}");
    }
}

static void handle_imu() {
    if (server.hasArg("autorotate")) {
        String ar = server.arg("autorotate");
        if (ar == "1" || ar == "true" || ar == "on") {
            imu_sensor_set_autorotate_enabled(true);
        } else if (ar == "0" || ar == "false" || ar == "off") {
            imu_sensor_set_autorotate_enabled(false);
        }
    }
    if (server.hasArg("rotate")) {
        int r = server.arg("rotate").toInt();
        if (r == 180) {
            imu_sensor_force_orientation(true);
        } else if (r == 0) {
            imu_sensor_force_orientation(false);
        }
    }

    imu_reading_t imu;
    bool ok = imu_sensor_get_latest(&imu);
    char buf[384];
    if (ok) {
        snprintf(buf, sizeof(buf),
            "{\"available\":true,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
            "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"temp_c\":%.1f,"
            "\"is_inverted\":%s,\"orientation\":\"%s\",\"autorotate\":%s}",
            imu.ax, imu.ay, imu.az,
            imu.gx, imu.gy, imu.gz, imu.temp_c,
            imu.is_inverted ? "true" : "false",
            imu.orientation_str,
            imu_sensor_is_autorotate_enabled() ? "true" : "false"
        );
    } else {
        snprintf(buf, sizeof(buf), "{\"available\":false,\"error\":\"imu_unavailable\"}");
    }
    server.send(200, "application/json", buf);
}

static void handle_imu_stream() {
    WiFiClient client = server.client();
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Connection", "keep-alive");
    server.send(200, "text/event-stream", "");

    // Stream live sensor updates for up to 30 seconds or until client disconnects
    uint32_t start_ms = millis();
    while (client.connected() && (millis() - start_ms < 30000)) {
        imu_reading_t imu;
        if (imu_sensor_get_latest(&imu)) {
            char chunk[256];
            int len = snprintf(chunk, sizeof(chunk),
                "data: {\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"temp\":%.1f,\"inverted\":%s,\"state\":\"%s\"}\n\n",
                imu.ax, imu.ay, imu.az, imu.gx, imu.gy, imu.gz, imu.temp_c,
                imu.is_inverted ? "true" : "false", imu.orientation_str);
            client.write((const uint8_t *)chunk, len);
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 10Hz live stream
    }
}

static bool s_ota_active = false;
static size_t s_ota_bytes_last = 0;
static uint32_t s_ota_ms_last = 0;

static void handle_ota_repos(void) {
    const char *json = "{\"active_repo\":\"official\",\"approved_repos\":["
      "{\"id\":\"official\",\"name\":\"TuneBar Official (rahulraj80)\",\"author\":\"rahulraj80\","
      "\"catalog_url\":\"https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/firmware_catalog.json\","
      "\"repo_url\":\"https://github.com/rahulraj80/TuneBar\","
      "\"description\":\"Official firmware with HiFi radio, LAN streaming, Nixie clock, vector UI & IMU auto-rotation\","
      "\"metrics\":{\"dram_free_avg_kb\":182,\"psram_free_avg_mb\":4.8,\"battery_life_clock_hours\":14.5,\"battery_life_active_hours\":5.2}},"
      "{\"id\":\"upstream\",\"name\":\"TuneBar Upstream (VaAndCob)\",\"author\":\"VaAndCob\","
      "\"catalog_url\":\"https://raw.githubusercontent.com/VaAndCob/TuneBar/main/releases/firmware_catalog.json\","
      "\"repo_url\":\"https://github.com/VaAndCob/TuneBar\","
      "\"description\":\"Original upstream baseline repository\","
      "\"metrics\":{\"dram_free_avg_kb\":160,\"psram_free_avg_mb\":4.5,\"battery_life_clock_hours\":12.0,\"battery_life_active_hours\":4.5}}"
      "]}";
    server.send(200, "application/json", json);
}

static void handle_ota_catalog(void) {
    String json = get_generated_catalog_json();
    server.send(200, "application/json", json);
}

static void handle_update_done(void) {
    uint8_t batt_pct = 0; float batt_v = 0.0f;
    if (!ota_check_battery_allowed(&batt_pct, &batt_v)) {
        server.sendHeader("Connection", "close");
        server.send(403, "application/json", "{\"status\":\"blocked\",\"reason\":\"battery_too_low\",\"battery_pct\":" + String(batt_pct) + ",\"required_pct\":70,\"message\":\"OTA blocked: Battery below 70% (" + String(batt_pct) + "%). Connect USB charger.\"}");
        return;
    }

    bool ok = !Update.hasError();
    server.sendHeader("Connection", "close");
    if (ok) {
        server.send(200, "application/json", "{\"status\":\"ok\",\"version\":\"" TUNEBAR_VERSION_STR "\",\"build\":" + String(TUNEBAR_BUILD_NUM) + ",\"message\":\"Update successful! Rebooting now...\"}");
        log_i("[OTA] Firmware update successful! Restarting in 2s...");
        delay(2000);
        ESP.restart();
    } else {
        String err = "{\"status\":\"error\",\"message\":\"Update failed (code " + String(Update.getError()) + ")\"}";
        server.send(500, "application/json", err);
        log_e("[OTA] Firmware update failed with error code: %d", Update.getError());
    }
}

static size_t s_ota_total_expected = 0;

static void handle_update_upload(void) {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
#ifdef LAUNCHER_BUILD
        // The next OTA slot may hold another app the Launcher installed.
        log_w("[OTA] Disabled in the Launcher build; update DeskBar from the Launcher");
        s_ota_active = false;
        return;
#endif
        log_i("[OTA] Starting firmware upload: %s", upload.filename.c_str());

        int clen = server.header("Content-Length").toInt();
        if (clen > 100000) {
            s_ota_total_expected = (size_t)clen;
            log_i("[OTA] Dynamic Content-Length parsed: %u bytes", (unsigned)s_ota_total_expected);
        } else {
            s_ota_total_expected = 6350000ULL;
            log_w("[OTA] Content-Length missing/small (%d), using fallback %u bytes", clen, (unsigned)s_ota_total_expected);
        }

        uint8_t batt_pct = 0; float batt_v = 0.0f;
        if (!ota_check_battery_allowed(&batt_pct, &batt_v)) {
            log_w("[OTA] BLOCKED: Battery is too low (%u%%, %.2fV)", (unsigned)batt_pct, batt_v);
            ota_ui_show_blocked_battery(batt_pct);
            s_ota_active = false;
            return;
        }

        s_ota_active = true;
        s_ota_bytes_last = 0;
        s_ota_ms_last = millis();

        ota_pre_update_teardown();
        ota_ui_show_neon_progress(upload.filename.c_str());

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
            log_e("[OTA] Update.begin failed!");
            s_ota_active = false;
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!s_ota_active) return;

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
            log_e("[OTA] Update.write failed!");
        } else {
            uint32_t now = millis();
            if (now - s_ota_ms_last >= 250) {
                uint32_t dt_ms = now - s_ota_ms_last;
                size_t dbytes = upload.totalSize - s_ota_bytes_last;
                uint32_t speed_kb = (dt_ms > 0) ? (uint32_t)((dbytes * 1000ULL) / (dt_ms * 1024ULL)) : 0;
                s_ota_ms_last = now;
                s_ota_bytes_last = upload.totalSize;

                size_t total_exp = (s_ota_total_expected > 0) ? s_ota_total_expected : 6350000ULL;
                uint8_t pct = (uint8_t)((upload.totalSize * 100ULL) / total_exp);
                if (pct > 99) pct = 99;
                ota_ui_update_progress(pct, upload.totalSize, total_exp, speed_kb);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!s_ota_active) return;

        if (Update.end(true)) {
            log_i("[OTA] Firmware upload completed! Total size: %u bytes", (unsigned)upload.totalSize);
            ota_ui_update_progress(100, upload.totalSize, upload.totalSize, 0);
            ota_ui_show_complete(TUNEBAR_VERSION_STR);
        } else {
            Update.printError(Serial);
            log_e("[OTA] Update.end failed!");
        }
        s_ota_active = false;
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        s_ota_active = false;
        log_w("[OTA] Firmware upload aborted by client");
    }
}

static void handle_power_profile_get() {
    const power_profile_t *pwr = power_profile_get();
    float volts[BATTERY_CURVE_PTS];
    uint8_t pcts[BATTERY_CURVE_PTS];
    uint8_t count = 0;
    battery_get_curve_points(volts, pcts, &count);

    String json = "{";
    json += "\"status\":\"ok\",";
    json += "\"bl_start\":" + String(pwr->bl_start) + ",";
    json += "\"bl_end\":" + String(pwr->bl_end) + ",";
    json += "\"bl_black\":" + String(pwr->bl_black) + ",";
    json += "\"ramp_up_ms\":" + String(pwr->ramp_up_ms) + ",";
    json += "\"hold_high_ms\":" + String(pwr->hold_high_ms) + ",";
    json += "\"ramp_down_ms\":" + String(pwr->ramp_down_ms) + ",";
    json += "\"hold_dark_ms\":" + String(pwr->hold_dark_ms) + ",";
    json += "\"inactivity_tier\":" + String(pwr->inactivity_tier ? "true" : "false") + ",";
    json += "\"gate_wifi\":" + String(pwr->gate_wifi ? "true" : "false") + ",";
    json += "\"gate_audio\":" + String(pwr->gate_audio ? "true" : "false") + ",";
    json += "\"gate_sensors\":" + String(pwr->gate_sensors ? "true" : "false") + ",";
    json += "\"cal_source\":\"" + String(battery_get_cal_source()) + "\",";
    json += "\"cal_source_label\":\"" + String(battery_get_cal_source_label()) + "\",";
    json += "\"cal_time\":\"" + String(battery_get_cal_timestamp()) + "\",";
    json += "\"volts\":[";
    for (int i = 0; i < count; i++) {
        json += String(volts[i], 3) + (i < count - 1 ? "," : "");
    }
    json += "],\"pcts\":[";
    for (int i = 0; i < count; i++) {
        json += String(pcts[i]) + (i < count - 1 ? "," : "");
    }
    json += "]}";

    server.send(200, "application/json", json);
}

static void handle_power_profile_post() {
    power_profile_t pwr = *power_profile_get();

    if (server.hasArg("plain")) {
        DynamicJsonDocument doc(1024);
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (!err) {
            if (doc.containsKey("bl_start")) pwr.bl_start = doc["bl_start"];
            if (doc.containsKey("bl_end")) pwr.bl_end = doc["bl_end"];
            if (doc.containsKey("bl_black")) pwr.bl_black = doc["bl_black"];
            if (doc.containsKey("ramp_up_ms")) pwr.ramp_up_ms = doc["ramp_up_ms"];
            if (doc.containsKey("hold_high_ms")) pwr.hold_high_ms = doc["hold_high_ms"];
            if (doc.containsKey("ramp_down_ms")) pwr.ramp_down_ms = doc["ramp_down_ms"];
            if (doc.containsKey("hold_dark_ms")) pwr.hold_dark_ms = doc["hold_dark_ms"];
            if (doc.containsKey("inactivity_tier")) pwr.inactivity_tier = doc["inactivity_tier"];
            if (doc.containsKey("gate_wifi")) pwr.gate_wifi = doc["gate_wifi"];
            if (doc.containsKey("gate_audio")) pwr.gate_audio = doc["gate_audio"];
            if (doc.containsKey("gate_sensors")) pwr.gate_sensors = doc["gate_sensors"];
        }
    } else {
        if (server.hasArg("bl_start")) pwr.bl_start = server.arg("bl_start").toInt();
        if (server.hasArg("bl_end")) pwr.bl_end = server.arg("bl_end").toInt();
        if (server.hasArg("bl_black")) pwr.bl_black = server.arg("bl_black").toInt();
        if (server.hasArg("ramp_up_ms")) pwr.ramp_up_ms = server.arg("ramp_up_ms").toInt();
        if (server.hasArg("hold_high_ms")) pwr.hold_high_ms = server.arg("hold_high_ms").toInt();
        if (server.hasArg("ramp_down_ms")) pwr.ramp_down_ms = server.arg("ramp_down_ms").toInt();
        if (server.hasArg("hold_dark_ms")) pwr.hold_dark_ms = server.arg("hold_dark_ms").toInt();
        if (server.hasArg("inactivity_tier")) pwr.inactivity_tier = (server.arg("inactivity_tier") == "1" || server.arg("inactivity_tier") == "true");
        if (server.hasArg("gate_wifi")) pwr.gate_wifi = (server.arg("gate_wifi") == "1" || server.arg("gate_wifi") == "true");
        if (server.hasArg("gate_audio")) pwr.gate_audio = (server.arg("gate_audio") == "1" || server.arg("gate_audio") == "true");
        if (server.hasArg("gate_sensors")) pwr.gate_sensors = (server.arg("gate_sensors") == "1" || server.arg("gate_sensors") == "true");
    }

    power_profile_set(&pwr);
    handle_power_profile_get();
}

static void handle_battery_sync_cal() {
    bool ok = battery_sync_from_cloud(NULL);
    if (ok) {
        if (lvgl_port_lock(500)) {
            ui_power_profile_refresh();
            lvgl_port_unlock();
        }
        handle_power_profile_get();
    } else {
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to sync battery curve from cloud\"}");
    }
}

void web_remote_init(void) {
    if (server_running) return;

    const char *headerkeys[] = {"Content-Length", "User-Agent"};
    server.collectHeaders(headerkeys, 2);

    server.on("/", HTTP_GET, handle_root);
    server.on("/api/status", HTTP_GET, handle_status);
    server.on("/api/update", HTTP_POST, handle_update_done, handle_update_upload);
    server.on("/api/ota/repos", HTTP_GET, handle_ota_repos);
    server.on("/api/ota/catalog", HTTP_GET, handle_ota_catalog);
    server.on("/api/vol", HTTP_POST, handle_vol);
    server.on("/api/radio/lang", HTTP_GET, handle_radio_lang);
    server.on("/api/radio/lang", HTTP_POST, handle_radio_lang);
    server.on("/api/radio/catalog", HTTP_POST, handle_radio_catalog);
    server.on("/api/radio/play", HTTP_POST, handle_radio_play);
    server.on("/api/radio/resume", HTTP_POST, handle_radio_resume);
    server.on("/api/radio/stop", HTTP_POST, handle_radio_stop);
    server.on("/api/screen", HTTP_GET, handle_screen);
    server.on("/api/screen", HTTP_POST, handle_screen);
    server.on("/api/touch", HTTP_GET, handle_touch);
    server.on("/api/touch", HTTP_POST, handle_touch);
    server.on("/api/bl", HTTP_POST, handle_bl);
    server.on("/api/alarm", HTTP_GET, handle_alarm);
    server.on("/api/alarm", HTTP_POST, handle_alarm);
    server.on("/api/alarm/stop", HTTP_POST, handle_alarm_stop);
    server.on("/api/alarm/test", HTTP_POST, handle_alarm_test);
    server.on("/api/telemetry", HTTP_POST, handle_telemetry);
    server.on("/api/consent", HTTP_GET, handle_consent);
    server.on("/api/consent", HTTP_POST, handle_consent);
    server.on("/api/secrets", HTTP_GET, handle_secrets);
    server.on("/api/secrets", HTTP_POST, handle_secrets);
    server.on("/api/ask", HTTP_GET, handle_ask);
    server.on("/api/ask", HTTP_POST, handle_ask);
    server.on("/api/lan/server", HTTP_GET, handle_lan_server);
    server.on("/api/lan/server", HTTP_POST, handle_lan_server);
    server.on("/api/lan/fetch", HTTP_GET, handle_lan_fetch);
    server.on("/api/lan/fetch", HTTP_POST, handle_lan_fetch);
    server.on("/api/lan/play", HTTP_GET, handle_lan_play);
    server.on("/api/lan/play", HTTP_POST, handle_lan_play);
    server.on("/rec.wav", HTTP_GET, handle_rec_wav);
    server.on("/api/rec", HTTP_GET, handle_rec_action);
    server.on("/api/rec", HTTP_POST, handle_rec_action);
    server.on("/api/screenshot", HTTP_GET, handle_screenshot);
    server.on("/screenshot.bmp", HTTP_GET, handle_screenshot);
    server.on("/api/imu", HTTP_GET, handle_imu);
    server.on("/api/imu", HTTP_POST, handle_imu);
    server.on("/api/imu/stream", HTTP_GET, handle_imu_stream);
    server.on("/api/power_profile", HTTP_GET, handle_power_profile_get);
    server.on("/api/power_profile", HTTP_POST, handle_power_profile_post);
    server.on("/api/battery/sync_cal", HTTP_POST, handle_battery_sync_cal);

    server.enableCORS(true);
    server.begin();
    server_running = true;
    log_i("[WEB REMOTE] Server started on port 80. Accessible at http://%s/",
          WiFi.localIP().toString().c_str());
}

void web_remote_loop(void) {
    if (server_running) {
        server.handleClient();
    }
}
