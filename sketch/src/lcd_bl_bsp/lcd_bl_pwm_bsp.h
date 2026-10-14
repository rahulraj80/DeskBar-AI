#ifndef LCD_BL_PWM_BSP_H
#define LCD_BL_PWM_BSP_H
#include <stdint.h>



#define  LCD_PWM_MODE_0   (0xff-0)
#define  LCD_PWM_MODE_25  (0xff-25)
#define  LCD_PWM_MODE_50  (0xff-50)
#define  LCD_PWM_MODE_75  (0xff-75)
#define  LCD_PWM_MODE_100 (0xff-100)
#define  LCD_PWM_MODE_125 (0xff-125)
#define  LCD_PWM_MODE_150 (0xff-150)
#define  LCD_PWM_MODE_175 (0xff-175)
#define  LCD_PWM_MODE_200 (0xff-200)
#define  LCD_PWM_MODE_225 (0xff-225)
#define  LCD_PWM_MODE_255 (0xff-255)

#define  LCD_BL_MIN   (255 - 90)   // 90 = Minimum vacuum tube glow floor
#define  LCD_BL_LOW   (255 - 92)   // 92 = LOW
#define  LCD_BL_MED   (255 - 120)  // 120 = MEDIUM
#define  LCD_BL_HIGH  (255 - 250)  // 250 = HIGH



#ifdef __cplusplus
extern "C" {
#endif

extern volatile uint8_t BL_OFF;
extern volatile uint8_t clock_face_active;
extern volatile uint8_t backlight_state;
extern volatile uint32_t SCREEN_OFF_TIMER;
extern volatile uint32_t SCREEN_OFF_DELAY;


void lcd_bl_pwm_bsp_init(uint16_t duty);
void setUpduty(uint16_t duty);
void screenPowerOff(void);
void screenPowerOn(void);

#ifdef __cplusplus
}
#endif

#endif