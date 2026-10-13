#ifndef SECRETS_MANAGER_H
#define SECRETS_MANAGER_H

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize secrets management subsystem:
 * - Computes unique per-device SHA256 hardware hash from eFuse MAC + Master PIN.
 * - Parses /secrets.ini from LittleFS if present to override baked endpoints.
 * - Loads cloud consent status from NVS.
 */
void secrets_manager_init(void);

/**
 * Reloads configuration from /secrets.ini (e.g. after user uploads new file via Web Remote)
 */
void secrets_manager_reload(void);

// Get active service endpoints & keys (INI overrides if set, else baked defaults)
const char* secrets_get_ai_url(void);
const char* secrets_get_telemetry_endpoint(void);
const char* secrets_get_telemetry_table(void);
uint32_t    secrets_get_telemetry_interval(void);
const char* secrets_get_weather_key(void);
const char* secrets_get_device_hash(void);
const char* secrets_get_master_pin(void);

// Community cloud consent management
bool secrets_has_cloud_consent(void);
bool secrets_has_decided_consent(void);
bool secrets_has_existing_pin(void);
void secrets_set_cloud_consent(bool consent);
void secrets_reset_consent(void);
void secrets_set_master_pin(const char *pin);
void secrets_calculate_table_for_pin(const char *pin, char *out_table, size_t max_len);
void secrets_erase_all(void);
bool secrets_has_custom_ini(void);

// Web Remote INI editor helpers
bool secrets_write_ini_file(const char *ai_url, const char *telem_url, const char *table, uint32_t interval, const char *pin, bool consent);
bool secrets_delete_ini_file(void);

#ifdef __cplusplus
}
#endif

#endif // SECRETS_MANAGER_H
