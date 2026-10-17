# TuneBar Power Optimization & Remote Control Architecture Plan

**Target Device:** Waveshare ESP32-S3-Touch-LCD-3.49 (Rev 1.1)  
**Base Repository:** [VaAndCob/TuneBar](https://github.com/VaAndCob/TuneBar) at commit `338143c` (v1.2.2)  
**Workspace:** `C:\Users\rahul\OpenCode\TuneBar`  
**Date:** September 2026

---

## 1. Objectives & Architectural Constraints

1. **Maximize Battery Life During Screen-Off:**
   - Eliminate all unnecessary power draw during timeout and screen lock.
   - Handle two distinct screen-off states:
     - **Active Playback:** Streaming audio/radio over I2S while screen is off.
     - **Idle / Paused:** Screen timed out and no audio playing (deepest possible power reduction).
2. **Zero Additional Internal RAM Pressure (Critical Constraint):**
   - ESP32-S3 internal SRAM is heavily constrained by audio buffers and WiFi network stacks.
   - All newly introduced buffers must use static allocation, stack reuse, or external PSRAM (`MALLOC_CAP_SPIRAM`). No dynamic `malloc()` churn on the internal heap.
3. **Flawless UI & Audio Continuity:**
   - Instant, glitch-free screen wake-up without losing LVGL state or audio buffering.

---

## 2. Phase 1: Core Power-Down Architecture (Screen-Off Phase)

### A. Backlight & Power Rail Management (~80–120 mA Reduction)
* **Problem in Upstream:** Upstream TuneBar only dialed down LEDC PWM duty cycle (`setUpduty(LCD_PWM_MODE_0)`). It never cut the backlight power rail and left the boost converter energized, causing visible milky bleed and burning quiescent power.
* **Solution:**
  1. Pull `EXIO1` (`BL_EN` on the TCA9554 expander) **LOW** on lock: physically shuts down the AP3032 step-up boost converter (true 0 photons, 0 mA LED current).
  2. Set LEDC PWM duty to minimum on `GPIO1`.
  3. Re-enable `EXIO1` and restore user brightness on unlock.

### B. Display Controller & QSPI DMA Engine (~25–35 mA Reduction)
* **Problem in Upstream:** The AXS15231B LCD driver remained fully awake, and LVGL's flush callback continued pushing 220 KB frames across the 40 MHz Quad-SPI bus via GDMA.
* **Solution:**
  1. On lock: Issue display sleep commands (`0x28` DISPOFF / `0x10` SLPIN) to the AXS15231B.
  2. In `WAVESHARE_349_lvgl_flush_cb`: If `BL_OFF` is active, immediately call `lv_disp_flush_ready(drv); return;` without queuing QSPI DMA transactions.
  3. On unlock: Send `0x11` (SLPOUT) and `0x29` (DISPON), allow 20 ms wake settle, and resume QSPI flushes.

### C. Touch Controller Polling (~5–10 mA Reduction)
* **Problem in Upstream:** `WAVESHARE_349_lvgl_touch_cb` transmits an 11-byte I2C read transaction every 10–20 ms continuously.
* **Solution:**
  1. When `BL_OFF` is true, immediately return `data->state = LV_INDEV_STATE_REL` without performing I2C bus transactions.
  2. Screen wake-up is handled cleanly by the physical BOOT button or Power button. (Optionally configure `TP_INT` pin for hardware interrupt wake if tap-to-wake is enabled).

### D. Audio & Amplifier Power Management (~15–25 mA Reduction When Idle)
* **Problem in Upstream:** The onboard Class-D audio amplifier (`EXIO7`) remains permanently powered on even when audio is stopped or idle, draining quiescent bias current and creating background hiss.
* **Solution:**
  1. If screen is off **and** `audio.isRunning()` is `false` (or paused):
     - Pull `EXIO7` **LOW** to disable the power amplifier.
     - Put the ES8311 DAC into low-power mute/standby.
  2. If audio resumes: Pull `EXIO7` **HIGH** with a 50 ms pop-suppression delay.

### E. LVGL Task Throttling & UI Update Suppression (~10–15 mA Reduction)
* **Problem in Upstream:** Background FreeRTOS tasks (RTC clock, battery monitor, audio progress) post messages to `ui_status_queue` every second, causing continuous LVGL widget re-renders.
* **Solution:**
  1. While `BL_OFF`, increase the `lvgl_port_task` delay from `5–10 ms` to `100–250 ms`.
  2. Discard purely visual redraw invalidations (such as second-by-second Nixie clock image updates) while the screen is dark.

### F. WiFi Modem Sleep (~30–50 mA Reduction)
* **Solution:**
  - Call `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` after WiFi connects.
  - Allows the ESP32-S3 radio to sleep between AP beacon intervals (DTIM) without dropping the network socket or audio stream.

---

## 3. Phase 2: Remote Control ("ADB" Interface), Station Management & Logging

### A. Unified Command Console Engine ("ADB for TuneBar")
* **Design:** Transport-agnostic text command processor with a shared static 128-byte input buffer (zero heap allocation).
* **Dual Transports:**
  1. **Wired USB Serial:** Listens on `Serial` (`USB-CDC` or USB-Serial-JTAG).
  2. **WiFi / Network:** Lightweight TCP socket (port 2323) or REST/HTTP endpoint when WiFi is connected.
* **Supported Commands:**
  | Command | Description |
  | :--- | :--- |
  | `status` | Reports system state (Vbat %, Audio state, Track/Station, Screen state, Free Heap/PSRAM) |
  | `play [url/file]` | Start playback of specified stream or SD track |
  | `stop` | Stop audio playback and sleep amplifier |
  | `pause` / `resume` | Toggle audio pause |
  | `vol <0-21>` | Set volume level |
  | `station <index>` | Switch to radio station by index |
  | `station_list` | Dump current radio stations list |
  | `station_add <name>,<url>` | Append new station to LittleFS `/stations.csv` and reload |
  | `station_del <index>` | Remove station from `/stations.csv` and reload |
  | `bl <0-255>` | Adjust backlight brightness |
  | `lock` / `unlock` | Force screen lock (dark) or unlock |
  | `batt` | Read instant raw ADC mV, voltage, and calculated percentage |
  | `batt_log` | Dump stored battery log history |
  | `reboot` | Soft restart the ESP32-S3 |

### B. Radio Stations Management (No Recompilation Required)
* **Storage:** TuneBar already loads `/stations.csv` from LittleFS into an array in PSRAM (`stations[]`).
* **Implementation:**
  - Expose `station_add`, `station_del`, and `station_list` commands over wire and WiFi.
  - Editing updates `/stations.csv` on LittleFS and calls `loadStationList()`, re-populating the PSRAM array on the fly.
  - Zero RAM penalty: the `stations[]` structure is already resident in 8 MB PSRAM.

### C. Lightweight Battery Percentage Logging
* **Storage:** Append-only CSV log `/batt_log.csv` on LittleFS.
* **Record Format:** `timestamp_epoch,vbat_mv,percent,state` (approx. 24 bytes per entry).
* **Logging Interval:** Every 5 to 10 minutes (or on significant battery drop >= 2%).
* **Size Cap:** Maximum 144 entries (12 to 24 hours of history = ~3.5 KB total flash usage).
* **Pulling Log:** Available via `batt_log` console command over serial or HTTP download `/api/batt_log`.

### D. Periodic Serial Debug Beacon
* **Cadence:** Every 10 seconds (6 times per minute).
* **Format:** Single-line concise telemetry string:
  ```text
  [BEACON] Vbat: 3.94V (78%) | Screen: OFF | Audio: PLAYING [BBC Radio 1] | FreeHeap: 138KB | FreePSRAM: 6.78MB
  ```
* **Battery Impact:** Virtually zero (<0.01% CPU usage, non-blocking UART FIFO write).

---

## 4. RAM Pressure & Safety Guardrails

1. **Command Parser:** Uses a fixed-size `char cmd_buf[128]` in `.bss`. Zero dynamic `malloc()` calls.
2. **Battery Logger:** Writes directly to LittleFS file without holding arrays in internal SRAM.
3. **PSRAM Priority:** All heavy data structures (LVGL draw buffers, radio station arrays, audio metadata) remain strictly in PSRAM (`MALLOC_CAP_SPIRAM`).
4. **Stack Safety:** Existing FreeRTOS task stack sizes are audited to prevent stack overflows without needlessly allocating excess memory.

---

## 5. Summary Table: Power Consumption Projections

| Operating Mode | Original TuneBar | Phase 1 Optimized | Phase 2 (With Beacon & Modem Sleep) |
| :--- | :--- | :--- | :--- |
| **Screen Active + Audio Streaming** | ~190 – 240 mA | ~170 – 200 mA | ~150 – 180 mA |
| **Screen OFF + Audio Streaming** | ~140 – 170 mA | ~65 – 80 mA | **~50 – 65 mA** |
| **Screen OFF + Audio Paused/Idle** | ~120 – 150 mA | ~18 – 25 mA | **~12 – 18 mA** |
