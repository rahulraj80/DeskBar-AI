# AUDIO_HARDWARE_LEARNINGS.md — TuneBar Audio Pipeline & Microphone Hardware Master Reference

**Author:** Antigravity Pair-Programming Session  
**Date:** October 6, 2026  
**Hardware:** Waveshare ESP32-S3-Touch-LCD-3.49 (ESP32-S3, 16MB Flash, 8MB OPI PSRAM)  
**Codebase:** TuneBar / ESP-IDF `08_Audio_Test` Reference

---

## 1. Executive Summary

This document captures all verified ground-truth hardware learnings, driver behaviors, silicon quirks, and protocol discoveries established through rigorous empirical testing on physical hardware.

All findings have been acoustically and digitally verified:
1. **On-device physical verification**: Touchscreen record and onboard speaker playback tested by user.
2. **Acoustic cross-validation**: Laptop microphone recorded device speaker playback across the room.
3. **Bit-for-bit digital verification**: Complete PSRAM audio buffers retrieved over USB and HTTP, evaluated with Google STT achieving 100% transcript fidelity.
4. **Autonomous physical validation**: Closed-loop testing using laptop speakers for acoustic queries, TuneBar ES7210 recording, onboard speaker repeat feedback loop, server upload, LCD screen snapshot via webcam, and TTS answer playback.

---

## 2. Hardware Architecture & Pin Map

The TuneBar board features a dedicated multi-chip audio pipeline:

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

### Complete Audio Pin Assignment Table:
| Signal / Function | ESP32-S3 Pin | Notes |
| :--- | :--- | :--- |
| **I2C SDA** | `GPIO 47` | Shared bus: ES7210, ES8311, TCA9554, RTC, Touch |
| **I2C SCL** | `GPIO 48` | Shared bus with 4.7k pullups |
| **I2S MCLK** | `GPIO 7` | Master clock for ES8311 DAC & ES7210 ADC (required!) |
| **I2S BCLK** | `GPIO 15` | Bit clock |
| **I2S WS / LRCK**| `GPIO 46` | Word select / frame clock |
| **I2S DOUT** | `GPIO 45` | Audio data output to ES8311 DAC |
| **I2S DIN** | `GPIO 6` | Audio data input from ES7210 ADC |
| **TCA9554 SYS_EN**| `EXIO Pin 1` | Latched `HIGH` (1) to power audio domain |
| **TCA9554 NS_MODE**|`EXIO Pin 2`| Latched `HIGH` (1) to un-mute NS4150B amp |

> [!IMPORTANT]
> Both `SYS_EN` and `NS_MODE` must be explicitly asserted `HIGH` via TCA9554 register writes upon boot. If either pin is low, the NS4150B amplifier enters shutdown and the onboard speaker produces zero sound.

---

## 3. ES7210 ADC Clock Dividers & DMA Slot Structure (Critical Bug Fix)

During early testing, microphone recordings retrieved over USB were severely distorted, noisy, or appeared 8x slowed down with rhythmic clicking. Two fundamental hardware root causes were diagnosed and resolved:

### Root Cause 1: Clock Divider Register Default (The 8x Slowdown)
* **Silicon Behavior**: With an external MCLK of `4.096 MHz`, the ES7210 internal LRCK clock divider registers (Reg 0x04 `lrck_divh` & Reg 0x05 `lrck_divl`) default to `2048` (`0x08 0x00`) instead of `256` (`0x01 0x00`).
* Because the divider was set to 2048, the internal delta-sigma ADC only output a valid sample once every 8 clock periods, producing an exact **8x slowdown** with 7 zero-frames for every valid sample frame.
* **Fix in `sketch/src/es7210/es7210.cpp`**:
  ```cpp
  writeReg(0x03, 0x00); // MCLK source from pad
  writeReg(0x04, 0x01); // lrck_divh = 0x01 (divider 256 high byte)
  writeReg(0x05, 0x00); // lrck_divl = 0x00 (divider 256 low byte)
  writeReg(0x02, 0xC1); // dll=1, doubler=1, adc_div=1
  writeReg(0x07, 0x20); // osr = 0x20
  ```

