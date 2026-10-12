# TuneBar Multi-Repo Firmware Ecosystem & OTA Architecture Plan

---

## 1. Executive Summary & Goals
This document defines the complete architecture, UI design, security/safety interlocks, versioning standards, and multi-repository distribution pipeline for the TuneBar ESP32-S3 firmware over-the-air (OTA) updates.

Key pillars:
1. **Zero-USB OTA**: Full over-the-air update support directly via the Web Remote LAN interface or the on-device System App.
2. **Approved Community Repositories (`approved_repos.json`)**: Community forks and official releases can be browsed and selected directly from GitHub. Repositories report DRAM, PSRAM, and battery life metrics.
3. **Strict Numeric Semantic Versioning**: `v<Major>.<Minor>.<Patch>` (e.g. `v1.2.6`) strictly without alphanumeric suffixes.
4. **Hardware Safety Guard**: OTA blocked if battery is `< 70%` unless actively connected to USB/charger.
5. **App Teardown & Resource Freeing**: Shuts down audio, LAN streaming, IMU SSE streams, and frees buffers prior to flashing.
6. **Bold Neon Blue Visual Interface**: Full-screen `#000000` display with `#00E5FF` typography, warning message, progress bar, and transfer speed.
7. **Landscape Split Carousel UI (`640 × 172`)**: 200px stationary left card (badge, size, select button) + 420px right card (changelog with vertical auto-scroll, 2s bounceback pause, and 5s touch pause).
8. **On-Device Version Stepper**: Search and flash any historical or arbitrary release from GitHub via `v[▲▼].[▲▼].[▲▼]`.

---

## 2. Multi-Repo Architecture & Manifest Schema

### 2.1. Approved Repositories Registry (`approved_repos.json`)
Hosted on GitHub (official repo: `https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/approved_repos.json`):

```json
{
  "approved_repos": [
    {
      "id": "official",
      "name": "TuneBar Official",
      "author": "rahulraj80",
      "repo_url": "https://github.com/rahulraj80/TuneBar",
      "catalog_url": "https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/firmware_catalog.json",
      "description": "Official firmware with HiFi radio, LAN streaming, Nixie clock, vector UI & IMU auto-rotation",
      "is_official": true,
      "metrics": {
        "dram_free_avg_kb": 182,
        "psram_free_avg_mb": 4.8,
        "battery_life_clock_hours": 14.5,
        "battery_life_active_hours": 5.2
      }
    }
  ]
}
```

### 2.2. Firmware Catalog Schema (`firmware_catalog.json`)
Each repository maintains up to 6 curated releases (2 Major, 2 Minor, 2 Patch):

```json
{
  "repo_id": "official",
  "releases": [
    {
      "version": "1.2.6",
      "type": "patch",
      "build": 48,
      "date": "2026-10-12",
      "title": "Breathing 10s Rhythm & Wi-Fi Power Save",
      "changelog": "10s breathing cycle with 5s full black, 2s hold high. Wi-Fi turned off in clock mode. Real-time battery telemetry beacon.",
      "url": "https://raw.githubusercontent.com/rahul/TuneBar/main/releases/bin/firmware-v1.2.6.bin",
      "size": 6323168
    }
  ]
}
```

---

## 3. Strict Numeric Semantic Versioning

- Format: strictly `v<Major>.<Minor>.<Patch>`.
- Increments:
  - **Major** (1st part): Architectural / core framework changes (e.g. `2.0.0`).
  - **Minor** (2nd part): New modules, apps, major features (e.g. `1.3.0`).
  - **Patch** (3rd part): Bug fixes, calibration, timing adjustments (e.g. `1.2.6`).
- Automatic Build Injection:
  - Generated into `sketch/include/version.h` via `scripts/generate_version.py` on every build:
    ```c
    #define TUNEBAR_VERSION_MAJOR 1
    #define TUNEBAR_VERSION_MINOR 2
    #define TUNEBAR_VERSION_PATCH 6
    #define TUNEBAR_VERSION_STR   "1.2.6"
    #define TUNEBAR_BUILD_NUM     48
    #define TUNEBAR_GIT_HASH      "g7a8b9c"
    #define TUNEBAR_BUILD_DATE    "2026-10-12 08:00:00"
    ```

---

## 4. Hardware Safety & Resource Management

### 4.1. Battery & Charger Interlock
- Threshold: `Battery >= 70%` **OR** `Charger Connected` (USB VBUS / charging flag detected).
- If `< 70%` and unplugged:
  - On Device: Displays amber warning modal stating battery is below 70% and requires connecting the charger.
  - Web Remote API: Returns `HTTP 403 Forbidden` with JSON:
    ```json
    {
      "status": "blocked",
      "reason": "battery_too_low",
      "battery_pct": 58,
      "required_pct": 70,
      "message": "OTA update blocked: Battery is 58%. Minimum 70% required or connect charger."
    }
    ```

### 4.2. Application Teardown Prior to Flashing
When update is triggered:
1. `audio.stopSong()` mutes and halts ES8311 I2S codec.
2. LAN streaming tasks and client connections terminate.
3. IMU SSE streaming (`/api/imu/stream`) and sensor polling pause.
4. Screen auto-off timer disabled (`SCREEN_OFF_DELAY = 0; BL_OFF = false;`).
5. FreeRTOS background telemetry timers suspend.
6. Dynamic DRAM & PSRAM heap buffers are flushed to ensure maximum transfer headroom.

