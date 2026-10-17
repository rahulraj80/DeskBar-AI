## Version history

### 2.5.1 (Oct 17th, 2026)
- **Visual Hierarchy Refinement**: Exchanged the positions of the countdown renewal timer and "30-DAY ACCESS" badge in the Cloud & PIN setup modal:
  - Countdown timer `(%02us)` placed on the sub-header row beside `"WEB ACCESS CODE:"`.
  - `"30-DAY ACCESS"` badge placed beside the prominent 20pt web access code.

### 2.5.0 (Oct 17th, 2026)
- **Two-Column Cloud & PIN Setup Modal**:
  - Expanded dialog card to 624x160 with clean two-column division and vertical separator.
  - Left column: 4 interactive PIN digits + large target table badge (`TABLE: TB_<hash>`).
  - Right column: Large prominent 20pt cyan web access code `[ xxx xxx ]` with countdown timer and privacy guarantee.
  - Side-by-side action buttons: `CONFIRM & CONNECT` (emerald) and `STAY OFFLINE` (amber).
- **Radio Screen Real-Time Stream Quality Telemetry**:
  - Top-row telemetry beside station index (`1 of 10`): Audio Bitrate (`128k`), Download Speed (`DL: 128k`), and PSRAM Buffer Percentage (`BUF: 92%`).
  - Dynamic buffer color-coding: Green ($\ge 50\%$), Amber ($20-49\%$), Red ($< 20\%$) to indicate stream health and assist users in avoiding stuttering stations.

### 2.4.0 (Oct 17th, 2026)
- **Dynamic TOTP Hardware Cloud Authentication**:
  - Hardware-bound 6-digit rolling TOTP code generated on-device with SHA-256 HMAC and countdown timer.
  - Implemented `TB_DEVICE_AUTH` registry table with automated table provisioning on backend.
  - Interactive TOTP challenge modal on `telemetry.php` and `graph.php` issuing 30-day encrypted authenticated browser sessions.
- **Oracle Autonomous DB Scalability**: Verified PGA/SGA memory allocation and cloud tablespace integrity for high-concurrency fleet telemetry.

### 2.3.7 (Oct 17th, 2026)
- **Audio Continuity & SD Card Stability**: Fixed audio dropouts during clock PWM breathing transitions and resolved FATFS PSRAM DMA bounce buffer contention during SD card playback.

### 2.3.5 (Oct 17th, 2026)
- **Async Cloud Battery Calibration Sync**: Offloaded cloud battery discharge curve synchronization to dedicated FreeRTOS background task with live pull timestamp feedback.

### 2.3.4 (Oct 17th, 2026)
- **Fine-Grained Backlight Control**: Added fine-grained backlight stepping below 100 PWM, refreshed utility icons, and scrollable power profile manager.

### 2.0.5 (Oct 13th, 2026)
- **Throttled Stream Replacement**: Replaced server-throttled web streams with high-bandwidth CDN alternatives (Bombay Beats to regional load balancer, added Radio Paradise Rock, RdMix Classic Rock, and Radio Caroline 259).
- **Buffer Standardization**: Standardized audio prebuffering to 64 KB across all stations.
- **Safe Serial Monitor Tooling**: Implemented non-resetting serial CLI & framebuffer capture script (`safe_serial_monitor.py`) eliminating unexpected hardware resets.

### 2.0.4 (Oct 13th, 2026)
- **Buffer Underflow & Recovery Telemetry**: Added deep telemetry tracking and automated recovery callbacks on stream starve.

### 2.0.3 (Oct 13th, 2026)
- **PSRAM Webstream Prebuffering**: Increased webstream prebuffer to 256 KB in PSRAM to eliminate stutter on 256 kbps radio stations.

### 2.0.2 (Oct 13th, 2026)
- **gTTS Speech Fallback**: Added speech synthesis fallback and eliminated UI freeze when Edge-TTS audio returns empty or fails.

### 2.0.1 (Oct 13th, 2026)
- **AI Authentication Guard**: Fixed AI assistant device authentication headers and restricted status handling against device registry.

### 2.0.0 (Oct 13th, 2026)
- **Major Architecture Rewrite**: Re-architected versioning, security consent models, and accessibility features.

### 1.3.0 (Oct 12th, 2026)
- **Interactive Security PIN Setup**: First-boot 4-digit Master PIN configuration with hardware TRNG default (`esp_random()`).
- **Masked Protection & Erase**: Added masked PIN UI protection and OTA secrets erase workflow.

### 1.2.9 (Oct 12th, 2026)
- **First-Boot Registration Modal**: Full-screen community cloud consent dialog, telemetry unblocking, initial ping-home, and web remote secrets manager.

### 1.2.8 (Oct 12th, 2026)
- **4-Row Orientations & Precision Shelf Level**: Added 4-row named posture tracking, dynamic wireframe, and dynamic release catalog pipeline.

### 1.2.7 (Oct 12th, 2026)
- **Tactical UI Glyphs & Vector Refinements**: Refined Spirit Level tactical UI glyphs, slider tracks, and calibration badges.

