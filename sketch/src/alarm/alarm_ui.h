#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern lv_obj_t *ui_Info_Panel_Alarm;

void alarm_ui_init(lv_obj_t *parent);
void alarm_ui_show(void);
void alarm_ui_refresh(void);

#ifdef __cplusplus
}
#endif
