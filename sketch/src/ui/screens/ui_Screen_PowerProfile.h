#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"

void ui_power_profile_init(lv_obj_t *parent_screen);
void ui_power_profile_open(void);
void ui_power_profile_close(void);
bool ui_power_profile_is_open(void);

#ifdef __cplusplus
}
#endif
