#include "secrets_manager.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_mac.h>
#include <mbedtls/sha256.h>
#include "user_config.h"

static char s_device_hash[20] = {0};
static char s_telemetry_table[32] = {0};
static char s_ai_url[192] = {0};
static char s_telemetry_endpoint[192] = {0};
static char s_weather_key[64] = {0};
static char s_master_pin[32] = {0};
static uint32_t s_telemetry_interval = 300; // default 5 minutes (300s)
static bool s_has_custom_ini = false;
static bool s_cloud_consent = false;

static Preferences s_sec_prefs;

static void compute_device_hash(void) {
    uint8_t mac[6] = {0};
    esp_efuse_mac_get_default(mac);

    // Read stored PIN or default to "1234"
    if (s_master_pin[0] == '\0') {
        if (s_sec_prefs.begin("sys_sec", true)) {
            String pin = s_sec_prefs.getString("pin", "1234");
            strncpy(s_master_pin, pin.c_str(), sizeof(s_master_pin) - 1);
            s_sec_prefs.end();
        } else {
            strncpy(s_master_pin, "1234", sizeof(s_master_pin) - 1);
        }
    }

    // Hash MAC + PIN with SHA-256
    uint8_t input[38];
    memcpy(input, mac, 6);
    size_t pin_len = strlen(s_master_pin);
    if (pin_len > 31) pin_len = 31;
    memcpy(input + 6, s_master_pin, pin_len);

    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 for SHA-256
    mbedtls_sha256_update(&ctx, input, 6 + pin_len);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    // Take first 16 hex characters in uppercase
    for (int i = 0; i < 8; i++) {
        snprintf(s_device_hash + (i * 2), 3, "%02X", hash[i]);
    }
    s_device_hash[16] = '\0';

    // Default telemetry table is TB_<hash>
    snprintf(s_telemetry_table, sizeof(s_telemetry_table), "TB_%s", s_device_hash);
}

static void trim_str(char *str) {
    if (!str) return;
    char *p = str;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (p != str) memmove(str, p, strlen(p) + 1);
    size_t len = strlen(str);
    while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' || str[len - 1] == '\r' || str[len - 1] == '\n')) {
        str[len - 1] = '\0';
        len--;
    }
}

