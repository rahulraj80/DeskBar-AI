#pragma once

#include <Arduino.h>
#include "i2c_bsp/i2c_bsp.h"

#define INPUT_REGISTER   0x00
#define OUTPUT_REGISTER  0x01
#define CONFIG_REGISTER  0x03

extern const uint8_t EXIO1_BIT; // backlight (Bit 1 = BL_EN)
extern const uint8_t EXIO5_BIT; // LCD Reset (Bit 5 = LCD_RST)
extern const uint8_t EXIO6_BIT; // Power (Bit 6 = SYS_EN)
extern const uint8_t EXIO7_BIT; // audio amp (Bit 7 = NS_MODE)

class TCA9554 {
public:
    uint8_t outputState;
    uint8_t configState;

    i2c_master_dev_handle_t dev;

    TCA9554(i2c_master_dev_handle_t handle);

    bool begin();
    bool setPinMode(uint8_t pin_mask, uint8_t mode);
    bool digitalWrite(uint8_t pin_mask, uint8_t state);
    uint8_t digitalRead(uint8_t pin_mask);

private:
    bool writeRegister(uint8_t reg, uint8_t data);
    uint8_t readRegister(uint8_t reg);
};

#ifdef __cplusplus
extern "C" {
#endif
void bsp_lcd_reset(void);
void bsp_set_backlight_power(bool enable);
void bsp_set_audio_amp_power(bool enable);
#ifdef __cplusplus
}
#endif

