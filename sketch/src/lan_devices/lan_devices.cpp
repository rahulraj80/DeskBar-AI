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

extern lv_obj_t * ui_Screen_MainMenu;

/* ========================================================================
 *  LAN DEVICES — implementation with icon-based UI (like WiFi settings)
 * ======================================================================== */

static Preferences prefs;
static const char *NVS_NS = "lan_dev";

static lan_device_t s_devices[64];
static int s_device_count = 0;
static int s_srv_state[4] = {0, 0, 0, 0};  // per server: 0 unknown, 1 connected, 2 failed
static int s_refresh_state = 0;  // 0 none, 1 ok, 2 failed

static lan_ha_server_t s_ha_servers[4];
static int s_ha_server_count = 0;

static lan_agent_t s_agents[4];
static int s_agent_count = 0;

/* Temporary storage for multi-step keyboard flows */
static char s_temp_host[128];
static uint16_t s_temp_port;
static char s_temp_user[96];
static char s_temp_agent_name[64];
static char s_temp_agent_host[128];
static uint16_t s_temp_agent_port;

/* ---------- mDNS ---------- */
static lv_timer_t *s_mdns_timer = NULL;
static bool s_mdns_started = false;

/* UI state */
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_scroll = NULL;
static lv_obj_t *s_close_btn = NULL;
static bool s_screen_open = false;

static lv_obj_t *s_icon_refresh = NULL;
static lv_obj_t *s_icon_add = NULL;
static lv_obj_t *s_icon_config = NULL;

static lv_obj_t *s_kb_panel = NULL;
static volatile bool s_job_running = false, s_job_done = false;
static bool s_kb_visible(void) { return s_kb_panel && !lv_obj_has_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN); }
static lv_obj_t *s_kb_textarea = NULL;
static lv_obj_t *s_kb_keyboard = NULL;
static lv_obj_t *s_kb_done_btn = NULL;
static lv_obj_t *s_kb_title = NULL;
static lv_obj_t *s_kb_cancel_btn = NULL;
static lv_obj_t *s_kb_eye_btn = NULL;
static lv_obj_t *s_kb_eye_lbl = NULL;

static enum {
    KB_NONE,
    KB_ADD_HOST,
    KB_ADD_PORT,
    KB_ADD_USER,
    KB_ADD_PASS,
    KB_ADD_TOKEN,
    KB_AGENT_NAME,
    KB_AGENT_HOST,
    KB_AGENT_PORT,
    KB_AGENT_PATH
} s_kb_mode = KB_NONE;

static void lan_devices_ui_refresh(void);
static bool lan_screen_alive(void);


/* ========================================================================
 *  Helpers
 * ======================================================================== */

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

static bool has_creds(const lan_ha_server_t *s) { return s->token[0] || s->refresh[0]; }

static void device_reset(lan_device_t *d) {
    memset(d, 0, sizeof(*d));
    d->type = LAN_DEV_OTHER;
    d->controllable = false;
}

