#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

// Lifecycle & Navigation
void ui_ota_manager_init(lv_obj_t *parent_screen);
void ui_ota_open_manager(void);
void ui_ota_close_manager(void);

// Live GitHub Catalog Fetch & Trigger
void ui_ota_fetch_live_catalog(void);
void ui_ota_trigger_flash_url(const char *version_str, const char *bin_url);

#ifdef __cplusplus
}
#endif
