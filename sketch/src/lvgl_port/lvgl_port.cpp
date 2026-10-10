#include "lvgl_port.h"
#include "axs15231b/esp_lcd_axs15231b.h"
#include "driver/spi_master.h"
#include "esp32-hal.h"
#include "esp_err.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bsp/i2c_bsp.h"
#include "lvgl.h"
#include "user_config.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"

#include "esp_task_wdt.h"
#include "task_msg/task_msg.h"
#include "ui/ui.h"
#include "ui/screens/ui_Screen_Boot.h"
#include "ui/screens/ui_Screen_Info.h"
#include "ui/screens/ui_Screen_MainMenu.h"
#include "ui/screens/ui_Screen_Player.h"
#include "ui/screens/ui_Screen_Utility.h"

extern uint8_t mediaType;
extern "C" void exit_clock_breathing(void);
extern void screenPowerOn(void);

#define LCD_BIT_PER_PIXEL (16)

static const char *TAG = "lvgl_port";
static SemaphoreHandle_t lvgl_mux = NULL;
static esp_lcd_panel_handle_t s_panel_handle = NULL;
extern "C" void bsp_lcd_reset(void);

static uint16_t *lvgl_dma_buf = NULL;
static SemaphoreHandle_t lvgl_flush_semap;
static const uint16_t *s_last_frame_buffer = NULL;

#if (Rotated == USER_DISP_ROT_90)
uint16_t *rotat_ptr = NULL;
#endif

static const axs15231b_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 100},
};

static bool WAVESHARE_349_notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx) {
  BaseType_t TaskWoken;
  xSemaphoreGiveFromISR(lvgl_flush_semap, &TaskWoken);
  return false;
}

static void WAVESHARE_349_increase_lvgl_tick(void *arg) {
  lv_tick_inc(WAVESHARE_349_LVGL_TICK_PERIOD_MS);
}

static void WAVESHARE_349_lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map) {
  if (BL_OFF) {
    lv_disp_flush_ready(drv);
    return;
  }
#if (Rotated == USER_DISP_ROT_90)
  uint32_t index = 0;
  uint16_t *data_ptr = (uint16_t *)color_map;
  for (uint16_t j = 0; j < WAVESHARE_349_LCD_H_RES; j++) {
    for (uint16_t i = 0; i < WAVESHARE_349_LCD_V_RES; i++) {
      rotat_ptr[index++] = data_ptr[WAVESHARE_349_LCD_H_RES * (WAVESHARE_349_LCD_V_RES - i - 1) + j];
    }
  }
#endif
  esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
  const int flush_coun = (LVGL_SPIRAM_BUFF_LEN / LVGL_DMA_BUFF_LEN);
  const int offgap = (LCD_NOROT_VRES / flush_coun);
  const int dmalen = (LVGL_DMA_BUFF_LEN / 2);
  int offsetx1 = 0;
  int offsety1 = 0;
  int offsetx2 = LCD_NOROT_HRES;
  int offsety2 = offgap;

#if (Rotated == USER_DISP_ROT_90)
  uint16_t *map = (uint16_t *)rotat_ptr;
#else
  uint16_t *map = (uint16_t *)color_map;
#endif

  xSemaphoreGive(lvgl_flush_semap);
  for (int i = 0; i < flush_coun; i++) {
    xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
    memcpy(lvgl_dma_buf, map, LVGL_DMA_BUFF_LEN);
    esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2, offsety2, lvgl_dma_buf);
    offsety1 += offgap;
    offsety2 += offgap;
    map += dmalen;
  }
  xSemaphoreTake(lvgl_flush_semap, portMAX_DELAY);
  s_last_frame_buffer = (const uint16_t *)color_map;
  lv_disp_flush_ready(drv);
}

const uint16_t* lvgl_port_get_framebuffer(void) {
  return s_last_frame_buffer;
}

bool lvgl_port_take_screenshot(void) {
  exit_clock_breathing();
  screenPowerOn();
  if (lvgl_port_lock(500)) {
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lvgl_port_unlock();
    return (s_last_frame_buffer != NULL);
  }
  return false;
}

