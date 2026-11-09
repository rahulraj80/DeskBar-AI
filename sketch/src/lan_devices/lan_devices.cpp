#include "lan_devices.h"
#include "network/network.h"
#include "secrets/secrets_manager.h"
#include "lvgl_port/lvgl_port.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ESPmDNS.h>

extern "C" {
    extern lv_obj_t * ui_Screen_MainMenu;
}

/* ========================================================================
 *  LAN DEVICES — implementation
 *  HTTP-only local device control. No SSH. No OOM risk.
 * ======================================================================== */

static Preferences prefs;
static const char *NVS_NS = "lan_dev";

static lan_device_t s_devices[64];      // max 64 entities
static int s_device_count = 0;

static lan_ha_server_t s_ha_servers[4]; // max 4 HA servers
static int s_ha_server_count = 0;

static lan_agent_t s_agents[4];         // max 4 agents
static int s_agent_count = 0;

/* ---------- mDNS ---------- */
static lv_timer_t *s_mdns_timer = NULL;
static bool s_mdns_started = false;

/* ---------- UI ---------- */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_scroll = NULL;
static lv_obj_t *s_close_btn = NULL;
static int s_next_row_y = 0;            // cursor for placing rows
static bool s_screen_open = false;

/* Forward decls */
static void lan_devices_ui_refresh(void);

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int find_ha_by_host_port(const char *host, uint16_t port) {
    for (int i = 0; i < s_ha_server_count; i++) {
        if (strcmp(s_ha_servers[i].host, host) == 0 && s_ha_servers[i].port == port)
            return i;
    }
    return -1;
}

static int find_agent_by_host_port(const char *host, uint16_t port) {
    for (int i = 0; i < s_agent_count; i++) {
        if (strcmp(s_agents[i].host, host) == 0 && s_agents[i].port == port)
            return i;
    }
    return -1;
}

static void device_reset(lan_device_t *d) {
    memset(d, 0, sizeof(*d));
    d->type = LAN_DEV_OTHER;
    d->controllable = false;
}

static void device_set_type(lan_device_t *d, const char *entity_id) {
    // Extract domain from "domain.entity_name"
    const char *dot = strchr(entity_id, '.');
    char domain[16] = "other";
    if (dot) {
        size_t dlen = dot - entity_id;
        if (dlen >= sizeof(domain)) dlen = sizeof(domain) - 1;
        memcpy(domain, entity_id, dlen);
        domain[dlen] = '\0';
    }
    strncpy(d->domain, domain, sizeof(d->domain) - 1);

    if (strcasecmp(domain, "switch") == 0)        d->type = LAN_DEV_SWITCH;
    else if (strcasecmp(domain, "light") == 0)    d->type = LAN_DEV_LIGHT;
    else if (strcasecmp(domain, "cover") == 0)    d->type = LAN_DEV_COVER;
    else if (strcasecmp(domain, "fan") == 0)      d->type = LAN_DEV_FAN;
    else if (strcasecmp(domain, "sensor") == 0)   d->type = LAN_DEV_SENSOR;
    else                                          d->type = LAN_DEV_OTHER;

    d->controllable = (d->type != LAN_DEV_SENSOR);
}

/* Friendly name: convert "kitchen_light" -> "Kitchen Light" */
static void pretty_name(const char *raw, char *out, size_t outsz) {
    if (outsz == 0) return;
    out[0] = '\0';
    size_t pos = 0;
    bool cap = true;
    for (size_t i = 0; i < outsz - 1 && raw[i]; i++) {
        char c = raw[i];
        if (c == '_') { cap = true; continue; }
        out[pos++] = cap ? (char)toupper(c) : (char)tolower(c);
        cap = false;
    }
    out[pos] = '\0';
}

/* Location from entity attributes */
static void extract_location(JsonObject &attrs, char *out, size_t outsz) {
    out[0] = '\0';
    if (attrs.containsKey("area_name")) {
        const char *area = attrs["area_name"] | "";
        strncpy(out, area, outsz - 1);
        out[outsz - 1] = '\0';
        return;
    }
    strncpy(out, "General", outsz - 1);
    out[outsz - 1] = '\0';
}

/* ------------------------------------------------------------------ */
/*  HTTP helpers                                                       */
/* ------------------------------------------------------------------ */

