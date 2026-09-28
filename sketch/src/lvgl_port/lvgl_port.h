#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif


void lvgl_port_init(void);
void bsp_display_sleep(void);
void bsp_display_wake(void);

#ifdef __cplusplus
}
#endif



#endif