# ⚡ On-Device Multi-Repo Firmware OTA Manager

TuneBar features a full-featured on-device Over-the-Air (OTA) firmware manager capable of querying official and community releases over Wi-Fi and flashing updates directly without a computer.

---

## 📸 Interface Showcase

<p align="center">
  <img src="../document/screen_ota_overview.png" alt="Firmware Manager Overview" width="48%" />
  <img src="../document/screen_ota_carousel.png" alt="Firmware Manager Carousel" width="48%" />
</p>
<p align="center">
  <img src="../document/screen_ota_stepper.png" alt="Numeric Stepper GitHub Query" width="60%" />
</p>

---

## 🛠️ Key Architectural Features

1. **200px / 420px Split Carousel**:
   - **Stationary Left Section (200px)**: Version badge (`MAJOR`, `MINOR`, `PATCH`), build date, binary size, real-time battery safety check, and single-tap `[Select & Flash]` action button.
   - **Dynamic Right Section (420px)**: Version changelog with smooth vertical auto-scroll (20 px/s), 2.0s bounceback delay at bottom/top, and a 5.0s user-touch hold override.
2. **Numeric Stepper GitHub Query**:
   - Browse arbitrary versions from GitHub using `v[▲▼].[▲▼].[▲▼]` with live HTTP HEAD compatibility checks.
3. **Approved Repositories Switcher**:
   - Switch between Official (`rahulraj80/TuneBar`) and Upstream (`VaAndCob/TuneBar`) repositories with automatic catalog re-fetching.
4. **Hardware Battery Guard**:
   - OTA flashing is automatically blocked below 70% battery capacity unless actively connected to USB power.
5. **App Teardown on OTA**:
   - Shuts down audio playback, network streams, and IMU SSE polling before flashing to guarantee maximum free RAM/PSRAM.
6. **Strict Semantic Versioning**:
   - Every build produces a strictly unique semantic version (`v<Major>.<Minor>.<Patch>`) enforced by `release.py`.

---

## 🚀 Git-First Build Pipeline & OTA Publishing Workflow

TuneBar decouples development compilation from OTA deployment to prevent Git repository bloat:

1. **Stage 1: Build & Local Test (`release.py` / `sync_and_build.py`)**:
   - Updates `version.h`, creates an annotated Git tag (`vX.Y.Z`), and pushes code/tags to GitHub (`origin main --tags`).
   - The remote build server (`auto`) fetches the exact pushed commit from GitHub and compiles via PlatformIO.
   - The binary artifact (`firmware.bin`) is downloaded directly to local `build_phase1/firmware.bin` via SCP in 1 second.
   - Firmware is flashed to the physical ESP32 and tested. No `.bin` files enter the Git history at this stage.

2. **Stage 2: Official OTA Promotion (`scripts/publish_ota.py`)**:
   - After on-device validation, run:
     ```bash
     python scripts/publish_ota.py [vX.Y.Z]
     ```
   - **Repository Hygiene Guard (Max 2 Releases)**: Automatically retains only the **latest 2 official binaries** in `releases/bin/` (~8.4 MB total), deleting and untracking older `.bin` files to prevent Git history bloat.
   - Updates `releases/firmware_catalog.json` and compiles `sketch/include/generated_catalog.h`.
   - Pushes the official release to GitHub where it becomes immediately accessible to the on-device OTA manager.