const char* lvgl_port_get_active_screen_name(void) {
  lv_obj_t *act = lv_scr_act();
  if (!act) return "UNKNOWN";
  if (act == ui_Screen_MainMenu) {
    if (ui_MainMenu_Tabview_ConfigPanel && !lv_obj_has_flag(ui_MainMenu_Tabview_ConfigPanel, LV_OBJ_FLAG_HIDDEN)) {
      uint16_t tab = lv_tabview_get_tab_act(ui_MainMenu_Tabview_ConfigPanel);
      switch(tab) {
        case 0: return "MAIN_MENU (Config: Network)";
        case 1: return "MAIN_MENU (Config: Screen)";
        case 2: return "MAIN_MENU (Config: Station)";
        case 3: return "MAIN_MENU (Config: Music)";
        case 4: return "MAIN_MENU (Config: Region)";
        case 5: return "MAIN_MENU (Config: Guide)";
        default: return "MAIN_MENU (Config Panel)";
      }
    }
    return "MAIN_MENU";
  }
  if (act == ui_Screen_Player) {
    if (mediaType == 0) return "PLAYER (Web Radio)";
    if (mediaType == 1) return "PLAYER (Music Player)";
    if (mediaType == 2) return "PLAYER (AI Voice Assistant)";
    return "PLAYER";
  }
  if (act == ui_Screen_Info) {
    extern uint8_t infoPageIndex;
    switch(infoPageIndex) {
      case 0: return "INFO (Nixie Clock)";
      case 1: return "INFO (Weather / Climate)";
      case 2: return "INFO (Alarm)";
      default: return "INFO";
    }
  }
  if (act == ui_Screen_Utility) return "UTILITY";
  if (act == ui_Screen_Boot) return "BOOT";
  return "CUSTOM_SCREEN";
}

static bool s_sim_touch_active = false;
static lv_point_t s_sim_touch_point = {0, 0};
static uint32_t s_sim_touch_release_ms = 0;

static bool s_sim_swipe_active = false;
static lv_point_t s_sim_swipe_start = {0, 0};
static lv_point_t s_sim_swipe_end = {0, 0};
static uint32_t s_sim_swipe_start_ms = 0;
static uint32_t s_sim_swipe_duration_ms = 0;

void lvgl_port_inject_touch(int16_t x, int16_t y, uint32_t duration_ms) {
  exit_clock_breathing();
  screenPowerOn();
  SCREEN_OFF_TIMER = millis();
  s_sim_swipe_active = false;
  s_sim_touch_point.x = x;
  s_sim_touch_point.y = y;
  s_sim_touch_active = true;
  s_sim_touch_release_ms = millis() + duration_ms;
}

void lvgl_port_inject_swipe(int16_t x1, int16_t y1, int16_t x2, int16_t y2, uint32_t duration_ms) {
  exit_clock_breathing();
  screenPowerOn();
  SCREEN_OFF_TIMER = millis();
  s_sim_touch_active = false;
  s_sim_swipe_start.x = x1;
  s_sim_swipe_start.y = y1;
  s_sim_swipe_end.x = x2;
  s_sim_swipe_end.y = y2;
  s_sim_swipe_start_ms = millis();
  s_sim_swipe_duration_ms = (duration_ms > 0) ? duration_ms : 300;
  s_sim_swipe_active = true;
}

