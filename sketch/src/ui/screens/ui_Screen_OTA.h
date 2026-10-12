#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

void ui_ota_manager_init(lv_obj_t *parent_screen);
void ui_ota_open_manager(void);
void ui_ota_close_manager(void);

#ifdef __cplusplus
}
#endif
