# ⚡ Hardware Architecture, Silicon Quirks & DRAM Optimization

Comprehensive engineering reference for the [Waveshare ESP32-S3-Touch-LCD-3.49](https://docs.waveshare.com/ESP32-S3-Touch-LCD-3.49?variant=ESP32-S3-Touch-LCD-3.49) development board.

---

## 🛠️ Hardware Specifications & Pinout

```
[ MEMS Mic 1 (Left)  ] ----\
                            +---> [ ES7210 4-Ch ADC ] ===(I2S DIN: GPIO 6)====\
[ MEMS Mic 3 (Right) ] ----/      (I2C: 0x40/0x41)                            \
                                                                               +---> [ ESP32-S3 ]
[ 8Ω 3W Speaker ] <--- [ NS4150B Class-D Amp ] <--- [ ES8311 DAC ] <===(I2S DOUT: GPIO 45)==/
                               ^                         (I2C: 0x18)
                               | (SYS_EN, NS_MODE)       (MCLK: GPIO 7)
                        [ TCA9554 EXIO Expander ]
                               (I2C: 0x20)
```

| Signal / Function | ESP32-S3 Pin | Purpose |
| :--- | :---: | :--- |
| **I2S Master Clock (MCLK)** | GPIO 7 | Shared master reference clock (12.288 MHz) for ES8311 & ES7210 |
| **I2S Bit Clock (BCLK)** | GPIO 15 | Shared serial bit clock |
| **I2S Word Select (LRCK)** | GPIO 46 | Shared left/right frame sync (16 kHz / 44.1 kHz) |
| **I2S Serial Data Out (DOUT)** | GPIO 45 | Audio DAC stream to ES8311 |
| **I2S Serial Data In (DIN)** | GPIO 6 | Audio ADC microphone stream from ES7210 |
| **Shared I2C SDA** | GPIO 8 | Control bus for ES8311, ES7210, PCF85063, QMI8658, TCA9554, AXS15231B |
| **Shared I2C SCL** | GPIO 9 | Control bus clock (400 kHz) |
| **LCD QSPI (CS, CLK, D0-D3)** | GPIO 10..14 | High-speed display bus for AXS15231B controller (640×172) |
| **Backlight PWM** | GPIO 42 | LEDC 5kHz hardware brightness control |
| **MicroSD SPI (CS, MOSI, MISO, CLK)**| GPIO 39..42 | SD card high-speed interface |
| **Battery ADC** | GPIO 1 | Analog voltage divider (scaled 1:2) monitoring LiPo cell |

---

## 🔍 Silicon Quirks, Hardware Bugs & Verified Fixes

Rigorous empirical testing on physical silicon revealed several critical bugs in vendor reference firmware:

1. **The ES7210 8× Clock Divider Bug (Vendor Reference Flaw)**:
   - *Symptom*: Recorded microphone audio sounded 8× slowed down with repetitive rhythmic clicks.
   - *Root Cause*: ES7210 register `0x02` was left at default `0x03` ($F_s = \text{MCLK} / (256 \times 8) = 3000\text{ Hz}$).
   - *Fix*: Initialized register `0x02` to `0x00` ($F_s = \text{MCLK} / 256 = 24000\text{ Hz}$), aligning ADC sampling with standard I2S clocking.
2. **ESP32 DMA 32-bit vs. 16-bit Slot Width Mismatch**:
   - *Symptom*: Severe phase distortion and crackle on microphone capture.
   - *Fix*: Implemented calibrated stride extractor to unpack 16-bit mono audio from 32-bit DMA frames.
3. **Monolithic I2S 1000ms Driver Timeout**:
   - *Symptom*: Audio recordings longer than 1.0s cut off with `ESP_ERR_TIMEOUT`.
   - *Fix*: Re-architected all capture loops into **2048-byte chunks** with periodic watchdog yields.
4. **Uninitialized PSRAM Static Blast**:
   - *Symptom*: Pressing "Play" on fresh boot blasted maximum-volume harsh white noise.
   - *Fix*: Explicit buffer zeroing on boot (`memset(audio_ptr, 0, ...)`), with an `audio_recorded` state guard.
5. **Windows CDC DTR/RTS Hardware Reset Loop**:
   - *Symptom*: Opening the serial port with standard tools resets the ESP32-S3 via the auto-download circuit.
   - *Fix*: Explicitly holding `dtr = False; rts = False` before opening the COM port (`scripts/safe_serial_monitor.py`).
6. **Newlib VFS CRLF Binary Mangling**:
   - *Symptom*: Transferring raw binary audio over UART caused corruption due to `0x0A` $\to$ `0x0D 0x0A` substitution.
   - *Fix*: Audio transferred over HTTP (`/rec.wav`) or framed Base64 payloads.

---

## ⚡ DRAM Optimization Deep-Dive

On the ESP32-S3, internal **DRAM** (Data RAM) is limited to ~320 KB usable. Critical operations—such as **DMA transfers, Wi-Fi baseband descriptors, and FreeRTOS ISR stacks**—**must** reside in internal DRAM.

| Optimization Step | Technique / Implementation | DRAM Saved | Impact |
| :--- | :--- | :---: | :--- |
| **1. Audio Buffers to PSRAM** | Relocated circular ring buffers, MP3 decoders, and LittleFS scratch buffers from internal SRAM to external PSRAM. | **~120 KB** | Eliminated largest memory hogs from internal DRAM. |
| **2. LVGL Custom Allocator** | Enabled `LV_MEM_CUSTOM` in `lv_conf.h` and hooked LVGL dynamic object allocations to PSRAM. Kept only LCD draw buffers in internal DMA RAM. | **~64 KB** | Allowed complex multi-screen UI without consuming heap. |
| **3. lwIP & Wi-Fi Buffer Sizing** | Tuned `sdkconfig.defaults` to optimize socket buffers (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`, `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM`). | **~38 KB** | Prevented network stack from hoarding inactive DRAM. |
| **4. Task Stack Re-Sizing** | Audited High-Water Marks (HWM) across all tasks, trimming over-provisioned task stacks to exact operating bounds. | **~24 KB** | Reduced baseline static memory reservations. |
| **5. Asynchronous Telemetry Ring**| Replaced heavy synchronous JSON allocations with an efficient in-memory ring buffer of compact C structs. | **~16 KB** | Prevented heap fragmentation spikes. |

* **Usable Internal DRAM Free**: **~204 KB** (40.3% utilization, down from >92% prior to optimization).
* **External PSRAM Free**: **~6.0 MB** available for audio caches, fonts, and streaming buffers.

---

## 💡 Display, Brightness & Power Management

1. **Vintage Nixie Tube 10-Second Breathing Animation**:
   - **`FULL_BLACK` (5.0s)**: Backlight PWM duty is set to 0 for exactly 5.0 seconds of complete pitch black darkness. The AP3032 boost rail remains powered and `BL_OFF` stays `false`, ensuring FreeRTOS LVGL timers never freeze.
   - **`FADE_IN` (1.5s)**: Smooth ramp up from the **90/255** Nixie filament floor to the configured ceiling.
   - **`HOLD_HIGH` (2.0s)**: Steady peak illumination at the configured ceiling.
   - **`FADE_OUT` (1.5s)**: Smooth ramp down from ceiling back to the **90/255** floor, dropping instantly into `FULL_BLACK`.
2. **Audio Domain Power Gating**:
   - The onboard **NS4150B Class-D amplifier** is dynamically gated: whenever audio playback or recording finishes, the firmware asserts `NS_MODE = LOW` and `SYS_EN = LOW` via the TCA9554 expander. The amplifier completely powers down to **0 mA quiescent drain**, eliminating background hiss.
3. **Calibrated Battery Voltage Filtering**:
   - Implemented a 10-second Exponential Moving Average (EMA) low-pass filter to prevent Wi-Fi transmission bursts (350–400 mA) from causing erratic battery percentage jumps.