### 4.3. Partition Rollback Safety
- Uses `esp_ota_mark_app_valid_cancel_rollback()`.
- On boot into new firmware, a 10-second stability timer runs.
- If a panic/crash loop occurs within 10s, the bootloader automatically reverts to the previous working OTA partition.

---

## 5. User Interface Specifications (`640 × 172` Display)

### 5.1. System App Launcher (`ui_Screen_Utility`)
Grid of 4 evenly spaced circular icons across 640px (`x = 80, 240, 400, 560`):
1. 🔦 **Torch** (Flashlight utility)
2. ℹ️ **System Info** (Diagnostics & memory metrics)
3. 📐 **Spirit Level** (Placeholder for future inclinometer app)
4. ⚡ **Firmware & OTA** (Firmware Manager & GitHub releases)

### 5.2. Panel 1: Firmware Manager Overview
- Displays:
  - Current firmware version, build number, git commit hash.
  - Active GitHub repository name and author.
  - Latest version available in the active repository.
  - Wi-Fi connection state (SSID, RSSI) with a direct link to Wi-Fi settings if disconnected.
  - Battery percentage and safety status badge.
- Navigation buttons:
  - `[ 🌐 Switch Repository ]`
  - `[ 📜 Browse 6 Releases ]`
  - `[ 🔍 Custom Version Search ]`
  - `[ ⚡ Quick Update to Latest ]`

### 5.3. Panel 2: Landscape Split Carousel (Releases 1 of 6)
- **Left Stationary Column (200px)**:
  - Version Badge (🟢 `PATCH`, 🔵 `MINOR`, 🟣 `MAJOR`).
  - Release Date and Binary Size.
  - Battery safety check status.
  - Prominent `[ ⚡ Select & Flash ]` button.
- **Right Scrolling Column (420px)**:
  - Changelog and release notes container.
  - Vertical auto-scroll slowly downwards.
  - Reaching bottom: pauses for **2.0 seconds** $\to$ smoothly bounces back to top $\to$ pauses **2.0 seconds** $\to$ scrolls down again.
  - **Touch Override**: Touching or dragging the text immediately suspends auto-scroll. After **5.0 seconds** of touch inactivity, auto-scroll resumes.

### 5.4. Panel 3: Custom Version Stepper
- Clean numeric steppers for ultra-wide screen:
  `v [ ▲ 1 ▼ ] . [ ▲ 2 ▼ ] . [ ▲ 6 ▼ ]` $\to$ `[ 🔍 Query GitHub ]`.
- Queries active repository for tag `vX.Y.Z`.
- If found: renders the 200px/420px split card with changelog and enables flashing.
- If not found: displays "Version not found on GitHub repository".

### 5.5. Panel 4: Bold Neon Blue Flashing Overlay
- Full-screen `#000000` overlay.
- Text in `#00E5FF` (Bold Neon Blue):
  - `⚡ OTA UPDATE IN PROGRESS ⚡`
  - `DO NOT TOUCH DEVICE OR DISCONNECT POWER`
  - `Installing: vX.Y.Z (Build #... from <Repo>)`
- Thick neon-blue glowing progress bar (0–100%).
- Live metrics: Downloaded / Total MB, Transfer Speed (KB/s).
- On completion: Turns neon green with reboot countdown.
- On first boot: Top pill banner for 4s: `✨ Successfully updated to vX.Y.Z (Build #...)`.

---

## 6. Web Remote Interface Specifications

In `http://192.168.3.4/`:
1. **Repository Selector**:
   - Dropdown of all approved repositories with DRAM/PSRAM stats and battery estimates.
   - Selecting a repo refreshes the release catalog.
2. **Local .bin File Upload**:
   - File picker to select and upload any local `firmware.bin`.
3. **GitHub Release Puller**:
   - Select from the 6 curated releases or type a custom numeric version `vX.Y.Z`.
   - Displays release notes, date, and binary size.
   - `[ Fetch & Flash from GitHub ]` button: Web Remote streams the release directly to the device with live progress bar.
4. **Battery Interlock**:
   - Warns and blocks upload if battery `< 70%` and charger disconnected.

---

## 7. Implementation Roadmap & Milestones

| Phase | Description | Deliverables |
|---|---|---|
| **Phase 1** | **Bootstrap & Versioning** | Flash current build to COM11 to enable Web Remote OTA; create `generate_version.py` build hook. |
| **Phase 2** | **Power Guard & Teardown** | Implement battery/charger safety check, app teardown routine, and bold neon blue LVGL overlay. |
| **Phase 3** | **Catalogs & Web Remote** | Create `approved_repos.json` & `firmware_catalog.json`; implement repo switching & GitHub release puller in Web Remote. |
| **Phase 4** | **System App OTA UI** | 4-icon layout in `ui_Screen_Utility`, Overview Panel, 200px/420px Split Carousel with auto-scroll & 5s touch pause. |
| **Phase 5** | **Custom Version Stepper** | Numeric stepper `v[▲▼].[▲▼].[▲▼]` querying GitHub releases. |
| **Phase 6** | **Verification & Docs** | Test OTA updates over Wi-Fi, verify battery interlocks, test rollback watchdog, update README. |