static void device_set_type(lan_device_t *d, const char *entity_id) {
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

/* ========================================================================
 *  HTTP helpers
 * ======================================================================== */

static const char *HA_CLIENT_ID = "http://deskbar.local/";

static int ha_http_impl(const lan_ha_server_t *srv, const char *method, const char *path,
                        const char *ctype, const String &body, const char *bearer,
                        String &resp) {
    if (WiFi.status() != WL_CONNECTED) return -1;
    char url[300];
    snprintf(url, sizeof(url), "%s://%s:%u%s",
             srv->ssl ? "https" : "http", srv->host, srv->port, path);

    static HTTPClient http;
    static WiFiClient plain;
    static WiFiClientSecure secure;
    http.end();
    if (srv->ssl) {
        secure.setInsecure();
        http.begin(secure, url);
    } else {
        http.begin(plain, url);
    }
    http.setTimeout(8000);
    if (bearer && *bearer) http.addHeader("Authorization", String("Bearer ") + bearer);
    if (ctype) http.addHeader("Content-Type", ctype);
    int code = (strcmp(method, "GET") == 0)
                   ? http.GET()
                   : http.sendRequest(method, (uint8_t *)body.c_str(), body.length());
    if (code > 0) resp = http.getString();
    http.end();
    return code;
}

/* The LVGL (6 KB) and serial CLI tasks have small stacks; HTTPClient needs more.
 * Run every request on a short-lived worker task and wait for it. */
struct HaJob {
    const lan_ha_server_t *srv; const char *method; const char *path;
    const char *ctype; const String *body; const char *bearer; String *resp;
    int code; volatile bool done;
};

static int ha_http(const lan_ha_server_t *srv, const char *method, const char *path,
                   const char *ctype, const String &body, const char *bearer,
                   String &resp) {
    if (strcmp(pcTaskGetName(NULL), "lan_job") == 0)
        return ha_http_impl(srv, method, path, ctype, body, bearer, resp);
    HaJob job = {srv, method, path, ctype, &body, bearer, &resp, -1, false};
    BaseType_t ok = xTaskCreatePinnedToCore([](void *p) {
        HaJob *j = (HaJob *)p;
        j->code = ha_http_impl(j->srv, j->method, j->path, j->ctype, *j->body, j->bearer, *j->resp);
        j->done = true;
        vTaskDelete(NULL);
    }, "lan_net", 12 * 1024, &job, 3, NULL, 1);
    if (ok != pdPASS) return ha_http_impl(srv, method, path, ctype, body, bearer, resp);
    while (!job.done) vTaskDelay(pdMS_TO_TICKS(10));
    return job.code;
}

/* Exchange an auth code or refresh token for an access token. */
static bool ha_token_request(lan_ha_server_t *srv, const char *form, bool *got_refresh) {
    String resp;
    int code = ha_http(srv, "POST", "/auth/token",
                       "application/x-www-form-urlencoded", String(form), NULL, resp);
    if (code != 200) {
        log_e("[LAN_DEV] /auth/token -> %d", code);
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    const char *at = doc["access_token"] | "";
    if (!*at) return false;
    strncpy(srv->token, at, sizeof(srv->token) - 1);
    srv->token[sizeof(srv->token) - 1] = '\0';
    const char *rt = doc["refresh_token"] | "";
    if (*rt) {
        strncpy(srv->refresh, rt, sizeof(srv->refresh) - 1);
        srv->refresh[sizeof(srv->refresh) - 1] = '\0';
        if (got_refresh) *got_refresh = true;
    }
    return true;
}

static bool ha_refresh_access(lan_ha_server_t *srv) {
    if (!srv->refresh[0]) return false;
    String form = String("grant_type=refresh_token&client_id=") + HA_CLIENT_ID +
                  "&refresh_token=" + srv->refresh;
    return ha_token_request(srv, form.c_str(), NULL);
}

/* Authenticated request; transparently refreshes an expired access token once. */
static int ha_authed(lan_ha_server_t *srv, const char *method, const char *path,
                     const String &body, String &resp) {
    if (!srv->token[0] && !ha_refresh_access(srv)) return -401;
    int code = ha_http(srv, method, path, "application/json", body, srv->token, resp);
    if (code == 401 && srv->refresh[0] && ha_refresh_access(srv))
        code = ha_http(srv, method, path, "application/json", body, srv->token, resp);
    return code;
}

static bool ha_call_service(lan_ha_server_t *srv, const char *domain,
                            const char *service, const char *entity_id,
                            int brightness = -1) {
    char path[96];
    snprintf(path, sizeof(path), "/api/services/%s/%s", domain, service);
    JsonDocument b;
    b["entity_id"] = entity_id;
    if (brightness >= 0) b["brightness"] = brightness;
    String body, resp;
    serializeJson(b, body);
    return ha_authed(srv, "POST", path, body, resp) == 200;
}

static int cmp_devices(const void *a, const void *b) {
    const lan_device_t *x = (const lan_device_t *)a, *y = (const lan_device_t *)b;
    int c = strcmp(x->location, y->location);
    return c ? c : strcmp(x->name, y->name);
}

static void fill_device(lan_device_t *d, const char *id, const char *name,
                        const char *state, const char *area, int brightness,
                        const char *unit) {
    device_reset(d);
    strncpy(d->entity_id, id, sizeof(d->entity_id) - 1);
    strncpy(d->state, state, sizeof(d->state) - 1);
    if (unit && *unit && strlen(state) + strlen(unit) + 1 < sizeof(d->state)) {
        strcat(d->state, " ");
        strcat(d->state, unit);
    }
    if (name && *name) strncpy(d->name, name, sizeof(d->name) - 1);
    else {
        const char *dot = strchr(id, '.');
        pretty_name(dot ? dot + 1 : id, d->name, sizeof(d->name));
    }
    strncpy(d->location, (area && *area) ? area : "General", sizeof(d->location) - 1);
    device_set_type(d, id);
    d->brightness = (uint8_t)brightness;
}

/* Preferred path: one small server-side template that returns only the
 * controllable entities WITH their area (room). /api/states carries no area
 * and can be hundreds of KB on a real install. */
static bool ha_get_via_template(lan_ha_server_t *srv, lan_device_t *out, int *out_count) {
    static const char *TPL =
        "{% set ns = namespace(l=[]) %}"
        "{% for s in states if s.domain in ['light','switch','cover','fan'] "
        "or (s.domain == 'sensor' and s.attributes.device_class in ['temperature','humidity']) %}"
        "{% set ns.l = ns.l + [{'i': s.entity_id, 'n': s.name, 's': s.state, "
        "'a': area_name(s.entity_id) or 'General', "
        "'b': s.attributes.brightness or 0, 'u': s.attributes.unit_of_measurement or ''}] %}"
        "{% endfor %}{{ ns.l[:64] | tojson }}";
    JsonDocument req;
    req["template"] = TPL;
    String body, resp;
    serializeJson(req, body);
    int code = ha_authed(srv, "POST", "/api/template", body, resp);
    if (code != 200) {
        log_w("[LAN_DEV] /api/template -> %d (falling back to /api/states)", code);
        return false;
    }
    JsonDocument doc;
    if (deserializeJson(doc, resp)) return false;
    int n = 0, max_out = *out_count;
    for (JsonObject o : doc.as<JsonArray>()) {
        if (n >= max_out) break;
        fill_device(&out[n], o["i"] | "", o["n"] | "", o["s"] | "unknown",
                    o["a"] | "General", o["b"] | 0, o["u"] | "");
        n++;
    }
    qsort(out, n, sizeof(lan_device_t), cmp_devices);
    *out_count = n;
    return true;
}

static bool ha_get_states(lan_ha_server_t *srv, lan_device_t *out, int *out_count) {
    if (ha_get_via_template(srv, out, out_count)) return true;

    // Fallback (non-admin users can't render templates): read /api/states and
    // keep only the fields and domains we show.
    String resp;
    int code = ha_authed(srv, "GET", "/api/states", "", resp);
    if (code != 200) return false;

    JsonDocument filter;
    filter[0]["entity_id"] = true;
    filter[0]["state"] = true;
    filter[0]["attributes"]["friendly_name"] = true;
    filter[0]["attributes"]["brightness"] = true;
    filter[0]["attributes"]["device_class"] = true;
    filter[0]["attributes"]["unit_of_measurement"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, resp, DeserializationOption::Filter(filter));
    if (err) {
        log_e("[LAN_DEV] HA states parse failed: %s", err.c_str());
        return false;
    }
    int n = 0, max_out = *out_count;
    for (JsonObject o : doc.as<JsonArray>()) {
        if (n >= max_out) break;
        const char *id = o["entity_id"] | "";
        if (!*id) continue;
        char dom[16] = "";
        const char *dot = strchr(id, '.');
        if (dot && (size_t)(dot - id) < sizeof(dom)) { memcpy(dom, id, dot - id); dom[dot - id] = 0; }
        bool ok = !strcmp(dom, "light") || !strcmp(dom, "switch") || !strcmp(dom, "cover") ||
                  !strcmp(dom, "fan");
        if (!strcmp(dom, "sensor")) {
            const char *dc = o["attributes"]["device_class"] | "";
            ok = !strcmp(dc, "temperature") || !strcmp(dc, "humidity");
        }
        if (!ok) continue;
        fill_device(&out[n], id, o["attributes"]["friendly_name"] | "", o["state"] | "unknown",
                    "General", o["attributes"]["brightness"] | 0,
                    o["attributes"]["unit_of_measurement"] | "");
        n++;
    }
    qsort(out, n, sizeof(lan_device_t), cmp_devices);
    *out_count = n;
    return true;
}

bool lan_devices_login(const char *host, uint16_t port, const char *user,
                       const char *pass, bool ssl, char *err, size_t errsz) {
    auto fail = [&](const char *m) { snprintf(err, errsz, "%s", m); return false; };
    if (WiFi.status() != WL_CONNECTED) return fail("Wi-Fi is not connected");

    lan_ha_server_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    strncpy(tmp.host, host, sizeof(tmp.host) - 1);
    tmp.port = port;
    tmp.ssl = ssl;

    // 1. start the login flow
    JsonDocument j1;
    j1["client_id"] = HA_CLIENT_ID;
    j1["handler"][0] = "homeassistant";
    j1["handler"][1] = nullptr;
    j1["redirect_uri"] = String(HA_CLIENT_ID) + "?auth_callback=1";
    String body, resp;
    serializeJson(j1, body);
    int code = ha_http(&tmp, "POST", "/auth/login_flow", "application/json", body, NULL, resp);
    if (code <= 0) return fail("Cannot reach Home Assistant");
    if (code != 200) return fail("Home Assistant refused sign-in");
    JsonDocument r1;
    if (deserializeJson(r1, resp)) return fail("Unexpected reply from server");
    String flow = r1["flow_id"] | "";
    if (!flow.length()) return fail("Unexpected reply from server");

    // 2. submit credentials
    JsonDocument j2;
    j2["client_id"] = HA_CLIENT_ID;
    j2["username"] = user;
    j2["password"] = pass;
    body = "";
    serializeJson(j2, body);
    String path = "/auth/login_flow/" + flow;
    code = ha_http(&tmp, "POST", path.c_str(), "application/json", body, NULL, resp);
    if (code != 200) return fail("Sign-in failed");
    JsonDocument r2;
    if (deserializeJson(r2, resp)) return fail("Unexpected reply from server");
    const char *type = r2["type"] | "";
    if (!strcmp(type, "form")) {
        const char *step = r2["step_id"] | "";
        if (strcmp(step, "init") != 0) return fail("Two-factor login is not supported");
        return fail("Wrong username or password");
    }
    if (strcmp(type, "create_entry") != 0) return fail("Sign-in was not accepted");
    String authcode = r2["result"] | "";
    if (!authcode.length()) return fail("No auth code returned");

    // 3. exchange the code for tokens
    String form = String("grant_type=authorization_code&client_id=") + HA_CLIENT_ID +
                  "&code=" + authcode;
    bool got_refresh = false;
    if (!ha_token_request(&tmp, form.c_str(), &got_refresh) || !got_refresh)
        return fail("Could not get access token");

    int existing = find_ha_by_host_port(host, port);
    if (existing < 0) {
        if (s_ha_server_count >= 4) return fail("Server list is full");
        existing = s_ha_server_count++;
        memset(&s_ha_servers[existing], 0, sizeof(lan_ha_server_t));
        strncpy(s_ha_servers[existing].host, host, sizeof(s_ha_servers[existing].host) - 1);
        s_ha_servers[existing].port = port;
    }
    lan_ha_server_t *s = &s_ha_servers[existing];
    s->ssl = ssl;
    strncpy(s->token, tmp.token, sizeof(s->token) - 1);
    strncpy(s->refresh, tmp.refresh, sizeof(s->refresh) - 1);
    lan_devices_save_config();
    return true;
}

/* ========================================================================
 *  mDNS discovery
 * ======================================================================== */

/* mDNS queries block for ~3 s (queryService) and up to 2 s more per queryHost.
 * Doing that inside an LVGL timer froze the UI and made the keyboard laggy, so the
 * query runs on its own task; the LVGL timer only applies finished results. */
struct MdnsHit { char host[16]; uint16_t port; };
static MdnsHit s_mdns_hits[4];
static volatile int s_mdns_hit_count = 0;
static volatile bool s_mdns_running = false, s_mdns_ready = false;

static void mdns_task(void *) {
    int n = MDNS.queryService("home-assistant", "tcp");
    int cnt = 0;
    for (int i = 0; i < n && cnt < 4; i++) {
        IPAddress ip = MDNS.address(i);
        uint16_t port = MDNS.port(i);
        String hn = MDNS.hostname(i);
        if ((uint32_t)ip == 0) ip = MDNS.queryHost(hn, 2000);
        if ((uint32_t)ip == 0) continue;   // never store the unresolvable UUID hostname
        snprintf(s_mdns_hits[cnt].host, sizeof(s_mdns_hits[cnt].host), "%s", ip.toString().c_str());
        s_mdns_hits[cnt].port = port;
        cnt++;
    }
    s_mdns_hit_count = cnt;
    s_mdns_ready = true;
    vTaskDelete(NULL);
}

static void mdns_browse_cb(lv_timer_t *t) {
    if (WiFi.getMode() == WIFI_OFF) return;

    if (s_mdns_ready) {
        s_mdns_ready = false;
        s_mdns_running = false;
        bool added = false;
        for (int i = 0; i < s_mdns_hit_count; i++) {
            if (find_ha_by_host_port(s_mdns_hits[i].host, s_mdns_hits[i].port) >= 0) continue;
            if (s_ha_server_count >= 4) break;
            lan_ha_server_t *s = &s_ha_servers[s_ha_server_count++];
            memset(s, 0, sizeof(*s));
            strncpy(s->host, s_mdns_hits[i].host, sizeof(s->host) - 1);
            s->port = s_mdns_hits[i].port;
            s->discovered = true;
            added = true;
            Serial.printf("[LAN_DEV] Discovered HA at %s:%u (sign-in needed)\n", s->host, s->port);
        }
        if (added) {
            lan_devices_save_config();
            if (lan_screen_alive() && !s_job_running && !s_kb_visible()) lan_devices_ui_refresh();
        }
        return;
    }

    // Only browse while the LAN screen is open, no job is running and no
    // keyboard is up (keep the CPU free for typing).
    if (!lan_screen_alive() || s_mdns_running || s_job_running || s_kb_visible()) return;
    s_mdns_running = true;
    if (xTaskCreatePinnedToCore(mdns_task, "lan_mdns", 8 * 1024, NULL, 1, NULL, 1) != pdPASS)
        s_mdns_running = false;
}

static void start_mdns(void);

void lan_devices_mdns_debug(void) {
    Serial.printf("[LAN_DEV] WiFi.status=%d s_mdns_started=%d\n",
                  (int)WiFi.status(), s_mdns_started ? 1 : 0);
    if (!s_mdns_started) start_mdns();
    int n = MDNS.queryService("home-assistant", "tcp");
    Serial.printf("[LAN_DEV] queryService(home-assistant,tcp) -> %d\n", n);
    for (int i = 0; i < n; i++) {
        String hn = MDNS.hostname(i);
        IPAddress ip = MDNS.address(i);
        uint16_t port = MDNS.port(i);
        Serial.printf("  [%d] hostname=%s ip=%s port=%u\n",
                      i, hn.c_str(), ip.toString().c_str(), port);
    }
}

static void start_mdns(void) {
    if (s_mdns_started) return;
    if (!MDNS.begin("deskbar")) {
        Serial.println("[LAN_DEV] mDNS begin FAILED");
        return;
    }
    Serial.println("[LAN_DEV] mDNS responder started");
    s_mdns_started = true;
    s_mdns_timer = lv_timer_create(mdns_browse_cb, 10000, NULL);
    log_i("[LAN_DEV] mDNS browser started");
}

/* ========================================================================
 *  Config load / save (NVS)
 * ======================================================================== */

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
        snprintf(key, sizeof(key), "ha%d_ref", i);
        prefs.putString(key, s_ha_servers[i].refresh);
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
        snprintf(key, sizeof(key), "ha%d_ref", i);
        String rf = prefs.getString(key, "");
        strncpy(s->refresh, rf.c_str(), sizeof(s->refresh) - 1);
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

/* ========================================================================
 *  Public API
 * ======================================================================== */

void lan_devices_init(void) {
    lan_devices_load_config();
    s_device_count = 0;
    if (WiFi.status() == WL_CONNECTED) {
        start_mdns();
    }
}

static void lan_devices_refresh_net(void) {
    s_device_count = 0;
    for (int i = 0; i < s_device_count; i++) device_reset(&s_devices[i]);

    s_refresh_state = 0;
    for (int i = 0; i < 4; i++) s_srv_state[i] = 0;
    for (int i = 0; i < s_ha_server_count; i++) {
        if (!has_creds(&s_ha_servers[i])) continue;
        int got = 64 - s_device_count;
        if (ha_get_states(&s_ha_servers[i], s_devices + s_device_count, &got)) {
            s_device_count += got;
            s_refresh_state = 1;
            s_srv_state[i] = 1;
            break;
        }
        s_srv_state[i] = 2;
        s_refresh_state = 2;
    }
    Serial.printf("[LAN_DEV] Refresh: %d devices (task %s, stack free %u B)\n", s_device_count,
                  pcTaskGetName(NULL), (unsigned)uxTaskGetStackHighWaterMark(NULL) * 4);
}

void lan_devices_refresh(void) {
    // mDNS is started lazily: at boot Wi-Fi is not up yet when init() runs, so
    // start the browser the first time we know the link is live.
    if (WiFi.status() == WL_CONNECTED) start_mdns();
    lan_devices_refresh_net();
}

int lan_devices_count(void) { return s_device_count; }
const lan_device_t *lan_devices_get(int idx) {
    if (idx < 0 || idx >= s_device_count) return NULL;
    return &s_devices[idx];
}

bool lan_devices_toggle(const char *entity_id) {
    const char *dot = strchr(entity_id, '.');
    char domain[16] = "switch";
    if (dot) {
        size_t dlen = dot - entity_id;
        if (dlen >= sizeof(domain)) dlen = sizeof(domain) - 1;
        memcpy(domain, entity_id, dlen);
        domain[dlen] = '\0';
    }

    for (int i = 0; i < s_ha_server_count; i++) {
        if (!has_creds(&s_ha_servers[i])) continue;

        const char *service = "toggle";
        if (strcasecmp(domain, "light") == 0 ||
            strcasecmp(domain, "fan") == 0) {
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
        if (!has_creds(&s_ha_servers[i])) continue;
        return ha_call_service(&s_ha_servers[i], "light", "turn_on", entity_id, brightness);
    }
    return false;
}

void lan_devices_add_ha_server(const char *host, uint16_t port,
                               const char *token, bool ssl) {
    // If mDNS already discovered this host (empty token), just fill the token.
    int existing = find_ha_by_host_port(host, port);
    if (existing >= 0) {
        strncpy(s_ha_servers[existing].token, token, sizeof(s_ha_servers[existing].token) - 1);
        s_ha_servers[existing].token[sizeof(s_ha_servers[existing].token) - 1] = '\0';
        s_ha_servers[existing].refresh[0] = '\0';
        s_ha_servers[existing].ssl = ssl;
        lan_devices_save_config();
        return;
    }
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

/* ========================================================================
 *  Keyboard / text entry helpers (borrowed from WiFi settings pattern)
 * ======================================================================== */

/* Keyboard dialog, styled exactly like Settings -> Wi-Fi: a form panel on the
 * LEFT (title + textarea + OK/Cancel) and the keyboard on the RIGHT
 * (350 px wide, full height). Both live on the LAN screen and are re-used for
 * every step; the step just changes the title/placeholder. */
/* ---------- Keyboard layout: 3 tall key rows + a shorter bottom row ----------
 * lv_btnmatrix gives every row the same height, so the bottom row (keyboard, <,
 * space, >, OK) is a second lv_keyboard stacked under a 3-row main keyboard.
 * lv_keyboard_set_map() is global, so the 3-row maps are installed while the LAN
 * screen is open and the stock 4-row maps are restored on close. */
#define LANKB_BTN(w) (LV_BTNMATRIX_CTRL_POPOVER | (w))
#define LANKB_FLAGS (LV_BTNMATRIX_CTRL_NO_REPEAT | LV_BTNMATRIX_CTRL_CLICK_TRIG | LV_BTNMATRIX_CTRL_CHECKED)
#define LANKB_B1 LANKB_BTN(1)
#define LANKB_CK1 (LV_BTNMATRIX_CTRL_CHECKED | LANKB_BTN(1))

static const char *const lankb_lc3[] = {"1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", ""};
static const char *const lankb_uc3[] = {"1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", ""};
static const char *const lankb_sp3[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", ""};
static const lv_btnmatrix_ctrl_t lankb_ctrl_lc3[] = {
    LANKB_FLAGS | 5, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LV_BTNMATRIX_CTRL_CHECKED | 7,
    LANKB_FLAGS | 6, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LV_BTNMATRIX_CTRL_CHECKED | 7,
    LANKB_CK1, LANKB_CK1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_CK1, LANKB_CK1, LANKB_CK1};
static const lv_btnmatrix_ctrl_t lankb_ctrl_sp3[] = {
    LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LV_BTNMATRIX_CTRL_CHECKED | 2,
    LANKB_FLAGS | 2, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1,
    LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1};
static const char *const lankb_bottom[] = {LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const lv_btnmatrix_ctrl_t lankb_ctrl_bottom[] = {
    LANKB_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LANKB_FLAGS | 2};

/* Stock LVGL 4-row maps, restored when the LAN screen closes. */
static const char *const stock_lc[] = {"1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const char *const stock_uc[] = {"1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const char *const stock_sp[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "+", "&", "/", "*", "=", "%", "!", "?", "#", "<", ">", "\n",
    "\\", "@", "$", "(", ")", "{", "}", "[", "]", ";", "\"", "'", "\n",
    LV_SYMBOL_KEYBOARD, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
static const lv_btnmatrix_ctrl_t stock_ctrl_lc[] = {
    LANKB_FLAGS | 5, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LANKB_B1 + 3, LV_BTNMATRIX_CTRL_CHECKED | 7,
    LANKB_FLAGS | 6, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LANKB_B1 + 2, LV_BTNMATRIX_CTRL_CHECKED | 7,
    LANKB_CK1, LANKB_CK1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_CK1, LANKB_CK1, LANKB_CK1,
    LANKB_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LANKB_FLAGS | 2};
static const lv_btnmatrix_ctrl_t stock_ctrl_sp[] = {
    LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LV_BTNMATRIX_CTRL_CHECKED | 2,
    LANKB_FLAGS | 2, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1,
    LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1, LANKB_B1,
    LANKB_FLAGS | 2, LV_BTNMATRIX_CTRL_CHECKED | 2, 6, LV_BTNMATRIX_CTRL_CHECKED | 2, LANKB_FLAGS | 2};

static const int KB_W = 422;         // keyboard width (was 352)
static const int KB_MAIN_H = 136;    // 3 key rows, ~41 px each (was 38)
static const int KB_BOTTOM_H = 36;   // bottom row ~28 px (was 38)
static lv_obj_t *s_kb_cont = NULL;
static lv_obj_t *s_kb_bottom = NULL;

static void lankb_install_maps(lv_obj_t *kb) {
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char **)lankb_uc3, lankb_ctrl_lc3);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, (const char **)lankb_sp3, lankb_ctrl_sp3);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char **)lankb_lc3, lankb_ctrl_lc3);
}

static void lankb_restore_maps(lv_obj_t *kb) {
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_UPPER, (const char **)stock_uc, stock_ctrl_lc);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_SPECIAL, (const char **)stock_sp, stock_ctrl_sp);
    lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_TEXT_LOWER, (const char **)stock_lc, stock_ctrl_lc);
}