static bool ha_call_service(lan_ha_server_t *srv, const char *domain,
                            const char *service, const char *entity_id) {
    char url[256];
    snprintf(url, sizeof(url),
             "%s://%s:%u/api/services/%s/%s",
             srv->ssl ? "https" : "http", srv->host, srv->port,
             domain, service);

    if (WiFi.status() != WL_CONNECTED) return false;

    static HTTPClient http;
    static WiFiClientSecure client;
    http.end();
    client.setInsecure();
    http.begin(client, url);
    http.addHeader("Authorization", ("Bearer " + String(srv->token)).c_str());
    http.addHeader("Content-Type", "application/json");

    char body[128];
    snprintf(body, sizeof(body), "{\"entity_id\":\"%s\"}", entity_id);

    int code = http.POST((uint8_t *)body, strlen(body));
    http.end();
    return (code == 200);
}

static bool ha_get_states(lan_ha_server_t *srv, lan_device_t *out, int *out_count) {
    char url[256];
    snprintf(url, sizeof(url),
             "%s://%s:%u/api/states",
             srv->ssl ? "https" : "http", srv->host, srv->port);

    if (WiFi.status() != WL_CONNECTED) return false;

    static HTTPClient http;
    static WiFiClientSecure client;
    http.end();
    client.setInsecure();
    http.begin(client, url);
    http.addHeader("Authorization", ("Bearer " + String(srv->token)).c_str());

    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }

    static char jsonBuf[4096];
    WiFiClient *stream = http.getStreamPtr();
    size_t idx = 0;
    uint32_t deadline = millis() + 8000;
    while (millis() < deadline && idx < sizeof(jsonBuf) - 1) {
        if (!stream->connected()) break;
        size_t avail = stream->available();
        if (avail == 0) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
        size_t toRead = min(avail, sizeof(jsonBuf) - 1 - idx);
        int r = stream->read((uint8_t *)(jsonBuf + idx), toRead);
        if (r <= 0) break;
        idx += r;
    }
    jsonBuf[idx] = '\0';
    http.end();

    // ArduinoJson 7: JsonDocument grows as needed (no capacity arg in ctor)
    static JsonDocument doc;
    doc.clear();
    DeserializationError err = deserializeJson(doc, jsonBuf);
    if (err) {
        log_e("[LAN_DEV] HA states parse failed: %s", err.c_str());
        return false;
    }

    JsonArray arr = doc.as<JsonArray>();
    int n = 0;
    int max_out = *out_count;
    for (JsonObject obj : arr) {
        if (n >= max_out) break;
        const char *entity_id = obj["entity_id"] | "";
        if (!entity_id || !*entity_id) continue;

        const char *state = obj["state"] | "unknown";
        JsonObject attrs = obj["attributes"].as<JsonObject>();

        strncpy(out[n].entity_id, entity_id, sizeof(out[n].entity_id) - 1);
        out[n].entity_id[sizeof(out[n].entity_id) - 1] = '\0';
        strncpy(out[n].state, state, sizeof(out[n].state) - 1);
        out[n].state[sizeof(out[n].state) - 1] = '\0';

        const char *fname = obj["attributes"]["friendly_name"] | "";
        const char *dot = strchr(entity_id, '.');
        const char *base = dot ? dot + 1 : entity_id;
        if (*fname) {
            strncpy(out[n].name, fname, sizeof(out[n].name) - 1);
        } else {
            pretty_name(base, out[n].name, sizeof(out[n].name));
        }
        out[n].name[sizeof(out[n].name) - 1] = '\0';

        extract_location(attrs, out[n].location, sizeof(out[n].location));
        device_set_type(&out[n], entity_id);

        if (attrs.containsKey("brightness"))
            out[n].brightness = (uint8_t)attrs["brightness"].as<int>();
        if (attrs.containsKey("current_temperature"))
            out[n].temperature = attrs["current_temperature"].as<float>();
        if (attrs.containsKey("current_humidity"))
            out[n].humidity = attrs["current_humidity"].as<float>();

        n++;
    }
    *out_count = n;
    return true;
}

/* ------------------------------------------------------------------ */
/*  mDNS discovery (Home Assistant)                                    */
/* ------------------------------------------------------------------ */

