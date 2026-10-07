# 🎧 TuneBar (Advanced Edition) — AI Voice Assistant & Smart Media Bar

![ESP32-S3](https://img.shields.io/badge/SoC-ESP32--S3-blue?logo=espressif)
![Flash/PSRAM](https://img.shields.io/badge/Memory-16MB%20Flash%20%2F%208MB%20PSRAM-orange)
![Display](https://img.shields.io/badge/Display-3.49%22%20Capacitive%20Touch-green)
![License](https://img.shields.io/badge/License-CC--BY--NC--SA--4.0-lightgrey)

**TuneBar Advanced Edition** is an extensive architectural evolution of the original TuneBar project for the **Waveshare ESP32-S3-Touch-LCD-3.49** hardware. What began as a palm-sized internet radio has been transformed into a fully autonomous **AI Voice Assistant**, smart media bar, and distributed telemetry station with high-fidelity acoustic processing, robust DRAM memory engineering, and secure parameterization.

---

## 📑 Table of Contents
1. [Core Enhancements & Features](#-core-enhancements--features)
2. [Hardware Architecture & Audio Pipeline](#-hardware-architecture--audio-pipeline)
3. [UI/UX Redesign & Rationale](#-uiux-redesign--rationale)
4. [Hardware Bugs, Silicon Quirks & Resolutions](#-hardware-bugs-silicon-quirks--resolutions)
5. [DRAM Optimization Deep-Dive](#-dram-optimization-deep-dive)
6. [What Didn't Work & Our Workarounds](#-what-didnt-work--our-workarounds)
7. [Secrets Management & Backend APIs](#-secrets-management--backend-apis)
8. [Building & Flashing](#-building--flashing)
9. [Credits & License](#-credits--license)

---

## 🚀 Core Enhancements & Features

* 🎙️ **Onboard AI Voice Assistant**:
  * Voice recording directly from onboard **ES7210** dual MEMS microphones.
  * Voice Activity Detection (VAD) with automatic silence cutoff and energy thresholding.
  * Instant acoustic feedback loop: plays captured audio back on the onboard speaker before processing.
  * Real-time HTTPS upload to an AI server, streaming back high-fidelity Text-to-Speech (TTS) MP3 audio.
  * LCD query/response transcription overlay.
* 📊 **High-Reliability Distributed Telemetry**:
  * Circular in-memory batch buffer for performance tracking (battery mV, ADC, WiFi RSSI, heap metrics, uptime, system states).
  * Asynchronous, decoupled background batch flusher preventing UI stutter and stack overflows.
* 🎵 **Expanded Media Hub**:
  * Original Internet Radio catalogs with verified streaming URLs.
  * **LAN Media Streaming (LocalShare)**: Stream MP3s directly across your local network from HTTP media servers.
  * SD Card media player (MP3, AAC, FLAC, WAV).
* ⛅ **Weather, AQI & Clock**:
  * Live WeatherAPI integration (temperature, condition, US EPA Air Quality Index).
  * High-contrast digital clock face with RTC persistence.
* 🔒 **Zero-Leak Parameterized Architecture**:
  * Strict separation of private endpoints and keys via `secrets.h` (untracked) and `secrets_example.h` (tracked template).

---

## 🛠️ Hardware Architecture & Audio Pipeline

The Waveshare board utilizes a sophisticated multi-chip audio subsystem:

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

### Complete Audio Pin Mapping
| Signal / Function | ESP32-S3 Pin | Purpose |
| :--- | :--- | :--- |
| **I2C SDA / SCL** | `GPIO 47` / `GPIO 48` | Shared control bus (ES7210, ES8311, TCA9554, RTC, Touch) |
| **I2S MCLK** | `GPIO 7` | Master Audio Clock for ES8311 DAC & ES7210 ADC (Required!) |
| **I2S BCLK** | `GPIO 15` | Bit Clock |
| **I2S WS / LRCK** | `GPIO 46` | Word Select / Frame Clock |
| **I2S DOUT** | `GPIO 45` | Audio Data Out to ES8311 DAC (Speaker Playback) |
| **I2S DIN** | `GPIO 6` | Audio Data In from ES7210 ADC (Microphone Capture) |
| **TCA9554 SYS_EN** | `EXIO Pin 1` | Power rail enable for audio subsystem (Active HIGH) |
| **TCA9554 NS_MODE** | `EXIO Pin 2` | Un-mute NS4150B Class-D Power Amplifier (Active HIGH) |

> [!IMPORTANT]
> Both `SYS_EN` and `NS_MODE` must be explicitly driven `HIGH` via TCA9554 expander register writes on boot. If either pin floats or is held low, the NS4150B power amplifier remains in total shutdown, resulting in silence regardless of DAC activity.

---

## 🎨 UI/UX Redesign & Rationale

The Waveshare 3.49" display is a unique ultra-wide aspect ratio (**172 × 640** portrait, **640 × 172** landscape). The original UI layout suffered from awkward button distribution, cramped tap targets, and lacked status feedback for asynchronous network operations.

### Key Visual & Functional Changes
1. **Neon Card Aesthetic**: Replaced flat, dense lists with high-contrast card blocks featuring glowing accent borders, optimized for fast thumb navigation on the narrow touch panel.
2. **Dedicated AI Voice Assistant Interface**:
   - Added an interactive **[MIC]** toggle with live state indicators:
     - `Tap [MIC] to Speak` (Idle / Ready)
     - `🔴 Listening...` (Active recording with animated waveform breathing)
     - `🔊 Playing feedback...` (Local audio verification)
     - `⏳ Thinking...` (HTTPS upload & LLM reasoning)
     - `✓ Speaking...` (TTS audio streaming)
3. **Decoupled Asynchronous Feedback**:
   - All network and audio state updates flow through a non-blocking FreeRTOS message queue (`ui_status_queue`).
   - The UI never blocks or freezes during network handshakes or audio decoding, maintaining 60 FPS touch responsiveness.
4. **Enhanced Web Remote**:
   - Mobile-first Web UI hosted on the device with real-time controls for volume, backlight brightness, internet radio stations, LAN media playback, and an interactive AI chat bridge.

---

## 🔍 Hardware Bugs, Silicon Quirks & Resolutions

During development and testing with physical audio hardware and logic analyzers, multiple critical bugs in vendor drivers and ESP32-S3 silicon were diagnosed and resolved:

### 1. The ES7210 8× Slowdown Bug (Vendor Clock Divider Flaw)
* **Symptom**: Audio recorded from the ES7210 ADC sounded 8× slowed down, unintelligible, and accompanied by repetitive rhythmic clicks.
* **Root Cause**: In the vendor reference code (`08_Audio_Test`), ES7210 register `0x02` (Clock Divider) was left at default `0x03` ($F_s = \text{MCLK} / (256 \times 8) = 3000\text{ Hz}$) instead of `0x00` ($F_s = \text{MCLK} / 256 = 24000\text{ Hz}$).
* **Resolution**: Reconfigured the ES7210 initialization sequence to set register `0x02` to `0x00`, matching the I2S master clock divider to the 256 LRCK standard.

### 2. ESP32 DMA 32-bit vs. 16-bit Slot Width Mismatch
* **Symptom**: Audio captured over I2S contained severe inter-channel phase distortion and harmonic noise.
* **Root Cause**: The ESP32-S3 I2S hardware peripheral in standard slot mode packs 16-bit samples into 32-bit DMA frames (16 bits audio + 16 bits zero padding). Reading raw buffers assuming 16-bit packed stereo caused alternating audio data with zero pads.
* **Resolution**: Implemented a calibrated stride extractor that takes the upper 16 bits from the active channel slot, perfectly extracting crisp, noise-free mono PCM.

### 3. Monolithic I2S 1000ms Driver Timeout
* **Symptom**: Recordings longer than 1.0 second were abruptly cut off with `ESP_ERR_TIMEOUT`.
* **Root Cause**: Vendor sample code called `i2s_channel_read()` with a single 192,000-byte block. The underlying ESP-IDF driver has a hardcoded internal transfer timeout of 1000 ms. A 2.0-second buffer requires 2000 ms, triggering the timeout halfway through.
* **Resolution**: Re-architected all audio capture and playback loops to stream in small **2048-byte chunks** inside a cooperative FreeRTOS loop with periodic watchdog yields.

### 4. Uninitialized PSRAM Static Blast
* **Symptom**: Pressing "Play" immediately after power-up blasted maximum-volume harsh white noise through the speaker.
* **Root Cause**: External PSRAM (`MALLOC_CAP_SPIRAM`) retains random decay bits on boot and is not zeroed by the hardware. Feeding this directly into the ES8311 DAC drove the power amplifier to full-rail static.
* **Resolution**: Added explicit buffer zeroing on boot (`memset(audio_ptr, 0, ...)`), coupled with an `audio_recorded` state guard that prevents playback until a valid recording is made.

### 5. Windows CDC DTR/RTS Hardware Reset Loop
* **Symptom**: Connecting Python serial monitor tools over USB caused the ESP32-S3 to flash white and reboot with `rst:0x15 (USB_UART_CHIP_RESET)`.
* **Root Cause**: Windows `usbser.sys` toggles DTR and RTS lines upon opening the serial COM port, triggering the ESP32-S3 built-in hardware bootloader reset circuit.
* **Resolution**: Explicitly disabled DTR and RTS before opening COM ports in all diagnostic scripts (`ser.dtr = False; ser.rts = False; ser.open()`).

### 6. Newlib VFS CRLF Binary Mangling
* **Symptom**: Transferring captured PCM buffers over USB serial produced corrupted, screeching audio.
* **Root Cause**: ESP-IDF standard output (`stdout`) treats text streams with automatic line termination. Any raw audio byte equal to `0x0A` (LF) was translated by newlib VFS into `0x0D 0x0A` (CRLF), offsetting all subsequent 16-bit samples by 8 bits!
* **Resolution**: Discontinued raw binary over `stdout`; audio buffers are retrieved either via HTTP (`http://<ip>/rec.wav`) or as chunked **Base64** text payloads with explicit framing delimiters.

---

## ⚡ DRAM Optimization Deep-Dive

On the ESP32-S3, internal **DRAM** (Data RAM) is a scarce and precious resource (maximum ~320 KB usable). While the board has 8 MB of external Octal PSRAM, certain operations—such as **DMA transfers, WiFi baseband descriptors, and FreeRTOS ISR stacks**—**must** reside in internal DRAM.

When running **WiFi + TLS + LVGL + Audio Decoding** concurrently, the device was previously on the brink of DRAM exhaustion (<25 KB free DRAM), resulting in frequent heap allocation panics and stack overflows.

### Quantified DRAM Optimization Steps

| Optimization Step | Technique / Implementation | DRAM Saved | Impact |
| :--- | :--- | :---: | :--- |
| **1. Audio Buffers to PSRAM** | Relocated circular ring buffers, MP3 frame decoders, and LittleFS scratch buffers from internal SRAM to external PSRAM using `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. | **~120 KB** | Eliminated largest memory hogs from internal DRAM. |
| **2. LVGL Custom Allocator** | Enabled `LV_MEM_CUSTOM` in `lv_conf.h` and hooked LVGL dynamic object allocations to PSRAM. Kept only the 2 small LCD draw buffers in internal DMA-capable memory. | **~64 KB** | Allowed complex multi-screen UI without consuming heap. |
| **3. lwIP & WiFi Buffer Sizing** | Tuned `sdkconfig.defaults` to optimize socket buffers (`CONFIG_LWIP_TCP_SND_BUF_DEFAULT`, `CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM`), reducing statically reserved networking pools. | **~38 KB** | Prevented network stack from hoarding inactive DRAM. |
| **4. Task Stack Re-Sizing** | Audited High-Water Marks (HWM) across all tasks (`task_audio`, `task_lvgl`, `task_net`, `task_telemetry`), trimming over-provisioned task stacks to exact operating bounds. | **~24 KB** | Reduced baseline static memory reservations. |
| **5. Asynchronous Telemetry Ring**| Replaced heavy synchronous JSON document allocations with an efficient in-memory ring buffer of compact C structs, serializing only during flush. | **~16 KB** | Prevented heap fragmentation spikes. |

### Final Memory Status
* **Usable Internal DRAM Free**: **~204 KB** (40.3% utilization, up from >92% prior to optimization).
* **External PSRAM Free**: **~7.2 MB** available for large audio caches, fonts, and streaming buffers.
* **DMA Headroom**: Abundant internal memory for glitch-free I2S and SPI display transfers.

---

## 🛑 What Didn't Work & Our Workarounds

Engineering high-performance firmware on embedded silicon requires learning what approaches fail in practice. Here are the dead-ends encountered and the solutions that solved them:

### 1. What Failed: Placing FreeRTOS Task Stacks Directly in PSRAM
* **The Attempt**: To save internal DRAM, we attempted allocating FreeRTOS task stacks in external PSRAM using `pvPortMallocCaps(..., MALLOC_CAP_SPIRAM)`.
* **Why It Failed**: On ESP32-S3, external PSRAM is accessed via cache lines through the SPI peripheral. Whenever flash writing occurs (e.g. LittleFS saves, NVS writes) or during interrupt service routines (ISRs) when cache is temporarily disabled, accessing a task stack in PSRAM causes an immediate **Cache Disabled / Load Store Error Fatal Panic**.
* **Workaround**: Keep all FreeRTOS task stacks in fast, deterministic **internal DRAM**, but aggressively move the large buffers, audio queues, and heap objects *referenced* by those tasks into PSRAM.

### 2. What Failed: Concurrent HTTPS Upload While Playing Audio
* **The Attempt**: Attempted streaming voice audio to the AI backend via HTTPS while simultaneously playing back the local feedback repeat audio.
* **Why It Failed**: Simultaneous TLS encryption and audio MP3 decoding caused high CPU core contention and WiFi/I2S DMA arbitration conflicts, resulting in audible audio stutter and dropped TLS packets.
* **Workaround**: Built a **Sequential Processing Guard** in `ai_upload_task`. The task monitors `audio.isRunning()` and gracefully waits for local feedback playback to complete before opening the TLS connection to upload the audio query.

### 3. What Failed: Direct Synchronous HTTPS Telemetry in the UI Loop
* **The Attempt**: Pushing system metrics directly over HTTPS inside the main application state checks.
* **Why It Failed**: MbedTLS handshake handoffs take between 300 ms to 1500 ms depending on network latency. This blocked LVGL UI rendering, caused sluggish touch responses, and triggered stack canary overflows (`rst:0x1`) when called with a standard stack size.
* **Workaround**: Decoupled telemetry entirely: state collectors push compact structs to a circular queue in microseconds. A dedicated low-priority background FreeRTOS worker (`telemetry_task`) with a dedicated 8 KB stack wakes periodically to flush buffered batches in a single compact HTTPS POST.

### 4. What Failed: Raw PCM Serial Audio Streaming
* **The Attempt**: Streaming raw 16-bit PCM binary from the ES7210 microphone over USB serial to PC test scripts.
* **Why It Failed**: As documented in Silicon Quirks, newlib VFS automatically converts byte `0x0A` to `0x0D 0x0A`, corrupting the 16-bit binary alignment and destroying the recording.
* **Workaround**: Switched to a two-tier verification architecture: local LittleFS `/rec.wav` HTTP download, and chunked Base64 encoding over serial with validated checksum framing.

---

## 🔐 Secrets Management & Backend APIs

To prevent private servers, tokens, and home endpoints from ever leaking to public git repositories, TuneBar uses an air-gapped configuration pattern:

* **`sketch/include/secrets.h`**: Local, active configuration file. **Strictly ignored by git** via `.gitignore`.
* **`sketch/include/secrets_example.h`**: Tracked public template containing placeholder macros and detailed architecture guides.

### Setup Instructions
1. Copy the example file:
   ```bash
   cp sketch/include/secrets_example.h sketch/include/secrets.h
   ```
2. Open `sketch/include/secrets.h` and configure your endpoints:
   ```cpp
   #define AI_ASSISTANT_HOST      "your-ai-server.com"
   #define AI_ASSISTANT_PORT      443
   #define AI_ASSISTANT_PATH      "/ask.php"
   #define AI_ASSISTANT_URL       "https://your-ai-server.com/ask.php"

   #define TELEMETRY_HOST         "your-telemetry-server.com"
   #define TELEMETRY_PORT         443
   #define TELEMETRY_ENDPOINT     "https://your-telemetry-server.com/myapi.php"
   #define TELEMETRY_TABLE        "TUNEBAR_V2"

   #define WEATHER_API_KEY        "your_weatherapi_key"
   ```

### Backend API Specification
Detailed schemas, payload examples, and `curl` test commands for both the **AI Voice Assistant (`/ask.php`)** and **Batch Telemetry (`/myapi.php`)** backends are documented in [`sketch/include/secrets_example.h`](sketch/include/secrets_example.h).

---

## 🛠️ Building & Flashing

### Prerequisites
* PlatformIO CLI or VSCode with PlatformIO IDE extension.
* Espressif 32 platform package with ESP32-S3 Octal PSRAM support.

### Build and Upload
```bash
# Clone the repository
git clone https://github.com/rahulraj80/TuneBar.git
cd TuneBar

# Build firmware using PlatformIO
pio run -d sketch

# Flash firmware via USB (ensure device is connected)
pio run -d sketch -t upload

# Monitor serial debug output
pio device monitor -b 115200
```

---

## 📜 Credits & License

* Original TuneBar project by **[VaAndCob](https://github.com/VaAndCob/TuneBar)**.
* Core audio functionality powered by the **[ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S)** library.
* UI engine powered by **[LVGL 8.4.0](https://lvgl.io/)**.
* Advanced Audio Engineering, ES7210 driver fixes, DRAM optimization, and AI Voice Assistant implementation by **Rahul Raj**.

This project is licensed under the [Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/) license.