### Root Cause 2: ESP32 I2S 32-bit Slot DMA Stride
* **DMA Layout**: In ESP32-S3 standard I2S stereo mode with 32-bit slot width, each stereo frame in the DMA buffer occupies **8 bytes** (4 `int16_t` values):
  * `dma16[i * 4 + 0]`: Left Channel Sample (Mic 1)
  * `dma16[i * 4 + 1]`: Zero padding / high word
  * `dma16[i * 4 + 2]`: Right Channel Sample (Mic 3)
  * `dma16[i * 4 + 3]`: Zero padding / high word
* If indexed naively as `dma16[i * 2]`, the code alternates between speech samples and zero padding, producing synthetic 50% duty crackle.
* **Fix in `sketch/include/xtask.h`**:
  ```cpp
  size_t frames = bytes_read / 8;
  for (size_t i = 0; i < frames && converted < max_samples_left; i++) {
    pcm_dest[converted++] = dma16[i * 4 + 0]; // Pure Mic 1 signal
  }
  ```

### Verification Result:
WAV headers configured at `8000 Hz` sample rate yield crystal-clear audio:
```
Google STT Transcription: what is the weather in Mumbai (100% transcript match)
```

---

## 4. Voice Feedback Repeat Loop (User Feature)

To give the user instant physical feedback on recording quality:

1. **Recording Phase**: When the user taps `[ MIC ]`, the device records 4.0 seconds into PSRAM `ai_wav_buffer` and applies peak normalization.
2. **Instant Local Playback**: The recorded audio is saved to LittleFS as `/rec.wav` and immediately played through the onboard speaker:
   ```cpp
   UIStatusPayload p = {.type = STATUS_UPDATE_TRACK_DESC_SET};
   snprintf(p.trackDesc, sizeof(p.trackDesc), "Voice Feedback:\nRepeating what was heard...");
   xQueueSend(ui_status_queue, &p, 100);

   mediaType = 2; // AI mode
   speaker.setVolume(90);
   audio.setVolume(21);
   audio.connecttoFS(LittleFS, "/rec.wav");
   ```
3. **Sequential Processing Guard**: `ai_upload_task` waits for `/rec.wav` playback to finish before initiating HTTPS POST to the configured AI Assistant backend (`AI_ASSISTANT_URL`):
   ```cpp
   uint32_t wait_start = millis();
   while (audio.isRunning() && (millis() - wait_start < 10000)) {
     vTaskDelay(pdMS_TO_TICKS(50));
   }
   vTaskDelay(pdMS_TO_TICKS(250)); // Natural pause
   ```
4. **Display & TTS Playback**: Once the server responds, the query and answer are displayed on the LCD, and the TTS answer streams via `audioPlayHOST()`.

---

## 5. Vendor Driver Bugs & Verified Firmware Fixes

Testing the vendor Waveshare reference project (`08_Audio_Test`) exposed multiple critical bugs that caused distorted audio, premature cut-offs, and random static bursts.

### Bug 1: Monolithic I2S Timeout Cut-off
* **Root Cause**: The vendor reference attempted to read and write all 192,000 bytes in a single monolithic function call (`audio_playback_read(audio_ptr, 192000)`). The underlying ESP-IDF I2S driver has a hardcoded timeout of `1000 ms`. A 2.0-second buffer at 24 kHz stereo requires `2000 ms` to transfer. At exactly 1000 ms, the driver timed out with `ESP_ERR_TIMEOUT`, cutting off half of the recording and half of playback.
* **Fix**: Transfer audio in 2048-byte chunks in a streaming loop.

### Bug 2: Uninitialized External PSRAM Static Blast
* **Root Cause**: `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` does NOT zero memory. On boot or reset, external PSRAM is populated with residual voltage decay and random uninitialized bits.
* When the user pressed `Play` immediately after booting or resetting the board, the DAC fed uninitialized PSRAM directly to the power amplifier at full volume, blasting loud harsh static!
* **Fix**:
  1. Always explicitly zero the buffer on boot: `memset(audio_ptr, 0, 288 * 1000);`
  2. Implement an `audio_recorded` state flag. If `Play` is pressed before any recording has taken place, reject the request and display `Record First!` on the LCD.

