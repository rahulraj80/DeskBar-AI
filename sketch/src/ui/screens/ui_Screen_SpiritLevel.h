#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Initialize Spirit Level screen and register timer
void ui_spirit_level_init(lv_obj_t *parent_screen);

// Open Spirit Level UI
void ui_spirit_level_open(void);

// Close Spirit Level UI
void ui_spirit_level_close(void);

#ifdef __cplusplus
}
#endif
