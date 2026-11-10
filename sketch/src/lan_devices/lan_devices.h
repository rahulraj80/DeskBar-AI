#ifndef LAN_DEVICES_H
#define LAN_DEVICES_H

/* ========================================================================
 *  LAN DEVICES — HTTP-only local device control (no SSH, no OOM risk)
 *
 *  Discovers & controls Home Assistant servers and custom HTTP agents
 *  (Pi, mobile Termux, desktop helpers) over plain HTTP.
 * ======================================================================== */

#include <stdint.h>
#include <stdbool.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------- Device types ---------- */
typedef enum {
    LAN_DEV_SWITCH,   // on/off toggle
    LAN_DEV_LIGHT,    // on/off + brightness
    LAN_DEV_COVER,    // open/closed (blinds, garage)
    LAN_DEV_FAN,      // on/off
    LAN_DEV_SENSOR,   // read-only (no toggle)
    LAN_DEV_OTHER     // generic
} lan_dev_type_t;

/* ---------- A discovered/controllable entity ---------- */
typedef struct {
    char  entity_id[64];   // e.g. "switch.kitchen_light"
    char  name[64];        // friendly name
    char  location[64];    // room/area for grouping (e.g. "Living Room")
    char  domain[16];      // switch, light, cover, fan, sensor
    char  state[32];       // on / off / open / closed / etc.
    lan_dev_type_t type;
    bool  controllable;    // true if we can send a toggle/service call
    uint8_t brightness;    // 0-255 for lights
    float  temperature;    // °C for climate sensors
    float  humidity;       // % for humidity sensors
} lan_device_t;

/* ---------- Home Assistant server config ---------- */
typedef struct {
    char  host[128];       // hostname or IP
    uint16_t port;         // 8123 typically
    char  token[384];      // access token (long-lived, or short-lived from login)
    char  refresh[160];    // refresh token from username/password login (empty = manual token)
    bool  ssl;             // https vs http
    bool  discovered;      // true if found via mDNS
} lan_ha_server_t;

/* ---------- Generic HTTP agent (Pi, mobile, desktop) ---------- */
typedef struct {
    char  name[64];        // "Office Pi", "Bedroom Phone"
    char  host[128];
    uint16_t port;
    char  base_path[128];  // e.g. "/api"
} lan_agent_t;

/* ---------- Public API ---------- */

/* Initialise: load saved HA servers / agents from NVS, start mDNS browse */
void lan_devices_init(void);

/* Call from the UI screen when it opens — refreshes device list from HA */
void lan_devices_refresh(void);

/* Number of devices currently known */
int lan_devices_count(void);

/* Pointer to device at index (0 .. count-1) */
const lan_device_t *lan_devices_get(int idx);

/* Toggle a device (switch/light/fan/cover). Returns true if command sent. */
bool lan_devices_toggle(const char *entity_id);

/* Set light brightness (0-255) */
bool lan_devices_set_brightness(const char *entity_id, uint8_t brightness);

/* Add / remove a manual HA server */
void lan_devices_add_ha_server(const char *host, uint16_t port,
                               const char *token, bool ssl);
void lan_devices_remove_ha_server(int idx);

/* Sign in to a Home Assistant server with username + password (HA login flow).
 * Stores only the refresh token, never the password. On failure returns false
 * and writes a short human-readable reason into err. */
bool lan_devices_login(const char *host, uint16_t port, const char *user,
                       const char *pass, bool ssl, char *err, size_t errsz);

/* Add / remove a generic HTTP agent */
void lan_devices_add_agent(const char *name, const char *host,
                           uint16_t port, const char *base_path);
void lan_devices_remove_agent(int idx);

/* Number of HA servers / agents */
int lan_devices_ha_server_count(void);
const lan_ha_server_t *lan_devices_ha_server_get(int idx);
int lan_devices_agent_count(void);
const lan_agent_t *lan_devices_agent_get(int idx);

/* Save current config to NVS (call after any add/remove) */
void lan_devices_save_config(void);

/* Open the LAN Devices UI screen */
void lan_devices_open(void);

/* Close the LAN Devices UI screen (return to menu) */
void lan_devices_close(void);

/* Diagnostic: run an mDNS query and print results to Serial */
void lan_devices_mdns_debug(void);

#ifdef __cplusplus
}
#endif

#endif // LAN_DEVICES_H
