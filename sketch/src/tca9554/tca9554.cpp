#include "tca9554.h"

// --- TCA9554 IO Expander DEFINITIONS ---


// Pin mapping: EXIO7 corresponds to the Most Significant Bit (MSB) P7
const uint8_t EXIO1_BIT = 0b00000001; // backlight

const uint8_t EXIO6_BIT = 0b01000000; // Power
const uint8_t EXIO7_BIT = 0b10000000; // audio amp


TCA9554::TCA9554(i2c_master_dev_handle_t handle) {
    dev = handle;
    outputState = 0x00;
    configState = 0xFF;
}

bool TCA9554::begin() {
    bool ok = writeRegister(CONFIG_REGISTER, configState);
    ok &= writeRegister(OUTPUT_REGISTER, outputState);
    if (ok)
        log_i("TCA9554 OK");
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

extern "C" void bsp_set_backlight_power(bool enable) {
    if (io) {
        io->setPinMode(EXIO1_BIT, 0); // 0 = OUTPUT
        io->digitalWrite(EXIO1_BIT, enable ? 1 : 0);
    }
}

extern "C" void bsp_set_audio_amp_power(bool enable) {
    if (io) {
        io->setPinMode(EXIO7_BIT, 0); // 0 = OUTPUT
        io->digitalWrite(EXIO7_BIT, enable ? 1 : 0);
    }
}