static void on_kb_done_click(lv_event_t *e);

static void kb_panel_hide(void) {
    if (s_kb_panel) lv_obj_add_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_kb_cont) lv_obj_add_flag(s_kb_cont, LV_OBJ_FLAG_HIDDEN);
    s_kb_mode = KB_NONE;
}

static void kb_cancel_cb(lv_event_t *e) { kb_panel_hide(); }

static void kb_panel_create(void) {
    if (s_kb_panel) return;

    // --- LEFT form panel (280 px, full height) ---
    s_kb_panel = lv_obj_create(s_screen);
    lv_obj_set_size(s_kb_panel, 640 - KB_W, 172);
    lv_obj_set_align(s_kb_panel, LV_ALIGN_TOP_LEFT);
    lv_obj_clear_flag(s_kb_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_kb_panel, 0, 0);
    lv_obj_set_style_bg_color(s_kb_panel, lv_color_hex(0x060c14), 0);
    lv_obj_set_style_bg_opa(s_kb_panel, 255, 0);
    lv_obj_set_style_border_color(s_kb_panel, lv_color_hex(0x00d2ff), 0);
    lv_obj_set_style_border_width(s_kb_panel, 2, 0);
    lv_obj_set_style_pad_all(s_kb_panel, 6, 0);

    s_kb_title = lv_label_create(s_kb_panel);
    lv_obj_set_width(s_kb_title, 640 - KB_W - 20);
    lv_label_set_long_mode(s_kb_title, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_kb_title, LV_ALIGN_TOP_LEFT, 2, 4);
    lv_obj_set_style_text_font(s_kb_title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_kb_title, lv_color_hex(0xFFFFFF), 0);

    s_kb_textarea = lv_textarea_create(s_kb_panel);
    lv_obj_set_size(s_kb_textarea, 640 - KB_W - 20, 40);
    lv_obj_align(s_kb_textarea, LV_ALIGN_TOP_LEFT, 2, 44);
    lv_textarea_set_one_line(s_kb_textarea, true);
    lv_obj_set_style_text_font(s_kb_textarea, &lv_font_montserrat_14, 0);

    s_kb_done_btn = lv_btn_create(s_kb_panel);
    lv_obj_set_size(s_kb_done_btn, 98, 40);
    lv_obj_align(s_kb_done_btn, LV_ALIGN_BOTTOM_LEFT, 2, -2);
    lv_obj_set_style_bg_color(s_kb_done_btn, lv_color_hex(0x0088cc), 0);
    lv_obj_set_style_bg_opa(s_kb_done_btn, 255, 0);
    lv_obj_t *done_lbl = lv_label_create(s_kb_done_btn);
    lv_label_set_text(done_lbl, "OK");
    lv_obj_center(done_lbl);
    lv_obj_set_style_text_font(done_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(done_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(s_kb_done_btn, [](lv_event_t *e) { on_kb_done_click(e); },
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *cancel_btn = lv_btn_create(s_kb_panel);
    s_kb_cancel_btn = cancel_btn;
    lv_obj_set_size(cancel_btn, 98, 40);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(cancel_btn, 1, 0);
    lv_obj_t *cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_center(cancel_lbl);
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(cancel_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(cancel_btn, kb_cancel_cb, LV_EVENT_CLICKED, NULL);

    // Show/hide-password toggle, only visible on the password step.
    s_kb_eye_btn = lv_btn_create(s_kb_panel);
    lv_obj_set_size(s_kb_eye_btn, 44, 40);
    lv_obj_align(s_kb_eye_btn, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(s_kb_eye_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(s_kb_eye_btn, lv_color_hex(0x888888), 0);
    lv_obj_set_style_border_width(s_kb_eye_btn, 1, 0);
    s_kb_eye_lbl = lv_label_create(s_kb_eye_btn);
    lv_label_set_text(s_kb_eye_lbl, LV_SYMBOL_EYE_CLOSE);
    lv_obj_center(s_kb_eye_lbl);
    lv_obj_set_style_text_font(s_kb_eye_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_kb_eye_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_event_cb(s_kb_eye_btn, [](lv_event_t *e) {
        bool hidden = lv_textarea_get_password_mode(s_kb_textarea);
        lv_textarea_set_password_mode(s_kb_textarea, !hidden);
        lv_label_set_text(s_kb_eye_lbl, hidden ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_kb_eye_btn, LV_OBJ_FLAG_HIDDEN);

    // --- RIGHT keyboard: container with the gradient, 3-row main keyboard on top
    //     and a shorter bottom row below ---
    s_kb_cont = lv_obj_create(s_screen);
    lv_obj_set_size(s_kb_cont, KB_W, 172);
    lv_obj_set_align(s_kb_cont, LV_ALIGN_TOP_RIGHT);
    lv_obj_clear_flag(s_kb_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(s_kb_cont, 0, 0);
    lv_obj_set_style_border_width(s_kb_cont, 0, 0);
    lv_obj_set_style_pad_all(s_kb_cont, 0, 0);
    lv_obj_set_style_bg_color(s_kb_cont, lv_color_hex(0x117200), 0);
    lv_obj_set_style_bg_opa(s_kb_cont, 255, 0);
    lv_obj_set_style_bg_grad_color(s_kb_cont, lv_color_hex(0x143201), 0);
    lv_obj_set_style_bg_grad_dir(s_kb_cont, LV_GRAD_DIR_VER, 0);

    s_kb_keyboard = lv_keyboard_create(s_kb_cont);
    lankb_install_maps(s_kb_keyboard);
    lv_keyboard_set_mode(s_kb_keyboard, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_set_size(s_kb_keyboard, KB_W, KB_MAIN_H);
    lv_obj_set_align(s_kb_keyboard, LV_ALIGN_TOP_LEFT);  // lv_keyboard defaults to bottom-aligned
    lv_obj_set_pos(s_kb_keyboard, 0, 0);
    lv_obj_set_style_bg_opa(s_kb_keyboard, 0, 0);
    lv_obj_set_style_border_width(s_kb_keyboard, 0, 0);
    lv_obj_set_style_pad_left(s_kb_keyboard, 4, 0);
    lv_obj_set_style_pad_right(s_kb_keyboard, 4, 0);
    lv_obj_set_style_pad_top(s_kb_keyboard, 4, 0);
    lv_obj_set_style_pad_bottom(s_kb_keyboard, 0, 0);
    lv_keyboard_set_textarea(s_kb_keyboard, s_kb_textarea);

    s_kb_bottom = lv_keyboard_create(s_kb_cont);
    lv_keyboard_set_map(s_kb_bottom, LV_KEYBOARD_MODE_USER_1, (const char **)lankb_bottom, lankb_ctrl_bottom);
    lv_keyboard_set_mode(s_kb_bottom, LV_KEYBOARD_MODE_USER_1);
    lv_obj_set_size(s_kb_bottom, KB_W, KB_BOTTOM_H);
    lv_obj_set_align(s_kb_bottom, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(s_kb_bottom, 0, KB_MAIN_H);
    lv_obj_set_style_bg_opa(s_kb_bottom, 0, 0);
    lv_obj_set_style_border_width(s_kb_bottom, 0, 0);
    lv_obj_set_style_pad_left(s_kb_bottom, 4, 0);
    lv_obj_set_style_pad_right(s_kb_bottom, 4, 0);
    lv_obj_set_style_pad_top(s_kb_bottom, 4, 0);
    lv_obj_set_style_pad_bottom(s_kb_bottom, 4, 0);
    lv_keyboard_set_textarea(s_kb_bottom, s_kb_textarea);
    lv_obj_add_event_cb(s_kb_bottom, [](lv_event_t *e) { on_kb_done_click(e); }, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_kb_bottom, kb_cancel_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_add_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_kb_cont, LV_OBJ_FLAG_HIDDEN);
}

static void kb_panel_show(const char *title_text, const char *placeholder, int max_len,
                          bool password = false) {
    kb_panel_create();
    lv_textarea_set_password_mode(s_kb_textarea, password);
    // Password step: OK | eye | Cancel; other steps: OK | Cancel.
    int bw = password ? 72 : 98;
    lv_obj_set_width(s_kb_done_btn, bw);
    lv_obj_set_width(s_kb_cancel_btn, bw);
    if (password) {
        lv_label_set_text(s_kb_eye_lbl, LV_SYMBOL_EYE_CLOSE);
        lv_obj_clear_flag(s_kb_eye_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_kb_eye_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_kb_title, title_text);
    lv_textarea_set_text(s_kb_textarea, "");
    lv_textarea_set_max_length(s_kb_textarea, max_len);
    lv_textarea_set_placeholder_text(s_kb_textarea, placeholder);

    // Numeric keyboard (stock 4-row, full height) for ports; split text keyboard otherwise.
    bool numeric = (max_len <= 6);
    if (numeric) {
        lv_obj_add_flag(s_kb_bottom, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_kb_keyboard, 172);
        lv_obj_set_style_pad_bottom(s_kb_keyboard, 4, 0);
    } else {
        lv_obj_clear_flag(s_kb_bottom, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_height(s_kb_keyboard, KB_MAIN_H);
        lv_obj_set_style_pad_bottom(s_kb_keyboard, 0, 0);
    }
    lv_keyboard_set_mode(s_kb_keyboard,
                         numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_keyboard_set_textarea(s_kb_keyboard, s_kb_textarea);
    lv_keyboard_set_textarea(s_kb_bottom, s_kb_textarea);

    lv_obj_clear_flag(s_kb_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_kb_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(s_kb_textarea, LV_STATE_FOCUSED);
}

/* ---------- Background job (never block the LVGL task) ---------- */
static struct {
    int kind;                 // 1 refresh, 2 login + refresh, 3 toggle + refresh
    char host[128]; uint16_t port;
    char user[96]; char pass[96];
    char entity[64];
    bool login_ok; char err[64];
} s_job;
static lv_timer_t *s_poll_timer = NULL;
static lv_obj_t *s_status_lbl = NULL;

/* The LAN screen can be deleted by someone else (screen transitions with auto-delete,
 * idle -> clock). Drop every pointer into it so no timer/job touches freed objects. */
static void lan_screen_forget(void) {
    s_screen_open = false;
    s_screen = NULL;
    s_scroll = NULL;
    s_close_btn = NULL;
    s_icon_refresh = NULL;
    s_icon_add = NULL;
    s_icon_config = NULL;
    s_kb_panel = NULL;
    s_kb_cont = NULL;
    s_kb_keyboard = NULL;
    s_kb_bottom = NULL;
    s_kb_textarea = NULL;
    s_kb_done_btn = NULL;
    s_kb_title = NULL;
    s_status_lbl = NULL;
    s_kb_cancel_btn = NULL;
    s_kb_eye_btn = NULL;
    s_kb_eye_lbl = NULL;
    s_kb_mode = KB_NONE;
}

static bool lan_screen_alive(void) {
    if (!s_screen_open || !s_screen || !s_scroll) return false;
    if (!lv_obj_is_valid(s_screen) || !lv_obj_is_valid(s_scroll)) {
        lan_screen_forget();
        return false;
    }
    return true;
}

static void lan_screen_deleted_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == s_screen) lan_screen_forget();
}

static void job_task(void *) {
    if (s_job.kind == 2) {
        s_job.login_ok = lan_devices_login(s_job.host, s_job.port, s_job.user, s_job.pass,
                                           false, s_job.err, sizeof(s_job.err));
        memset(s_job.pass, 0, sizeof(s_job.pass));
    } else if (s_job.kind == 3) {
        lan_devices_toggle(s_job.entity);
    }
    if (s_job.kind != 2 || s_job.login_ok) lan_devices_refresh_net();
    s_job_done = true;
    vTaskDelete(NULL);
}

static void job_poll_cb(lv_timer_t *t) {
    if (!s_job_done) return;
    s_job_done = false;
    s_job_running = false;
    s_poll_timer = NULL;
    lv_timer_del(t);
    if (!lan_screen_alive()) return;
    if (s_job.kind == 2) {
        if (!s_job.login_ok) {
            s_kb_mode = KB_ADD_USER;
            kb_panel_show(s_job.err, "username", 64);  // 2 lines max: title must not overlap the textarea
            return;
        }
        memset(s_job.user, 0, sizeof(s_job.user));
        kb_panel_hide();
    }
    lan_devices_ui_refresh();
}

static void job_start(int kind) {
    if (s_job_running) return;
    s_job.kind = kind;
    s_job_done = false;
    s_job_running = true;
    if (s_status_lbl) lv_label_set_text(s_status_lbl, kind == 2 ? "Signing in..." : "Loading...");
    if (xTaskCreatePinnedToCore(job_task, "lan_job", 12 * 1024, NULL, 3, NULL, 1) != pdPASS) {
        s_job_running = false;
        if (s_status_lbl) lv_label_set_text(s_status_lbl, "");
        return;
    }
    s_poll_timer = lv_timer_create(job_poll_cb, 150, NULL);
}

static void on_kb_done_click(lv_event_t *e) {
    if (!s_kb_textarea) return;

    const char *text = lv_textarea_get_text(s_kb_textarea);
    if (!text) return;
    // Only the username step may be left empty (= use an access token instead).
    if (!*text && s_kb_mode != KB_ADD_USER) return;

    switch (s_kb_mode) {
        case KB_ADD_HOST:
            strncpy(s_temp_host, text, sizeof(s_temp_host) - 1);
            s_kb_mode = KB_ADD_PORT;
            kb_panel_show("Home Assistant port", "8123", 5);
            break;
        case KB_ADD_PORT:
            s_temp_port = (uint16_t)atoi(text);
            if (s_temp_port == 0) s_temp_port = 8123;
            s_kb_mode = KB_ADD_USER;
            kb_panel_show("Home Assistant username\n(leave empty to paste a token)", "username", 64);
            break;
        case KB_ADD_USER:
            if (!*text) {
                s_kb_mode = KB_ADD_TOKEN;
                kb_panel_show("Long-lived access token", "eyJ...", 256);
            } else {
                strncpy(s_temp_user, text, sizeof(s_temp_user) - 1);
                s_kb_mode = KB_ADD_PASS;
                kb_panel_show("Home Assistant password", "password", 64, true);
            }
            break;
        case KB_ADD_PASS:
            strncpy(s_job.host, s_temp_host, sizeof(s_job.host) - 1);
            s_job.port = s_temp_port;
            strncpy(s_job.user, s_temp_user, sizeof(s_job.user) - 1);
            strncpy(s_job.pass, text, sizeof(s_job.pass) - 1);
            lv_label_set_text(s_kb_title, "Signing in...");
            memset(s_temp_user, 0, sizeof(s_temp_user));
            job_start(2);
            break;
        case KB_ADD_TOKEN:
            lan_devices_add_ha_server(s_temp_host, s_temp_port, text, false);
            kb_panel_hide();
            lan_devices_ui_refresh();
            job_start(1);
            break;
        case KB_AGENT_NAME:
            strncpy(s_temp_agent_name, text, sizeof(s_temp_agent_name) - 1);
            s_kb_mode = KB_AGENT_HOST;
            kb_panel_show("Agent host / IP", "192.168.3.60", 64);
            break;
        case KB_AGENT_HOST:
            strncpy(s_temp_agent_host, text, sizeof(s_temp_agent_host) - 1);
            s_kb_mode = KB_AGENT_PORT;
            kb_panel_show("Agent port", "8080", 5);
            break;
        case KB_AGENT_PORT:
            s_temp_agent_port = (uint16_t)atoi(text);
            if (s_temp_agent_port == 0) s_temp_agent_port = 8080;
            s_kb_mode = KB_AGENT_PATH;
            kb_panel_show("Agent base path", "/api", 64);
            break;
        case KB_AGENT_PATH:
            lan_devices_add_agent(s_temp_agent_name, s_temp_agent_host,
                                  s_temp_agent_port, text);
            kb_panel_hide();
            lan_devices_ui_refresh();
            break;
        default:
            break;
    }
}

/* ========================================================================
 *  Icon button callbacks
 * ======================================================================== */

static void on_close_click(lv_event_t *e) {
    lan_devices_close();
}

static void on_refresh_click(lv_event_t *e) {
    job_start(1);
}

static void start_signin(int idx) {
    strncpy(s_temp_host, s_ha_servers[idx].host, sizeof(s_temp_host) - 1);
    s_temp_port = s_ha_servers[idx].port;
    s_kb_mode = KB_ADD_USER;
    kb_panel_show("Sign in to Home Assistant\nUsername (empty = token)", "username", 64);
}

static void on_server_signin(lv_event_t *e) {
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < s_ha_server_count) start_signin((int)idx);
}

static void on_add_click(lv_event_t *e) {
    // A server found by mDNS but not signed in yet: skip the host/port steps.
    for (int i = 0; i < s_ha_server_count; i++) {
        if (has_creds(&s_ha_servers[i])) continue;
        strncpy(s_temp_host, s_ha_servers[i].host, sizeof(s_temp_host) - 1);
        s_temp_port = s_ha_servers[i].port;
        s_kb_mode = KB_ADD_USER;
        kb_panel_show("Sign in to Home Assistant\nUsername (empty = token)", "username", 64);
        return;
    }
    s_kb_mode = KB_ADD_HOST;
    kb_panel_show("Home Assistant host / IP", "192.168.3.50", 64);
}

static void on_config_click(lv_event_t *e) {
    // Agent flow (Pi / mobile / desktop HTTP agent)
    s_kb_mode = KB_AGENT_NAME;
    kb_panel_show("Agent name", "office-pi", 64);
}

static void on_device_toggle(lv_event_t *e) {
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_device_count) return;
    const lan_device_t *d = &s_devices[idx];
    if (!d->controllable) return;
    if (s_job_running) return;
    strncpy(s_job.entity, d->entity_id, sizeof(s_job.entity) - 1);
    job_start(3);
}

/* ========================================================================
 *  UI building
 * ======================================================================== */

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

    // Icon
    lv_obj_t *icon = lv_label_create(row);
    const char *ic = LV_SYMBOL_SETTINGS;
    switch (d->type) {
        case LAN_DEV_SWITCH: ic = LV_SYMBOL_POWER; break;
        case LAN_DEV_LIGHT:  ic = LV_SYMBOL_CHARGE; break;
        case LAN_DEV_COVER:  ic = LV_SYMBOL_UP; break;
        case LAN_DEV_FAN:    ic = LV_SYMBOL_LOOP; break;
        case LAN_DEV_SENSOR: ic = LV_SYMBOL_EYE_OPEN; break;
        default:             ic = LV_SYMBOL_SETTINGS; break;
    }
    lv_label_set_text(icon, ic);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);

    // Name + location
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

    // Toggle or state
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

static void add_header_icons(void) {
    // Top bar with title and action icons
    lv_obj_t *title = lv_label_create(s_scroll);
    lv_label_set_text(title, "LAN Devices");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 16, 12);

    s_status_lbl = lv_label_create(s_scroll);
    lv_label_set_text(s_status_lbl, s_job_running ? "Loading..." : "");
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x00E5FF), 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_LEFT, 16, 40);

    // Refresh icon
    s_icon_refresh = lv_btn_create(s_scroll);
    lv_obj_set_size(s_icon_refresh, 48, 48);
    lv_obj_align(s_icon_refresh, LV_ALIGN_TOP_RIGHT, -80, 12);
    lv_obj_set_style_bg_color(s_icon_refresh, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(s_icon_refresh, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(s_icon_refresh, 2, 0);
    lv_obj_set_style_radius(s_icon_refresh, 10, 0);
    lv_obj_add_event_cb(s_icon_refresh, on_refresh_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_refresh = lv_label_create(s_icon_refresh);
    lv_label_set_text(lbl_refresh, LV_SYMBOL_REFRESH);
    lv_obj_center(lbl_refresh);
    lv_obj_set_style_text_font(lbl_refresh, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_refresh, lv_color_hex(0xFFFFFF), 0);

    // Add server icon
    s_icon_add = lv_btn_create(s_scroll);
    lv_obj_set_size(s_icon_add, 48, 48);
    lv_obj_align(s_icon_add, LV_ALIGN_TOP_RIGHT, -136, 12);
    lv_obj_set_style_bg_color(s_icon_add, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(s_icon_add, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(s_icon_add, 2, 0);
    lv_obj_set_style_radius(s_icon_add, 10, 0);
    lv_obj_add_event_cb(s_icon_add, on_add_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_add = lv_label_create(s_icon_add);
    lv_label_set_text(lbl_add, LV_SYMBOL_PLUS);
    lv_obj_center(lbl_add);
    lv_obj_set_style_text_font(lbl_add, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_add, lv_color_hex(0xFFFFFF), 0);

    // Config/agents icon
    s_icon_config = lv_btn_create(s_scroll);
    lv_obj_set_size(s_icon_config, 48, 48);
    lv_obj_align(s_icon_config, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_set_style_bg_color(s_icon_config, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(s_icon_config, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(s_icon_config, 2, 0);
    lv_obj_set_style_radius(s_icon_config, 10, 0);
    lv_obj_add_event_cb(s_icon_config, on_config_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_config = lv_label_create(s_icon_config);
    lv_label_set_text(lbl_config, LV_SYMBOL_SETTINGS);
    lv_obj_center(lbl_config);
    lv_obj_set_style_text_font(lbl_config, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_config, lv_color_hex(0xFFFFFF), 0);

    // Close button
    lv_obj_t *close_btn = lv_btn_create(s_scroll);
    lv_obj_set_size(close_btn, 48, 48);
    lv_obj_align(close_btn, LV_ALIGN_TOP_RIGHT, -192, 12);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(close_btn, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(close_btn, 2, 0);
    lv_obj_set_style_radius(close_btn, 10, 0);
    lv_obj_add_event_cb(close_btn, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);
    lv_obj_set_style_text_font(close_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(close_lbl, lv_color_hex(0xFFFFFF), 0);
}

static int s_next_row_y = 0;

static void lan_devices_ui_refresh(void) {
    if (!lan_screen_alive()) return;

    lv_obj_clean(s_scroll);

    // Add header with action icons
    add_header_icons();

    s_next_row_y = 68; // below header icons

    // Empty state: no HA servers
    if (s_ha_server_count == 0) {
        lv_obj_t *empty = lv_label_create(s_scroll);
        lv_label_set_text(empty, "No Home Assistant servers configured.");
        lv_obj_set_style_text_color(empty, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_16, 0);
        lv_obj_set_width(empty, 620);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 2);

        lv_obj_t *hint = lv_label_create(s_scroll);
        lv_label_set_text(hint, "Tap + to sign in to your Home Assistant.");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x00E5FF), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_align(hint, LV_ALIGN_CENTER, 0, 34);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(hint, 600);

        lv_obj_set_size(s_scroll, 640, 172);
        lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
        return;
    }

    // One row per Home Assistant server: shows it is found, and lets the user sign in.
    for (int i = 0; i < s_ha_server_count; i++) {
        const lan_ha_server_t *srv = &s_ha_servers[i];
        bool signed_in = has_creds(srv);
        lv_obj_t *row = lv_obj_create(s_scroll);
        lv_obj_set_size(row, 608, 52);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x161C28), 0);
        lv_obj_set_style_bg_opa(row, 255, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x2A6FD6), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_align(row, LV_ALIGN_TOP_LEFT, 8, s_next_row_y);

        lv_obj_t *nm = lv_label_create(row);
        lv_label_set_text(nm, "Home Assistant");
        lv_obj_set_style_text_color(nm, lv_color_hex(0xFFFFFF), 0);
        lv_obj_set_style_text_font(nm, &lv_font_montserrat_16, 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 8, -9);

        char sub[96];
        int ss = (i < 4) ? s_srv_state[i] : 0;
        const char *st = !signed_in ? "not signed in"
                         : (ss == 2 ? "unreachable or sign-in failed"
                                    : (ss == 1 ? "connected" : "signed in"));
        bool bad = !signed_in || ss == 2;
        snprintf(sub, sizeof(sub), "%s:%u  -  %s", srv->host, srv->port, st);
        lv_obj_t *sb = lv_label_create(row);
        lv_label_set_text(sb, sub);
        lv_obj_set_style_text_color(sb, lv_color_hex(bad ? 0xFFB020 : 0x00E5A0), 0);
        lv_obj_set_style_text_font(sb, &lv_font_montserrat_14, 0);
        lv_obj_align(sb, LV_ALIGN_LEFT_MID, 8, 9);

        lv_obj_t *btn = lv_btn_create(row);
        lv_obj_set_size(btn, 130, 38);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -6, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x0088cc), 0);
        lv_obj_add_event_cb(btn, on_server_signin, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *bl = lv_label_create(btn);
        lv_label_set_text(bl, signed_in ? "Re-sign in" : "Sign in");
        lv_obj_center(bl);
        lv_obj_set_style_text_font(bl, &lv_font_montserrat_16, 0);
        s_next_row_y += 58;
    }

    // No devices but have servers
    if (s_device_count == 0) {
        lv_obj_t *hint = lv_label_create(s_scroll);
        lv_label_set_text(hint, !has_creds(&s_ha_servers[0]) ? "Sign in to see your lights and switches."
                                                            : "No lights, switches or sensors found.");
        lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
        lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 16, s_next_row_y + 2);

        lv_obj_set_size(s_scroll, 640, 172);
        lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
        return;
    }

    // List devices grouped by location
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

/* ========================================================================
 *  Screen open / close
 * ======================================================================== */

void lan_devices_open(void) {
    if (s_screen_open) return;
    s_screen_open = true;

    s_screen = lv_obj_create(NULL);
    lv_obj_add_event_cb(s_screen, lan_screen_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_set_size(s_screen, 640, 172);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0A0C10), 0);
    lv_obj_set_style_bg_opa(s_screen, 255, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_scroll = lv_obj_create(s_screen);
    lv_obj_set_size(s_scroll, 640, 172);
    lv_obj_set_align(s_scroll, LV_ALIGN_CENTER);
    lv_obj_set_style_bg_color(s_scroll, lv_color_hex(0x0A0C10), 0);
    lv_obj_set_style_bg_opa(s_scroll, 0, 0);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);

    if (WiFi.status() == WL_CONNECTED) start_mdns();
    lan_devices_ui_refresh();
    job_start(1);

    lv_scr_load_anim(s_screen, LV_SCR_LOAD_ANIM_OVER_TOP, 300, 0, false);
    log_i("[LAN_DEV] Screen opened");
}

void lan_devices_close(void) {
    if (!s_screen_open) return;
    s_screen_open = false;


    // Switch to the persistent MainMenu screen instantly so the LAN screen is
    // no longer active, then defer freeing it. This function runs from the
    // close button's own event handler (and from the CLI); deleting the screen
    // that owns that button mid-handler is a use-after-free (LoadProhibited).
    lv_scr_load(ui_Screen_MainMenu);
    if (s_screen) {
        lv_obj_del_async(s_screen);
        s_screen = NULL;
    }
    s_scroll = NULL;
    s_close_btn = NULL;
    s_icon_refresh = NULL;
    s_icon_add = NULL;
    s_icon_config = NULL;
    // These were children of s_screen; clear them so the next open recreates
    // them instead of touching freed objects.
    s_kb_panel = NULL;
    // Restore via the bottom keyboard (USER_1 mode, map unchanged) so the main
    // keyboard is not re-shaped right before it is deleted.
    if (s_kb_bottom) lankb_restore_maps(s_kb_bottom);
    s_kb_keyboard = NULL;
    s_kb_cont = NULL;
    s_kb_bottom = NULL;
    s_kb_textarea = NULL;
    s_kb_done_btn = NULL;
    s_kb_title = NULL;
    s_status_lbl = NULL;
    s_kb_cancel_btn = NULL;
    s_kb_eye_btn = NULL;
    s_kb_eye_lbl = NULL;
    log_i("[LAN_DEV] Screen closed, back to menu");
}