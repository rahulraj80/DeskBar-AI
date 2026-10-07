#include "lan_stream.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include "lvgl_port/lvgl_port.h"
#include "file/file.h"
#include "task_msg/task_msg.h"
#include "ui/screens/ui_Screen_Player.h"
#include "user_config.h"

static LanFileEntry *s_files = NULL;
static int s_file_count = 0;
static char s_server[128] = DEFAULT_LAN_STREAM_SRV;
static Preferences lan_pref;
static bool s_is_fetching = false;

void lan_stream_init(void) {
    if (!s_files) {
        s_files = (LanFileEntry *)heap_caps_malloc(sizeof(LanFileEntry) * LAN_MAX_FILES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_files) {
            memset(s_files, 0, sizeof(LanFileEntry) * LAN_MAX_FILES);
        }
    }

    lan_pref.begin("tb_lan", false);
    String s = lan_pref.getString("srv", DEFAULT_LAN_STREAM_SRV);
    if (s == "192.168.3.5:8080") { // migrate legacy default to new LocalShare endpoint
        s = "192.168.3.10:8080/e37bd4";
        lan_pref.putString("srv", s);
    }
    strncpy(s_server, s.c_str(), sizeof(s_server) - 1);
    s_server[sizeof(s_server) - 1] = '\0';
    lan_pref.end();
    log_i("[LAN STREAM] Initialized with server: %s", s_server);
}

void lan_set_server(const char *ip_port) {
    if (!ip_port || strlen(ip_port) == 0) return;
    String s = String(ip_port);
    s.trim();
    if (s.startsWith("http://")) s = s.substring(7);
    if (s.startsWith("https://")) s = s.substring(8);
    while (s.endsWith("/")) s = s.substring(0, s.length() - 1);

    strncpy(s_server, s.c_str(), sizeof(s_server) - 1);
    s_server[sizeof(s_server) - 1] = '\0';
    lan_pref.begin("tb_lan", false);
    lan_pref.putString("srv", s_server);
    lan_pref.end();
    log_i("[LAN STREAM] Server updated to: %s", s_server);
}

const char* lan_get_server(void) {
    return s_server;
}

int lan_get_file_count(void) {
    return s_file_count;
}

const LanFileEntry* lan_get_file(int idx) {
    if (idx >= 0 && idx < s_file_count && s_files) {
        return &s_files[idx];
    }
    return NULL;
}

