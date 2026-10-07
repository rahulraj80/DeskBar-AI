#include "tca9554.h"

// --- TCA9554 IO Expander DEFINITIONS ---


// Pin mapping: EXIO7 corresponds to the Most Significant Bit (MSB) P7
const uint8_t EXIO1_BIT = 0b00000010; // Bit 1: SYS_EN on V2 / BL_EN on V1.1
const uint8_t EXIO2_BIT = 0b00000100; // Bit 2: NS_MODE (NS4150B amp enable on V2)
const uint8_t EXIO5_BIT = 0b00100000; // Bit 5: LCD Reset (LCD_RST)
const uint8_t EXIO6_BIT = 0b01000000; // Bit 6: SYS_EN on V1
const uint8_t EXIO7_BIT = 0b10000000; // Bit 7: NS_MODE on V1


TCA9554::TCA9554(i2c_master_dev_handle_t handle) {
    dev = handle;
    outputState = EXIO1_BIT | EXIO6_BIT; // Hold SYS_EN power latch HIGH by default across all board revisions
    configState = 0xFF;
}

bool TCA9554::begin() {
    // Configure power latch pins as output (0 = output)
    configState &= ~(EXIO1_BIT | EXIO6_BIT);
    bool ok = writeRegister(CONFIG_REGISTER, configState);
    ok &= writeRegister(OUTPUT_REGISTER, outputState);
    if (ok)
        log_i("TCA9554 OK (Power pins latched HIGH)");
    else
        log_e("TCA9554 init failed");
    return ok;
}

bool TCA9554::setPinMode(uint8_t pin_mask, uint8_t mode) {
    if (mode == 0)
        configState &= ~pin_mask;
    else
        configState |= pin_mask;

    return writeRegister(CONFIG_REGISTER, configState);
}

bool TCA9554::digitalWrite(uint8_t pin_mask, uint8_t state) {
    if (state)
        outputState |= pin_mask;
    else
        outputState &= ~pin_mask;

    return writeRegister(OUTPUT_REGISTER, outputState);
}

uint8_t TCA9554::digitalRead(uint8_t pin_mask) {
    uint8_t value = readRegister(INPUT_REGISTER);
    return (value & pin_mask) ? 1 : 0;
}

bool TCA9554::writeRegister(uint8_t reg, uint8_t data) {
    uint8_t buf[1] = { data };
    return i2c_write_buff(dev, reg, buf, 1) == ESP_OK;
}

uint8_t TCA9554::readRegister(uint8_t reg) {
    uint8_t buf = 0;
    i2c_read_buff(dev, reg, &buf, 1);
    return buf;
}

extern TCA9554 *io;

extern "C" void bsp_lcd_reset(void) {
    if (io) {
        io->setPinMode(EXIO5_BIT, 0); // 0 = OUTPUT
        io->digitalWrite(EXIO5_BIT, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
        io->digitalWrite(EXIO5_BIT, 0);
        vTaskDelay(pdMS_TO_TICKS(250));
        io->digitalWrite(EXIO5_BIT, 1);
        vTaskDelay(pdMS_TO_TICKS(30));
    }
}

extern "C" void bsp_set_backlight_power(bool enable) {
    if (io) {
        io->setPinMode(EXIO1_BIT, 0); // 0 = OUTPUT
        io->digitalWrite(EXIO1_BIT, enable ? 1 : 0);
    }
}

extern "C" void bsp_set_audio_amp_power(bool enable) {
    if (io) {
        // Assert NS_MODE on both Bit 2 (V2 hardware) and Bit 7 (V1 hardware)
        io->setPinMode(EXIO2_BIT | EXIO7_BIT, 0); // 0 = OUTPUT
        io->digitalWrite(EXIO2_BIT | EXIO7_BIT, enable ? 1 : 0);
    }
}