static void mdns_browse_cb(lv_timer_t *t) {
    if (WiFi.status() != WL_CONNECTED) return;

    int n = MDNS.queryService("home-assistant", "tcp");
    if (n <= 0) return;

    for (int i = 0; i < n; i++) {
        String hostname = MDNS.hostname(i);
        uint16_t port = MDNS.port(i);

        char host[64];
        hostname.toCharArray(host, sizeof(host));

        if (find_ha_by_host_port(host, port) >= 0) continue;

        if (s_ha_server_count >= 4) continue;

        lan_ha_server_t *s = &s_ha_servers[s_ha_server_count++];
        memset(s, 0, sizeof(*s));
        strncpy(s->host, host, sizeof(s->host) - 1);
        s->port = port;
        s->ssl = false;
        s->discovered = true;
        log_i("[LAN_DEV] Discovered HA at %s:%u", host, port);
    }
    lan_devices_save_config();
}

static void start_mdns(void) {
    if (s_mdns_started) return;
    if (!MDNS.begin("deskbar")) {
        log_e("[LAN_DEV] mDNS begin failed");
        return;
    }
    s_mdns_started = true;
    s_mdns_timer = lv_timer_create(mdns_browse_cb, 10000, NULL);
    log_i("[LAN_DEV] mDNS browser started");
}

/* ------------------------------------------------------------------ */
/*  Config load / save (NVS)                                           */
/* ------------------------------------------------------------------ */

void lan_devices_save_config(void) {
    prefs.begin(NVS_NS, false);
    prefs.putUChar("ha_count", s_ha_server_count);
    prefs.putUChar("agent_count", s_agent_count);
    for (int i = 0; i < s_ha_server_count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "ha%d_host", i);
        prefs.putString(key, s_ha_servers[i].host);
        snprintf(key, sizeof(key), "ha%d_port", i);
        prefs.putUShort(key, s_ha_servers[i].port);
        snprintf(key, sizeof(key), "ha%d_token", i);
        prefs.putString(key, s_ha_servers[i].token);
        snprintf(key, sizeof(key), "ha%d_ssl", i);
        prefs.putBool(key, s_ha_servers[i].ssl);
    }
    for (int i = 0; i < s_agent_count; i++) {
        char key[32];
        snprintf(key, sizeof(key), "ag%d_name", i);
        prefs.putString(key, s_agents[i].name);
        snprintf(key, sizeof(key), "ag%d_host", i);
        prefs.putString(key, s_agents[i].host);
        snprintf(key, sizeof(key), "ag%d_port", i);
        prefs.putUShort(key, s_agents[i].port);
        snprintf(key, sizeof(key), "ag%d_path", i);
        prefs.putString(key, s_agents[i].base_path);
    }
    prefs.end();
}

