#include "es7210.h"
#include "i2c_bsp/i2c_bsp.h"
#include "es7210_reg.h"


ES7210::ES7210() {}
ES7210::~ES7210() {}




esp_err_t ES7210::writeReg(uint8_t reg, uint8_t val) {
    if (!es7210_dev_handle) return ESP_FAIL;
    return i2c_write_buff(es7210_dev_handle, reg, &val, 1) == ESP_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t ES7210::readReg(uint8_t reg, uint8_t *val) {
    if (!es7210_dev_handle || val == nullptr) return ESP_FAIL;
    return i2c_read_buff(es7210_dev_handle, reg, val, 1) == ESP_OK ? ESP_OK : ESP_FAIL;
}


bool ES7210::updateReg(uint8_t reg, uint8_t mask, uint8_t value) {
    uint8_t old_val;
    if (readReg(reg, &old_val) != ESP_OK) return false;
    uint8_t new_val = (old_val & ~mask) | (value & mask);
    return writeReg(reg, new_val) == ESP_OK;
}

bool ES7210::reset() {
    if (writeReg(ES7210_RESET_REG00, 0xFF) != ESP_OK) return false; 
    vTaskDelay(pdMS_TO_TICKS(50));
    if (writeReg(ES7210_RESET_REG00, 0x41) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(10));
    return true;
}

bool ES7210::init() {
    uint8_t id = 0;
    if (readReg(ES7210_RESET_REG00, &id) != ESP_OK) {
        log_e("ES7210 not responding on I2C");
        return false;
    }
    log_i("ES7210 detected on I2C (Reg00 = 0x%02X)", id);
    return true;
}

bool ES7210::start() {
    uint8_t check;
    if (readReg(0x00, &check) != ESP_OK) {
        log_e("ES7210 I2C Error");
        return false;
    }

    // 1. Software Reset
    writeReg(0x00, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(10));
    writeReg(0x00, 0x41);
    writeReg(0x01, 0x3F); // Turn off ADC clock during setup

    // 2. Timing Control
    writeReg(0x09, 0x30);
    writeReg(0x0A, 0x30);

    // 3. High Pass Filters
    writeReg(0x20, 0x0A);
    writeReg(0x21, 0x2A);
    writeReg(0x22, 0x0A);
    writeReg(0x23, 0x2A);

    // 4. Mode Config: Slave Mode (preserve bits 7:1, set bit 0 to 0)
    updateReg(0x08, 0x01, 0x00);

    // 5. Analog power & Mic bias (0x43 = VMID 5K start, vdda=3.3V, analog power ON)
    writeReg(0x40, 0x43);
    writeReg(0x41, 0x70); // Mic 1/2 bias 2.87V
    writeReg(0x42, 0x70); // Mic 3/4 bias 2.87V

    // 6. Digital Audio Interface: 16-bit Standard I2S, non-TDM stereo
    writeReg(0x11, 0x60); // 0x60 = 16-bit I2S standard
    writeReg(0x12, 0x00); // ADC12 to SDOUT1 (Mic 1 on Left channel)

    // 7. Clock dividers & OSR for 16 kHz capture (MCLK = 4.096 MHz, divider = 256)
    writeReg(0x03, 0x00); // MCLK source from pad
    writeReg(0x04, 0x01); // lrck_divh = 0x01 (divider 256 high byte)
    writeReg(0x05, 0x00); // lrck_divl = 0x00 (divider 256 low byte)
    writeReg(0x02, 0xC1); // dll=1, doubler=1, adc_div=1
    writeReg(0x07, 0x20); // osr = 0x20

    // 8. Gain (+34.5 dB high sensitivity for far-field room voice capture)
    writeReg(0x43, 0x1C); // Mic 1 PGA enable + 34.5 dB
    writeReg(0x44, 0x1C); // Mic 2
    writeReg(0x45, 0x1C); // Mic 3
    writeReg(0x46, 0x1C); // Mic 4

    // 9. Power on Microphones and ADCs (0x00 = fully powered on)
    writeReg(0x47, 0x08);
    writeReg(0x48, 0x08);
    writeReg(0x49, 0x08);
    writeReg(0x4A, 0x08);
    writeReg(0x06, 0x00); // Power on ADC/DLL
    writeReg(0x4B, 0x00); // Mic 1/2 bias & ADC power ON
    writeReg(0x4C, 0x00); // Mic 3/4 bias & ADC power ON

    // 10. Unmute
    writeReg(0x13, 0x00);
    writeReg(0x14, 0x00);

    // 11. Enable Device and Clocks
    writeReg(0x00, 0x71);
    vTaskDelay(pdMS_TO_TICKS(10));
    writeReg(0x00, 0x41);
    writeReg(0x01, 0x00); // All ADC clocks active

    log_i("[ES7210] Started: verified 16-bit I2S stereo mode (Reg 0x40=0x43, Reg 0x4B=0x00, Reg 0x01=0x00)");
    return true;
}

bool ES7210::stop() {
    writeReg(0x47, 0xFF);
    writeReg(0x48, 0xFF);
    writeReg(0x49, 0xFF);
    writeReg(0x4A, 0xFF);
    writeReg(0x4B, 0xFF);
    writeReg(0x4C, 0xFF);
    writeReg(0x40, 0xC0); // Power down analog
    writeReg(0x01, 0x7F); // Turn off ADC clock
    writeReg(0x06, 0x07); // Power down ADC
    log_i("[ES7210] Stopped (low-power standby)");
    return true;
}

bool ES7210::setMicGain(uint8_t ch, float db) {
    uint8_t reg = (ch == 0) ? ES7210_MIC1_GAIN_REG43 : ES7210_MIC2_GAIN_REG44;
    uint8_t step = (uint8_t)(db / 3.0f);
    if (step > 0x0F) step = 0x0F;
    return writeReg(reg, 0x10 | step) == ESP_OK;
}

//=================================================

bool is_mic_mode = false;