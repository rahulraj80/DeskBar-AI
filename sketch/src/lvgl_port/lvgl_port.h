#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif


void lvgl_port_init(void);
void bsp_display_sleep(void);
void bsp_display_wake(void);
bool lvgl_port_lock(int timeout_ms);
void lvgl_port_unlock(void);
void lvgl_port_inject_touch(int16_t x, int16_t y, uint32_t duration_ms);
void lvgl_port_inject_swipe(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint32_t duration_ms);
const uint16_t* lvgl_port_get_framebuffer(void);
bool lvgl_port_take_screenshot(void);
const char* lvgl_port_get_active_screen_name(void);

#ifdef __cplusplus
}
#endif



#endif