void lan_devices_load_config(void) {
    prefs.begin(NVS_NS, true);
    s_ha_server_count = prefs.getUChar("ha_count", 0);
    s_agent_count = prefs.getUChar("agent_count", 0);
    for (int i = 0; i < s_ha_server_count && i < 4; i++) {
        lan_ha_server_t *s = &s_ha_servers[i];
        memset(s, 0, sizeof(*s));
        char key[32];
        snprintf(key, sizeof(key), "ha%d_host", i);
        String h = prefs.getString(key, "");
        strncpy(s->host, h.c_str(), sizeof(s->host) - 1);
        snprintf(key, sizeof(key), "ha%d_port", i);
        s->port = prefs.getUShort(key, 8123);
        snprintf(key, sizeof(key), "ha%d_token", i);
        String tk = prefs.getString(key, "");
        strncpy(s->token, tk.c_str(), sizeof(s->token) - 1);
        snprintf(key, sizeof(key), "ha%d_ssl", i);
        s->ssl = prefs.getBool(key, false);
        s->discovered = true;
    }
    for (int i = 0; i < s_agent_count && i < 4; i++) {
        lan_agent_t *a = &s_agents[i];
        memset(a, 0, sizeof(*a));
        char key[32];
        snprintf(key, sizeof(key), "ag%d_name", i);
        String n = prefs.getString(key, "");
        strncpy(a->name, n.c_str(), sizeof(a->name) - 1);
        snprintf(key, sizeof(key), "ag%d_host", i);
        String h = prefs.getString(key, "");
        strncpy(a->host, h.c_str(), sizeof(a->host) - 1);
        snprintf(key, sizeof(key), "ag%d_port", i);
        a->port = prefs.getUShort(key, 80);
        snprintf(key, sizeof(key), "ag%d_path", i);
        String p = prefs.getString(key, "/api");
        strncpy(a->base_path, p.c_str(), sizeof(a->base_path) - 1);
    }
    prefs.end();
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void lan_devices_init(void) {
    lan_devices_load_config();
    s_device_count = 0;
    if (WiFi.status() == WL_CONNECTED) {
        start_mdns();
    }
}

void lan_devices_refresh(void) {
    s_device_count = 0;
    for (int i = 0; i < s_device_count; i++) device_reset(&s_devices[i]);

    for (int i = 0; i < s_ha_server_count; i++) {
        if (!s_ha_servers[i].token[0]) continue;
        int got = 64 - s_device_count;
        if (ha_get_states(&s_ha_servers[i], s_devices + s_device_count, &got)) {
            s_device_count += got;
            break;
        }
    }
    log_i("[LAN_DEV] Refresh: %d devices", s_device_count);
}

int lan_devices_count(void) { return s_device_count; }
const lan_device_t *lan_devices_get(int idx) {
    if (idx < 0 || idx >= s_device_count) return NULL;
    return &s_devices[idx];
}

bool lan_devices_toggle(const char *entity_id) {
    // Extract domain from entity_id prefix
    const char *dot = strchr(entity_id, '.');
    char domain[16] = "switch";
    if (dot) {
        size_t dlen = dot - entity_id;
        if (dlen >= sizeof(domain)) dlen = sizeof(domain) - 1;
        memcpy(domain, entity_id, dlen);
        domain[dlen] = '\0';
    }

    for (int i = 0; i < s_ha_server_count; i++) {
        if (!s_ha_servers[i].token[0]) continue;

        // Determine the service call based on domain
        const char *service = "toggle";
        if (strcasecmp(domain, "light") == 0 ||
            strcasecmp(domain, "fan") == 0) {
            // Check current state to pick turn_on/turn_off
            bool is_on = false;
            for (int j = 0; j < s_device_count; j++) {
                if (strcmp(s_devices[j].entity_id, entity_id) == 0) {
                    is_on = (strcmp(s_devices[j].state, "on") == 0);
                    break;
                }
            }
            service = is_on ? "turn_off" : "turn_on";
        } else if (strcasecmp(domain, "cover") == 0) {
            bool is_open = false;
            for (int j = 0; j < s_device_count; j++) {
                if (strcmp(s_devices[j].entity_id, entity_id) == 0) {
                    is_open = (strcmp(s_devices[j].state, "open") == 0);
                    break;
                }
            }
            service = is_open ? "close_cover" : "open_cover";
        }

        if (ha_call_service(&s_ha_servers[i], domain, service, entity_id)) {
            // Optimistic state flip
            for (int j = 0; j < s_device_count; j++) {
                if (strcmp(s_devices[j].entity_id, entity_id) == 0) {
                    if (strcasecmp(domain, "light") == 0 ||
                        strcasecmp(domain, "fan") == 0) {
                        strncpy(s_devices[j].state,
                                strcmp(s_devices[j].state, "on") == 0 ? "off" : "on",
                                sizeof(s_devices[j].state) - 1);
                    } else if (strcasecmp(domain, "cover") == 0) {
                        strncpy(s_devices[j].state,
                                strcmp(s_devices[j].state, "open") == 0 ? "closed" : "open",
                                sizeof(s_devices[j].state) - 1);
                    } else {
                        strncpy(s_devices[j].state,
                                strcmp(s_devices[j].state, "on") == 0 ? "off" : "on",
                                sizeof(s_devices[j].state) - 1);
                    }
                    break;
                }
            }
            return true;
        }
    }
    return false;
}

bool lan_devices_set_brightness(const char *entity_id, uint8_t brightness) {
    for (int i = 0; i < s_ha_server_count; i++) {
        if (!s_ha_servers[i].token[0]) continue;
        char url[256];
        snprintf(url, sizeof(url),
                 "%s://%s:%u/api/services/light/turn_on",
                 s_ha_servers[i].ssl ? "https" : "http",
                 s_ha_servers[i].host, s_ha_servers[i].port);
        if (WiFi.status() != WL_CONNECTED) return false;
        static HTTPClient http;
        static WiFiClientSecure client;
        http.end();
        client.setInsecure();
        http.begin(client, url);
        http.addHeader("Authorization", ("Bearer " + String(s_ha_servers[i].token)).c_str());
        http.addHeader("Content-Type", "application/json");
        char body[160];
        snprintf(body, sizeof(body),
                 "{\"entity_id\":\"%s\",\"brightness\":%u}",
                 entity_id, brightness);
        int code = http.POST((uint8_t *)body, strlen(body));
        http.end();
        return (code == 200);
    }
    return false;
}

void lan_devices_add_ha_server(const char *host, uint16_t port,
                               const char *token, bool ssl) {
    if (find_ha_by_host_port(host, port) >= 0) return;
    if (s_ha_server_count >= 4) return;
    lan_ha_server_t *s = &s_ha_servers[s_ha_server_count++];
    memset(s, 0, sizeof(*s));
    strncpy(s->host, host, sizeof(s->host) - 1);
    s->port = port;
    strncpy(s->token, token, sizeof(s->token) - 1);
    s->ssl = ssl;
    s->discovered = false;
    lan_devices_save_config();
}

void lan_devices_remove_ha_server(int idx) {
    if (idx < 0 || idx >= s_ha_server_count) return;
    for (int i = idx; i < s_ha_server_count - 1; i++)
        s_ha_servers[i] = s_ha_servers[i + 1];
    s_ha_server_count--;
    lan_devices_save_config();
}

int lan_devices_ha_server_count() { return s_ha_server_count; }
const lan_ha_server_t *lan_devices_ha_server_get(int idx) {
    if (idx < 0 || idx >= s_ha_server_count) return NULL;
    return &s_ha_servers[idx];
}

void lan_devices_add_agent(const char *name, const char *host,
                           uint16_t port, const char *base_path) {
    if (find_agent_by_host_port(host, port) >= 0) return;
    if (s_agent_count >= 4) return;
    lan_agent_t *a = &s_agents[s_agent_count++];
    memset(a, 0, sizeof(*a));
    strncpy(a->name, name, sizeof(a->name) - 1);
    strncpy(a->host, host, sizeof(a->host) - 1);
    a->port = port;
    strncpy(a->base_path, base_path, sizeof(a->base_path) - 1);
    lan_devices_save_config();
}

void lan_devices_remove_agent(int idx) {
    if (idx < 0 || idx >= s_agent_count) return;
    for (int i = idx; i < s_agent_count - 1; i++)
        s_agents[i] = s_agents[i + 1];
    s_agent_count--;
    lan_devices_save_config();
}

int lan_devices_agent_count() { return s_agent_count; }
const lan_agent_t *lan_devices_agent_get(int idx) {
    if (idx < 0 || idx >= s_agent_count) return NULL;
    return &s_agents[idx];
}

/* ------------------------------------------------------------------ */
/*  UI screen                                                          */
/* ------------------------------------------------------------------ */

static void on_close_click(lv_event_t *e) {
    lan_devices_close();
}

static void on_device_toggle(lv_event_t *e) {
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_device_count) return;
    const lan_device_t *d = &s_devices[idx];
    if (!d->controllable) return;
    lan_devices_toggle(d->entity_id);
    lan_devices_ui_refresh();
}