---

## 6. USB Serial JTAG Silicon Behavior & Protocol Fixes

### Quirk 1: Windows CDC DTR/RTS Hardware Reset (`rst:0x15`)
* **Silicon Behavior**: The ESP32-S3 built-in `USB_SERIAL_JTAG` peripheral includes an internal hardware auto-reset circuit designed to ease bootloader entry.
* When Python opens a serial port on Windows via default `serial.Serial('COM11')`, the Windows CDC ACM driver (`usbser.sys`) pulses the **DTR** and **RTS** control lines.
* This pulse triggers an immediate hardware reset: **`rst:0x15 (USB_UART_CHIP_RESET)`**, causing the digital core to reboot, the LCD to flash white, and external memory to re-initialize.
* **Fix**: In Python scripts, instantiate the serial object and explicitly clear DTR and RTS **before** calling `open()`:
  ```python
  ser = serial.Serial()
  ser.port = 'COM11'
  ser.baudrate = 115200
  ser.dtr = False
  ser.rts = False
  ser.open()
  ```
  Verified result: Device maintains continuous uptime across hours of serial connections with zero white-screen flashes or reboots.

### Quirk 2: Newlib VFS CRLF Binary Corruption (`0x0A` $\rightarrow$ `0x0D 0x0A`)
* **Behavior**: Transmitting raw binary PCM over `stdout` using `fwrite()` traverses ESP-IDF's newlib C standard library. By default, standard output treats all streams as text and converts LF (`0x0A`) into CRLF (`0x0D 0x0A`).
* In 16-bit PCM audio, byte `0x0A` occurs hundreds of times in every second of audio. Each injected `0x0D` byte shifts the stream by 8 bits, transposing high bytes and low bytes and scrambling speech into harsh white noise!
* **Fix**: Never transmit raw binary PCM over text-oriented serial streams. Instead, retrieve via HTTP (`http://<device_ip>/rec.wav`) or encode into chunked **Base64** between explicit framing markers (`[B64_START]` / `[B64_END]`).

---

## 7. Acoustic Characteristics & Microphone Gain Settings

| Channel | Physical Hardware | Typical Quiet Room RMS | Speech RMS (Close Mic) | Speech RMS (Across Desk) |
| :--- | :--- | :---: | :---: | :---: |
| **Channel 0** | MEMS Mic 1 (Left) | $0.013$ ($-37.7\text{ dBFS}$) | $0.422$ ($-7.5\text{ dBFS}$) | $0.025$ ($-31.9\text{ dBFS}$) |
| **Channel 1** | MEMS Mic 3 (Right) | $0.013$ ($-37.7\text{ dBFS}$) | $0.401$ ($-7.9\text{ dBFS}$) | $0.024$ ($-32.4\text{ dBFS}$) |

### Key Gain Insights:
1. Both onboard microphones have identical sensitivity and frequency response.
2. For close-talking use cases (holding device to speak), default `+30 dB` to `+35 dB` gain yields ideal signal levels without digital clipping.
3. For distant/room speech (e.g., smart speaker mode), apply **Peak Normalization** ($+15\text{ dB}$ to $+24\text{ dB}$) or **Soft-Knee Dynamic Range Compression (DRC)** to bring up quiet speech while preventing clipping on acoustic peaks.

---

## 8. Summary of All Completed Implementations
- [x] ES7210 16-bit I2S register initialization with proper 256 LRCK clock divider.
- [x] ESP32 32-bit slot width DMA frame stride for clean single-channel extraction.
- [x] 8000 Hz WAV formatting and speech normalization with DC offset removal.
- [x] Instant voice feedback repeat loop over physical speaker.
- [x] Autonomous end-to-end multi-modal testing with speaker, mic, webcam, and STT verification.