- **Procedural LVGL Vector Themes**: Replaced 8 heavy bitmap backgrounds with procedural LVGL vector gradients and styles, slashing flash consumption from 6.44MB to 4.06MB (2.21MB flash freed).
- **Dynamic Multi-Repo Firmware OTA Engine**: Replaced static mock release tables with dynamic HTTPS catalog query (`firmware_catalog.json` and `approved_repos.json`) calculating actual binary sizing directly from integer byte counts.
- **On-Device Version Stepper & GitHub Binary Probing**: Step through any historical release; queries GitHub directly with HTTP HEAD if not present in the curated catalog.
- **3-Panel Spirit Level App with 8-Step Calibration**: Bullseye bubble reticle, calibrated IMU telemetry with configurable tolerance, custom named orientation presets, and 8-step guided calibration wizard persisting bias to NVS and synchronizing with display auto-rotation.
- **Battery Safety Interlock**: Firmware flash strictly blocked below 70% battery unless actively connected to USB charger.
- **Clock Breathing & Wi-Fi Power Save**: 10s rhythm (5.0s black, 1.5s fade in, 2.0s hold high, 1.5s fade out) with Wi-Fi modem sleep saving ~50mA quiescent current.
- **10-Second Breathing Cycle with 5-Second Full Black**: Implemented exact 10s rhythm: 5.0s `FULL_BLACK` (0% PWM), 1.5s `FADE_IN` (90 $\to$ ceiling), 2.0s `HOLD_HIGH` (at ceiling), 1.5s `FADE_OUT` (ceiling $\to$ 90 $\to$ full black).
- **FreeRTOS LVGL Timer Freeze Fix**: Prevented `BL_OFF` from being asserted during breathing dark phase, guaranteeing LVGL timers run continuously without locking the screen dark.
- **Clock Sleep Lock Guard in `xtask.h`**: Prevented idle timeout from triggering `screenPowerOff()` on the Nixie Clock screen; idleness safely returns to ambient breathing.
- **Continuous Wi-Fi in Clock Mode**: Kept Wi-Fi online during clock breathing so Web Remote and LAN OTA updates remain accessible without requiring screen wake.
- **Continuous Battery Beacon**: `[BEACON]` logs real-time voltage and percentage (`Batt: 4.12V (91%)`) on every broadcast for automated discharge telemetry.
- **CLI `bl` Robustness**: Added digit checks for raw PWM argument parsing to avoid accidental screen sleep on invalid input.

### 1.2.4 (Oct 11th, 2026)
- **QMI8658 6-Axis IMU & Landscape Auto-Rotation**: Full integration of onboard accelerometer & gyroscope on shared I2C bus.
- **Strict Landscape Auto-Rotation**: Dynamically auto-rotates display between $0^\circ$ Normal Landscape and $180^\circ$ Inverted Landscape with 350ms hysteresis debounce filtering; locks and ignores portrait and flat positions.
- **Synchronized Coordinate Mapping**: Synchronous 180° inversion for AXS15231B touch driver and `/api/screenshot` BMP framebuffer dumping.
- **Orientation as User Interaction**: Physical flipping/rotation wakes display to full active brightness (`Low: 120`, `Med: 180`, `High: 250`), activates the AP3032 boost backlight rail, pauses clock breathing, and arms a 15-second inactivity timeout before breathing cycle re-engages.
- **Vintage Nixie Tube Breathing Engine**: Enforced hardware floor of 90/255 PWM to maintain a warm vacuum tube glow without going black. Ramps smoothly between 90 and `max_bl` (120/180/250) without cutting the boost power rail.
- **CLI Scale of 255**: `bl <0-255>` sets raw PWM duty directly on a scale of 255 with automatic breathing pause for inspection.
- **Web Remote Card 10: Firmware OTA Update**: Direct browser-based `firmware.bin` multipart file upload via `/api/update` with live percentage progress bar and automatic reboot.
- **Web Remote REST API & SSE Streaming**: Added `/api/imu` and `/api/imu/stream` endpoints for real-time 6-axis motion telemetry, manual orientation control, and SSE live streaming.
- **Diagnostics & Monitoring Tools**: Added `log_breathing_120m.py` for continuous 120-minute discharge logging and `monitor_accel.py` for live terminal motion streaming.

### 1.2.3 (Oct 8th, 2026)
- Fix Settings -> Music SD Card scan crash: added null guards for cross-screen UI widgets.
- Fix directory recursion stack canary overflow by moving path buffers to PSRAM.
- Added O(1) Fast Seek PSRAM Track Offset Table (0ms track access, 0 bytes DRAM).
- Added graceful failure handling when SD card is not present or unmounted.
- Improved playback display with instant clean track title extraction and continuous autoplay queue.

### 1.2.2 (June 17th, 2026)
- Fix RTC return wrong data make app crash for sometime.
- Fix random play the same sequence.
- Stability improved

### 1.2.1 (Feb 10th, 2026)
- Wrong path audio files indexing function fixed

### 1.2.0 (Jan 25th, 2026)
- First release

https://www.youtube.com/watch?v=7HEO5P5ezfM