void secrets_manager_reload(void) {
    // 1. Reset to baked defaults
    strncpy(s_ai_url, AI_ASSISTANT_URL, sizeof(s_ai_url) - 1);
    strncpy(s_telemetry_endpoint, TELEMETRY_ENDPOINT, sizeof(s_telemetry_endpoint) - 1);
    strncpy(s_weather_key, WEATHER_API_KEY, sizeof(s_weather_key) - 1);
    s_telemetry_interval = 300;
    s_has_custom_ini = false;

    // Load cloud consent from NVS
    if (s_sec_prefs.begin("sys_sec", true)) {
        s_cloud_consent = s_sec_prefs.getBool("consent", false);
        s_sec_prefs.end();
    }

    compute_device_hash();

    // 2. Check if /secrets.ini exists in LittleFS
    if (!LittleFS.exists("/secrets.ini")) {
        log_i("[SECRETS] No /secrets.ini found, using baked community defaults. Table: %s", s_telemetry_table);
        return;
    }

    File f = LittleFS.open("/secrets.ini", "r");
    if (!f) {
        log_w("[SECRETS] Failed to open /secrets.ini");
        return;
    }

    log_i("[SECRETS] Parsing /secrets.ini overrides...");
    s_has_custom_ini = true;

    char current_section[32] = {0};
    char line_buf[256];

    while (f.available()) {
        int len = f.readBytesUntil('\n', line_buf, sizeof(line_buf) - 1);
        line_buf[len] = '\0';
        trim_str(line_buf);

        if (line_buf[0] == '\0' || line_buf[0] == '#' || line_buf[0] == ';') {
            continue; // Skip comments and empty lines
        }

        if (line_buf[0] == '[' && line_buf[strlen(line_buf) - 1] == ']') {
            // Section header
            strncpy(current_section, line_buf + 1, sizeof(current_section) - 1);
            current_section[strlen(current_section) - 1] = '\0';
            trim_str(current_section);
            continue;
        }

        char *eq = strchr(line_buf, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line_buf;
        char *val = eq + 1;
        trim_str(key);
        trim_str(val);

        if (strcasecmp(current_section, "cloud") == 0) {
            if (strcasecmp(key, "consent") == 0) {
                if (atoi(val) == 1 || strcasecmp(val, "true") == 0 || strcasecmp(val, "yes") == 0) {
                    secrets_set_cloud_consent(true);
                }
            }
        } else if (strcasecmp(current_section, "ai") == 0) {
            if (strcasecmp(key, "endpoint") == 0 || strcasecmp(key, "url") == 0) {
                if (strlen(val) > 4) {
                    strncpy(s_ai_url, val, sizeof(s_ai_url) - 1);
                    log_i("[SECRETS] AI URL override: %s", s_ai_url);
                }
            }
        } else if (strcasecmp(current_section, "telemetry") == 0) {
            if (strcasecmp(key, "endpoint") == 0 || strcasecmp(key, "url") == 0) {
                if (strlen(val) > 4) {
                    strncpy(s_telemetry_endpoint, val, sizeof(s_telemetry_endpoint) - 1);
                    log_i("[SECRETS] Telemetry endpoint override: %s", s_telemetry_endpoint);
                }
            } else if (strcasecmp(key, "table") == 0) {
                if (strlen(val) > 2) {
                    strncpy(s_telemetry_table, val, sizeof(s_telemetry_table) - 1);
                    log_i("[SECRETS] Telemetry table override: %s", s_telemetry_table);
                }
            } else if (strcasecmp(key, "interval_sec") == 0 || strcasecmp(key, "interval") == 0) {
                int iv = atoi(val);
                if (iv >= 10 && iv <= 86400) {
                    s_telemetry_interval = (uint32_t)iv;
                    log_i("[SECRETS] Telemetry interval override: %u sec", (unsigned)s_telemetry_interval);
                }
            }
        } else if (strcasecmp(current_section, "weather") == 0) {
            if (strcasecmp(key, "api_key") == 0 || strcasecmp(key, "key") == 0) {
                if (strlen(val) > 4) {
                    strncpy(s_weather_key, val, sizeof(s_weather_key) - 1);
                    log_i("[SECRETS] Weather API key override applied");
                }
            }
        } else if (strcasecmp(current_section, "security") == 0) {
            if (strcasecmp(key, "master_pin") == 0 || strcasecmp(key, "pin") == 0) {
                if (strlen(val) >= 4) {
                    strncpy(s_master_pin, val, sizeof(s_master_pin) - 1);
                    compute_device_hash();
                    log_i("[SECRETS] Master PIN override applied. Recalculated table: %s", s_telemetry_table);
                }
            }
        }
    }
    f.close();
}

void secrets_manager_init(void) {
    secrets_manager_reload();
}

const char* secrets_get_ai_url(void) {
    return s_ai_url;
}

const char* secrets_get_telemetry_endpoint(void) {
    return s_telemetry_endpoint;
}

const char* secrets_get_telemetry_table(void) {
    return s_telemetry_table;
}

uint32_t secrets_get_telemetry_interval(void) {
    return s_telemetry_interval;
}

const char* secrets_get_weather_key(void) {
    return s_weather_key;
}

const char* secrets_get_device_hash(void) {
    return s_device_hash;
}

const char* secrets_get_master_pin(void) {
    return s_master_pin;
}

bool secrets_has_cloud_consent(void) {
    return s_cloud_consent;
}

void secrets_set_cloud_consent(bool consent) {
    s_cloud_consent = consent;
    if (s_sec_prefs.begin("sys_sec", false)) {
        s_sec_prefs.putBool("consent", consent);
        s_sec_prefs.end();
        log_i("[SECRETS] Cloud consent set to: %s", consent ? "ACCEPTED" : "DECLINED");
    }
}

bool secrets_has_custom_ini(void) {
    return s_has_custom_ini;
}