static void WAVESHARE_349_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  if (s_sim_swipe_active) {
    uint32_t elapsed = millis() - s_sim_swipe_start_ms;
    if (elapsed < s_sim_swipe_duration_ms) {
      data->state = LV_INDEV_STATE_PR;
      float progress = (float)elapsed / (float)s_sim_swipe_duration_ms;
      data->point.x = s_sim_swipe_start.x + (int16_t)((s_sim_swipe_end.x - s_sim_swipe_start.x) * progress);
      data->point.y = s_sim_swipe_start.y + (int16_t)((s_sim_swipe_end.y - s_sim_swipe_start.y) * progress);
      return;
    } else {
      s_sim_swipe_active = false;
      data->state = LV_INDEV_STATE_REL;
      data->point = s_sim_swipe_end;
      return;
    }
  }

  if (s_sim_touch_active) {
    if (millis() < s_sim_touch_release_ms) {
      data->state = LV_INDEV_STATE_PR;
      data->point = s_sim_touch_point;
      return;
    } else {
      s_sim_touch_active = false;
      data->state = LV_INDEV_STATE_REL;
      data->point = s_sim_touch_point;
      return;
    }
  }

  if (BL_OFF) {
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  static uint8_t read_touchpad_cmd[8] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x8};
  // uint8_t read_touchpad_cmd[11] = {0xb5, 0xab, 0xa5, 0x5a, 0x0, 0x0, 0x0, 0x0e,0x0, 0x0, 0x0};
  uint8_t buff[32] = {0};
  memset(buff, 0, 32);
  ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_write_read_dev(disp_touch_dev_handle, read_touchpad_cmd, 11, buff, 32));
  uint16_t pointX;
  uint16_t pointY;
  pointX = (((uint16_t)buff[2] & 0x0f) << 8) | (uint16_t)buff[3];
  pointY = (((uint16_t)buff[4] & 0x0f) << 8) | (uint16_t)buff[5];
  // ESP_LOGI("Touch","%d,%d",buff[0],buff[1]);
  // ESP_LOGD(TAG, "Raw Touch: %d - %d", pointX, pointY);

  if (buff[1] > 0 && buff[1] < 5) {
    exit_clock_breathing();
    screenPowerOn();
    SCREEN_OFF_TIMER = millis();
    data->state = LV_INDEV_STATE_PR;
#if (Rotated != USER_DISP_ROT_NONO)
    if (pointX > WAVESHARE_349_LCD_V_RES) pointX = WAVESHARE_349_LCD_V_RES;
    if (pointY > WAVESHARE_349_LCD_H_RES) pointY = WAVESHARE_349_LCD_H_RES;
    data->point.x = pointY;
    data->point.y = (WAVESHARE_349_LCD_V_RES - pointX);
#else
    if (pointX > WAVESHARE_349_LCD_H_RES) pointX = WAVESHARE_349_LCD_H_RES;
    if (pointY > WAVESHARE_349_LCD_V_RES) pointY = WAVESHARE_349_LCD_V_RES;
    data->point.x = (WAVESHARE_349_LCD_H_RES - pointX);
    data->point.y = (WAVESHARE_349_LCD_V_RES - pointY);
#endif

  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

bool lvgl_port_lock(int timeout_ms) {
  if (!lvgl_mux) return false;
  const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

void lvgl_port_unlock(void) {
  if (lvgl_mux) xSemaphoreGive(lvgl_mux);
}

static bool WAVESHARE_349_lvgl_lock(int timeout_ms) {
  return lvgl_port_lock(timeout_ms);
}

static void WAVESHARE_349_lvgl_unlock(void) {
  lvgl_port_unlock();
}

// ##########################################################


void WAVESHARE_349_lvgl_port_task(void *arg)
{
   uint32_t task_delay_ms = WAVESHARE_349_LVGL_TASK_MAX_DELAY_MS;

  for(;;) {
    if (BL_OFF) {
      if (WAVESHARE_349_lvgl_lock(50)) {
        process_ui_status_queue();
        WAVESHARE_349_lvgl_unlock();
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    if (WAVESHARE_349_lvgl_lock(-1)) {
      process_ui_status_queue();
      task_delay_ms = lv_timer_handler();
      WAVESHARE_349_lvgl_unlock();
    }
    if (task_delay_ms > WAVESHARE_349_LVGL_TASK_MAX_DELAY_MS) {
      task_delay_ms = WAVESHARE_349_LVGL_TASK_MAX_DELAY_MS;
    } else if (task_delay_ms < 10) {
      task_delay_ms = 10;
    }
    uint32_t delay_ticks = pdMS_TO_TICKS(task_delay_ms);
    if (delay_ticks == 0) delay_ticks = 1;
    vTaskDelay(delay_ticks);
  }
}


void lvgl_port_init(void) {
#if (Rotated == USER_DISP_ROT_90)
  rotat_ptr = (uint16_t *)heap_caps_malloc(WAVESHARE_349_LCD_H_RES * WAVESHARE_349_LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
  assert(rotat_ptr);
#endif
  lvgl_flush_semap = xSemaphoreCreateBinary();

  static lv_disp_draw_buf_t disp_buf; // contains internal graphic buffer(s) called draw buffer(s)
  static lv_disp_drv_t disp_drv; // contains callback functions
#if (WAVESHARE_349_PIN_NUM_LCD_RST >= 0)
  ESP_LOGI(TAG, "Initialize LCD RESET GPIO");
  gpio_config_t gpio_conf = {};
  gpio_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_conf.mode = GPIO_MODE_OUTPUT;
  gpio_conf.pin_bit_mask = ((uint64_t)0X01 << WAVESHARE_349_PIN_NUM_LCD_RST);
  gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
#endif

  ESP_LOGI(TAG, "Initialize QSPI bus");
  spi_bus_config_t buscfg = {};
  buscfg.data0_io_num = WAVESHARE_349_PIN_NUM_LCD_DATA0;
  buscfg.data1_io_num = WAVESHARE_349_PIN_NUM_LCD_DATA1;
  buscfg.sclk_io_num = WAVESHARE_349_PIN_NUM_LCD_PCLK;
  buscfg.data2_io_num = WAVESHARE_349_PIN_NUM_LCD_DATA2;
  buscfg.data3_io_num = WAVESHARE_349_PIN_NUM_LCD_DATA3;
  buscfg.max_transfer_sz = LVGL_DMA_BUFF_LEN;
  ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

  ESP_LOGI(TAG, "Install panel IO");
  esp_lcd_panel_io_handle_t panel_io = NULL;
  esp_lcd_panel_handle_t panel = NULL;

  esp_lcd_panel_io_spi_config_t io_config = {};
  io_config.cs_gpio_num = WAVESHARE_349_PIN_NUM_LCD_CS;
  io_config.dc_gpio_num = -1;
  io_config.spi_mode = 3;
  io_config.pclk_hz = 40 * 1000 * 1000;
  io_config.trans_queue_depth = 10;
  io_config.on_color_trans_done = WAVESHARE_349_notify_lvgl_flush_ready;
  // io_config.user_ctx = &disp_drv,
  io_config.lcd_cmd_bits = 32;
  io_config.lcd_param_bits = 8;
  io_config.flags.quad_mode = true;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &panel_io));

  axs15231b_vendor_config_t vendor_config = {};
  vendor_config.flags.use_qspi_interface = 1;
  vendor_config.init_cmds = lcd_init_cmds;
  vendor_config.init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]);

  esp_lcd_panel_dev_config_t panel_config = {};
  panel_config.reset_gpio_num = -1;
  panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  panel_config.bits_per_pixel = LCD_BIT_PER_PIXEL;
  panel_config.vendor_config = &vendor_config;

  ESP_LOGI(TAG, "Install panel driver");
  ESP_ERROR_CHECK(esp_lcd_new_panel_axs15231b(panel_io, &panel_config, &panel));

#if (WAVESHARE_349_PIN_NUM_LCD_RST >= 0)
  ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)WAVESHARE_349_PIN_NUM_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
  ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)WAVESHARE_349_PIN_NUM_LCD_RST, 0));
  vTaskDelay(pdMS_TO_TICKS(250));
  ESP_ERROR_CHECK(gpio_set_level((gpio_num_t)WAVESHARE_349_PIN_NUM_LCD_RST, 1));
  vTaskDelay(pdMS_TO_TICKS(30));
