#ifndef UI_SCREEN_CONSENT_H
#define UI_SCREEN_CONSENT_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void ui_consent_modal_init(lv_obj_t *parent);
void ui_consent_modal_show_if_needed(void);

#ifdef __cplusplus
}
#endif

#endif // UI_SCREEN_CONSENT_H