static void add_device_row(lv_obj_t *parent, const lan_device_t *d, int y) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, 608, 56);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x161C28), 0);
    lv_obj_set_style_bg_opa(row, 255, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(0x232D3F), 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_pad_all(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 8, y);

    // Icon (left)
    lv_obj_t *icon = lv_label_create(row);
    const char *ic = "⚙";
    switch (d->type) {
        case LAN_DEV_SWITCH: ic = "🔌"; break;
        case LAN_DEV_LIGHT:  ic = "💡"; break;
        case LAN_DEV_COVER:  ic = "🪟"; break;
        case LAN_DEV_FAN:    ic = "🌀"; break;
        case LAN_DEV_SENSOR: ic = "📊"; break;
        default:             ic = "⚙"; break;
    }
    lv_label_set_text(icon, ic);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);

    // Name + location (center)
    lv_obj_t *lbl_name = lv_label_create(row);
    lv_label_set_text(lbl_name, d->name);
    lv_obj_set_style_text_color(lbl_name, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(lbl_name, &lv_font_montserrat_16, 0);
    lv_obj_align(lbl_name, LV_ALIGN_LEFT_MID, 46, -8);

    lv_obj_t *lbl_loc = lv_label_create(row);
    lv_label_set_text(lbl_loc, d->location);
    lv_obj_set_style_text_color(lbl_loc, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_loc, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_loc, LV_ALIGN_LEFT_MID, 46, 8);

    // State / toggle (right)
    if (d->controllable) {
        lv_obj_t *sw = lv_switch_create(row);
        lv_obj_set_size(sw, 52, 28);
        lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -10, 0);
        bool on = (strcasecmp(d->state, "on") == 0 ||
                   strcasecmp(d->state, "open") == 0);
        if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, on_device_toggle, LV_EVENT_VALUE_CHANGED,
                            (void *)(intptr_t)(d - s_devices));

        lv_obj_t *lbl_state = lv_label_create(row);
        lv_label_set_text(lbl_state, on ? "ON" : "OFF");
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(on ? 0x00FF88 : 0x888888), 0);
        lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_state, LV_ALIGN_RIGHT_MID, -70, 0);
    } else {
        lv_obj_t *lbl_state = lv_label_create(row);
        lv_label_set_text(lbl_state, d->state);
        lv_obj_set_style_text_color(lbl_state, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_text_font(lbl_state, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl_state, LV_ALIGN_RIGHT_MID, -10, 0);
    }
}