#else
  bsp_lcd_reset();
#endif
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  s_panel_handle = panel;

  lv_init();

  lvgl_dma_buf = (uint16_t *)heap_caps_malloc(LVGL_DMA_BUFF_LEN, MALLOC_CAP_DMA);
  assert(lvgl_dma_buf);
  lv_color_t *buffer_1 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
  lv_color_t *buffer_2 = (lv_color_t *)heap_caps_malloc(LVGL_SPIRAM_BUFF_LEN, MALLOC_CAP_SPIRAM);
  assert(buffer_1);
  assert(buffer_2);
  lv_disp_draw_buf_init(&disp_buf, buffer_1, buffer_2, WAVESHARE_349_LCD_H_RES * WAVESHARE_349_LCD_V_RES);

  // REGISTER LVGL DISPLAY DRIVER
  ESP_LOGI(TAG, "Register display driver to LVGL");
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = WAVESHARE_349_LCD_H_RES;
  disp_drv.ver_res = WAVESHARE_349_LCD_V_RES;
  disp_drv.flush_cb = WAVESHARE_349_lvgl_flush_cb;
  disp_drv.draw_buf = &disp_buf;
  disp_drv.full_refresh = 1; // full_refresh must be 1
  disp_drv.user_data = panel;
  lv_disp_drv_register(&disp_drv);

  ESP_LOGI(TAG, "Install LVGL tick timer");
  esp_timer_create_args_t lvgl_tick_timer_args = {};
  lvgl_tick_timer_args.callback = &WAVESHARE_349_increase_lvgl_tick;
  lvgl_tick_timer_args.name = "lvgl_tick";
  esp_timer_handle_t lvgl_tick_timer = NULL;
  ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, WAVESHARE_349_LVGL_TICK_PERIOD_MS * 1000));

  // Register LVGL TOUCH
  static lv_indev_drv_t indev_drv; // Input device driver (Touch)
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = WAVESHARE_349_lvgl_touch_cb;
  lv_indev_drv_register(&indev_drv);

  lvgl_mux = xSemaphoreCreateMutex();
  assert(lvgl_mux);
  xTaskCreatePinnedToCore(WAVESHARE_349_lvgl_port_task, "LVGL", 6 * 1024, NULL, 5, NULL, 0); // Run Core 0
  if (WAVESHARE_349_lvgl_lock(-1)) {

    ui_init();
    WAVESHARE_349_lvgl_unlock();
  }
}
extern "C" void bsp_display_sleep(void) {
  // Screen blanking is fully handled by cutting the backlight rail (EXIO1)
  // and setting PWM duty to 0, plus short-circuiting LVGL flush.
  // We avoid sending QSPI sleep commands across cores to prevent bus collision/deadlocks.
}

extern "C" void bsp_display_wake(void) {
  // Screen wake is handled by re-enabling EXIO1 boost rail and restoring PWM duty.
}
