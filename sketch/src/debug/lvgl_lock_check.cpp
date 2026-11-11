// LVGL thread-safety checker (debug builds only: env esp32-s3-devkitc1-n16r8-lockcheck).
// LVGL is not thread-safe; every call from a task other than the LVGL task must hold
// lvgl_port_lock(). The linker wraps a few hot LVGL entry points and reports any call
// made by a task that does not hold the LVGL mutex. Resolve the printed caller address
// with xtensa-esp32s3-elf-addr2line against firmware.elf.
#ifdef LVGL_LOCK_CHECK
#include <Arduino.h>
#include <lvgl.h>

extern "C" void *lvgl_port_mux_handle(void);

static volatile uint32_t s_hits = 0;

static void lock_check(const char *fn, void *caller) {
    SemaphoreHandle_t m = (SemaphoreHandle_t)lvgl_port_mux_handle();
    if (!m) return;  // before lvgl_port_init(): single-threaded
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    if (xSemaphoreGetMutexHolder(m) == me) return;
    if (s_hits++ < 60)
        Serial.printf("[LOCKCHK] %s from task %s without LVGL lock, caller %p\n", fn, pcTaskGetName(me), caller);
}

extern "C" {
void *__real_lv_mem_alloc(size_t s);
void __real_lv_mem_free(void *p);
void *__real_lv_mem_realloc(void *p, size_t s);
lv_res_t __real_lv_event_send(lv_obj_t *o, lv_event_code_t c, void *p);
void __real_lv_obj_invalidate(const lv_obj_t *o);

void *__wrap_lv_mem_alloc(size_t s) { lock_check("lv_mem_alloc", __builtin_return_address(0)); return __real_lv_mem_alloc(s); }
void __wrap_lv_mem_free(void *p) { lock_check("lv_mem_free", __builtin_return_address(0)); __real_lv_mem_free(p); }
void *__wrap_lv_mem_realloc(void *p, size_t s) { lock_check("lv_mem_realloc", __builtin_return_address(0)); return __real_lv_mem_realloc(p, s); }
lv_res_t __wrap_lv_event_send(lv_obj_t *o, lv_event_code_t c, void *p) { lock_check("lv_event_send", __builtin_return_address(0)); return __real_lv_event_send(o, c, p); }
void __wrap_lv_obj_invalidate(const lv_obj_t *o) { lock_check("lv_obj_invalidate", __builtin_return_address(0)); __real_lv_obj_invalidate(o); }
}
#endif