static void add_location_header(lv_obj_t *parent, const char *loc, int y) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, loc);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_opa(lbl, 200, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 16, y);
}

static void lan_devices_ui_refresh(void) {
    if (!s_scroll) return;

    lv_obj_clean(s_scroll);
    s_next_row_y = 12;

    // Title bar
    lv_obj_t *title = lv_label_create(s_scroll);
    lv_label_set_text(title, "LAN Devices");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, s_next_row_y);
    s_next_row_y += 40;

    // Close button (top right)
    s_close_btn = lv_btn_create(s_scroll);
    lv_obj_set_size(s_close_btn, 48, 48);
    lv_obj_align(s_close_btn, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_set_style_bg_color(s_close_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(s_close_btn, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(s_close_btn, 2, 0);
    lv_obj_set_style_radius(s_close_btn, 10, 0);
    lv_obj_add_event_cb(s_close_btn, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_lbl = lv_label_create(s_close_btn);
    lv_label_set_text(close_lbl, "✕");
    lv_obj_center(close_lbl);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xFFFFFF), 0);

    // Empty state: no HA servers configured or no devices found
    if (s_ha_server_count == 0) {
        lv_obj_t *empty = lv_label_create(s_scroll);
        lv_label_set_text(empty, "No Home Assistant servers configured.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t *hint = lv_label_create(s_scroll);
        lv_label_set_text(hint, "Serial CLI: lan add <host> <port> <token> [ssl]");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(hint, 600);

        lv_obj_set_size(s_scroll, 640, 172);
        lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
        return;
    }

    if (s_device_count == 0) {
        lv_obj_t *empty = lv_label_create(s_scroll);
        lv_label_set_text(empty, "No devices found on LAN.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, -20);

        lv_obj_t *hint = lv_label_create(s_scroll);
        lv_label_set_text(hint, "Check HA server token, then tap again to refresh.");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 10);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(hint, 600);

        lv_obj_set_size(s_scroll, 640, 172);
        lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
        return;
    }

    // Group by location
    const char *last_loc = NULL;

    for (int i = 0; i < s_device_count; i++) {
        const lan_device_t *d = &s_devices[i];
        if (!last_loc || strcmp(d->location, last_loc) != 0) {
            last_loc = d->location;
            add_location_header(s_scroll, d->location, s_next_row_y);
            s_next_row_y += 26;
        }
        add_device_row(s_scroll, d, s_next_row_y);
        s_next_row_y += 60;
    }

    lv_obj_set_size(s_scroll, 640, 172);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
}

void lan_devices_open(void) {
    if (s_screen_open) return;
    s_screen_open = true;

    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, 640, 172);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0A0C10), 0);
    lv_obj_set_style_bg_opa(s_screen, 255, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_scroll = lv_obj_create(s_screen);
    lv_obj_set_size(s_scroll, 640, 172);
    lv_obj_set_align(s_scroll, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_scroll, lv_color_hex(0x0A0C10), 0);
    lv_obj_set_style_bg_opa(s_scroll, 0, 0);
    lv_obj_clear_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);

    lan_devices_refresh();
    lan_devices_ui_refresh();

    lv_scr_load_anim(s_screen, LV_SCR_LOAD_ANIM_OVER_TOP, 300, 0, false);
    log_i("[LAN_DEV] Screen opened");
}

void lan_devices_close(void) {
    if (!s_screen_open) return;
    s_screen_open = false;

    lv_scr_load_anim(ui_Screen_MainMenu, LV_SCR_LOAD_ANIM_MOVE_TOP, 300, 0, false);
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen = NULL;
    }
    s_scroll = NULL;
    s_close_btn = NULL;
    log_i("[LAN_DEV] Screen closed, back to menu");
}