int lan_fetch_files(void) {
    if (WiFi.status() != WL_CONNECTED) {
        log_w("[LAN STREAM] Cannot fetch files: WiFi not connected");
        return -1;
    }
    if (s_is_fetching) {
        log_w("[LAN STREAM] Fetch already in progress");
        return s_file_count;
    }
    s_is_fetching = true;

    if (!s_files) lan_stream_init();

    String srvClean = String(s_server);
    srvClean.trim();
    if (srvClean.startsWith("http://")) srvClean = srvClean.substring(7);
    if (srvClean.startsWith("https://")) srvClean = srvClean.substring(8);
    while (srvClean.endsWith("/")) srvClean = srvClean.substring(0, srvClean.length() - 1);

    String hostRoot = "http://";
    int slashIdx = srvClean.indexOf('/');
    if (slashIdx >= 0) {
        hostRoot += srvClean.substring(0, slashIdx);
    } else {
        hostRoot += srvClean;
    }

    String baseUrl = String("http://") + srvClean;

    HTTPClient http;
    http.setTimeout(4000);
    http.begin(baseUrl);
    http.addHeader("User-Agent", "TuneBar/1.0");
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        log_w("[LAN STREAM] HTTP GET %s returned %d", baseUrl.c_str(), code);
        http.end();
        s_is_fetching = false;
        return -1;
    }

    int contentLength = http.getSize();

    // Allocate buffer in PSRAM to protect internal DRAM
    const size_t BUF_SIZE = 49152;
    char *html_buf = (char *)heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!html_buf) {
        log_e("[LAN STREAM] Out of PSRAM for fetch buffer");
        http.end();
        s_is_fetching = false;
        return -1;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t total_read = 0;
    uint32_t start_ms = millis();
    while ((total_read < BUF_SIZE - 1) && (millis() - start_ms < 3000)) {
        if (contentLength > 0 && total_read >= (size_t)contentLength) break;
        size_t avail = stream->available();
        if (avail) {
            size_t to_read = (avail < (BUF_SIZE - 1 - total_read)) ? avail : (BUF_SIZE - 1 - total_read);
            int bytes = stream->readBytes(html_buf + total_read, to_read);
            if (bytes > 0) {
                total_read += bytes;
                start_ms = millis();
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (!http.connected() && stream->available() == 0) break;
        }
    }
    html_buf[total_read] = '\0';
    http.end();

    log_i("[LAN STREAM] Read %u bytes from %s", (unsigned)total_read, baseUrl.c_str());

    s_file_count = 0;

    // Format 1: LocalShare JSON manifest (window.initialFiles = [{ ... }])
    const char *p = strstr(html_buf, "initialFiles");
    if (p) {
        log_i("[LAN STREAM] Detected LocalShare JSON manifest format");
        while (s_file_count < LAN_MAX_FILES) {
            const char *fid_ptr = strstr(p, "\"fileId\"");
            if (!fid_ptr) break;
            const char *q1 = strchr(fid_ptr + 8, '\"');
            if (!q1) break;
            const char *q2 = strchr(q1 + 1, '\"');
            if (!q2) break;

            const char *fn_ptr = strstr(q2 + 1, "\"filename\"");
            if (!fn_ptr) break;
            const char *fq1 = strchr(fn_ptr + 10, '\"');
            if (!fq1) break;
            const char *fq2 = strchr(fq1 + 1, '\"');
            if (!fq2) break;

            char file_id[32] = {0};
            size_t id_len = q2 - (q1 + 1);
            if (id_len >= sizeof(file_id)) id_len = sizeof(file_id) - 1;
            strncpy(file_id, q1 + 1, id_len);

            char file_name[128] = {0};
            size_t name_len = fq2 - (fq1 + 1);
            if (name_len >= sizeof(file_name)) name_len = sizeof(file_name) - 1;
            strncpy(file_name, fq1 + 1, name_len);

            p = fq2 + 1;

            String lower = String(file_name);
            lower.toLowerCase();
            if (lower.endsWith(".mp3") || lower.endsWith(".aac") || lower.endsWith(".wav") ||
                lower.endsWith(".m4a") || lower.endsWith(".flac") || lower.endsWith(".ogg") ||
                lower.endsWith(".opus")) {

                strncpy(s_files[s_file_count].name, file_name, sizeof(s_files[s_file_count].name) - 1);
                s_files[s_file_count].name[sizeof(s_files[s_file_count].name) - 1] = '\0';

                String stream_url = baseUrl + "/file/" + file_id;
                strncpy(s_files[s_file_count].url, stream_url.c_str(), sizeof(s_files[s_file_count].url) - 1);
                s_files[s_file_count].url[sizeof(s_files[s_file_count].url) - 1] = '\0';

                log_i("[LAN STREAM] [%d] %s -> %s", s_file_count + 1, s_files[s_file_count].name, s_files[s_file_count].url);
                s_file_count++;
            }
        }
    } else {
        // Format 2: Standard HTML directory listing (<a href="...">)
        log_i("[LAN STREAM] Parsing standard HTML directory listing");
        char *ptr = html_buf;
        while (s_file_count < LAN_MAX_FILES) {
            char *href_idx = strstr(ptr, "href=\"");
            if (!href_idx) href_idx = strstr(ptr, "href='");
            if (!href_idx) break;

            char quote = *(href_idx + 5);
            char *start = href_idx + 6;
            char *end = strchr(start, quote);
            if (!end) break;

            *end = '\0';
            String file_link = String(start);
            ptr = end + 1;

            String lower = file_link;
            lower.toLowerCase();
            if (lower.endsWith(".mp3") || lower.endsWith(".aac") || lower.endsWith(".wav") || 
                lower.endsWith(".m4a") || lower.endsWith(".flac") || lower.endsWith(".ogg") ||
                lower.endsWith(".opus")) {

                String full_url;
                if (file_link.startsWith("http://") || file_link.startsWith("https://")) {
                    full_url = file_link;
                } else if (file_link.startsWith("/")) {
                    full_url = hostRoot + file_link;
                } else {
                    full_url = baseUrl + "/" + file_link;
                }

                String disp_name = file_link;
                int last_slash = disp_name.lastIndexOf('/');
                if (last_slash >= 0) disp_name = disp_name.substring(last_slash + 1);
                disp_name.replace("%20", " ");
                disp_name.replace("+", " ");

                strncpy(s_files[s_file_count].name, disp_name.c_str(), sizeof(s_files[s_file_count].name) - 1);
                s_files[s_file_count].name[sizeof(s_files[s_file_count].name) - 1] = '\0';
                strncpy(s_files[s_file_count].url, full_url.c_str(), sizeof(s_files[s_file_count].url) - 1);
                s_files[s_file_count].url[sizeof(s_files[s_file_count].url) - 1] = '\0';

                log_i("[LAN STREAM] [%d] %s -> %s", s_file_count + 1, s_files[s_file_count].name, s_files[s_file_count].url);
                s_file_count++;
            }
        }
    }

    heap_caps_free(html_buf);
    log_i("[LAN STREAM] Total files indexed: %d", s_file_count);

    if (mediaType == 1 && s_file_count > 0) {
        if (lvgl_port_lock(200)) {
            char buf[48];
            snprintf(buf, sizeof(buf), "1 of %d (LAN)", s_file_count);
            if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, buf);
            if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, s_files[0].name);
            lvgl_port_unlock();
        }
    }

    s_is_fetching = false;
    return s_file_count;
}

static void lan_fetch_task_func(void *pv) {
    lan_fetch_files();
    vTaskDelete(NULL);
}

void lan_fetch_files_async(void) {
    if (s_is_fetching) return;
    xTaskCreate(lan_fetch_task_func, "lan_fetch", 10240, NULL, 1, NULL);
}

bool lan_play(int idx) {
    if (idx < 0 || idx >= s_file_count || !s_files) return false;
    log_i("[LAN STREAM] Playing track [%d]: %s (%s)", idx + 1, s_files[idx].name, s_files[idx].url);
    mediaType = 1;
    lan_track_idx = idx;
    if (lvgl_port_lock(200)) {
        char buf[48];
        snprintf(buf, sizeof(buf), "%d of %d (LAN)", idx + 1, s_file_count);
        if (ui_Player_Label_trackNumber) lv_label_set_text(ui_Player_Label_trackNumber, buf);
        if (ui_Player_Textarea_status) lv_textarea_set_text(ui_Player_Textarea_status, s_files[idx].name);
        lvgl_port_unlock();
    }
    audioPlayHOST(s_files[idx].url, s_files[idx].name);
    return true;
}
