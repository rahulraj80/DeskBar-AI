# TuneBar: Environment Setup & Compilation Guide

This guide provides complete instructions to set up the build environment, compile the firmware, build/upload the LittleFS asset filesystem, and flash the device on any Linux, macOS, or Windows machine.

---

## 1. Hardware Target Specifications

- **Board:** Waveshare ESP32-S3-Touch-LCD-3.49 (Rev 1.1)
- **MCU:** ESP32-S3 (Dual-core Xtensa® 32-bit LX7 @ 240 MHz)
- **Memory:** 16 MB Flash (Quad/DIO, 80 MHz), 8 MB Octal PSRAM (OPI)
- **Display:** 3.49″ IPS Capacitive Touch LCD (172 × 640, QSPI ST77916)
- **Audio:** ES8311 Codec (I2S TX) + ES7210 Mic (I2S RX) + NS4150 Power Amp (TCA9554 EXIO7)
- **IO Expander:** TCA9554PWR (I2C address `0x20` / `0x38`)
  - `EXIO1`: Backlight Boost Regulator Enable (`AP3032`)
  - `EXIO5`: LCD Hardware Reset
  - `EXIO6`: System Power Hold Latch (`SYS_EN`)
  - `EXIO7`: Audio Power Amp Enable (`PA_EN`)
- **Backlight PWM:** GPIO 42 (LEDC Timer 0, 5 kHz)

---

## 2. Prerequisites & Toolchain Setup

### A. Python 3.9+ & Git
Ensure Python 3 and Git are installed:
```bash
# Ubuntu / Debian
sudo apt update && sudo apt install -y python3 python3-pip python3-venv git

# macOS (Homebrew)
brew install python git

# Windows (PowerShell with Winget)
winget install Python.Python.3.11 Git.Git
```

### B. PlatformIO Core (CLI)
Install PlatformIO using `pip` or the standalone installer:
```bash
pip install --upgrade platformio
```
Verify the installation:
```bash
pio --version
```
*(Optionally, you can use Visual Studio Code with the **PlatformIO IDE** extension).*

### C. USB Serial Permissions (Linux only)
If building on Linux and flashing via USB:
```bash
sudo usermod -a -G dialout $USER
# Add udev rules for Espressif chips:
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

---

## 3. Clone the Repository

```bash
git clone https://github.com/rahulraj80/TuneBar.git
cd TuneBar
```

Project Directory Layout:
```
TuneBar/
├── BUILD_GUIDE.md                       # This setup & build guide
├── README.md                            # Project overview
├── build_phase1/                        # Pre-compiled flashable binaries
│   └── firmware.bin
└── sketch/                              # PlatformIO project root
    ├── platformio.ini                   # Build configuration & dependencies
    ├── partitions/
    │   └── 7MB_app_ota_2MB_littlefs.csv # Custom 16MB partition table
    ├── data/                            # LittleFS asset files (UI icons, audio, config)
    │   ├── audio/                       # Notification & effect MP3s
    │   ├── img/                         # UI PNG/BIN icons
    │   ├── stations.csv                 # Default radio stations
    │   └── wifi.json                    # Saved WiFi credentials
    ├── include/                         # Header files (xtask.h, user_config.h, etc.)
    └── src/                             # Source code (main.cpp, lvgl_port, ui, drivers)
```

---

## 4. PlatformIO Configuration (`sketch/platformio.ini`)

The project uses the `pioarduino` platform fork which provides Arduino ESP32 Core v3+ with full ESP32-S3 Octal PSRAM & USB CDC support:

```ini
[env:esp32-s3-devkitc1-n16r8]
platform = https://github.com/pioarduino/platform-espressif32/releases/download//55.03.37/platform-espressif32.zip
board = esp32-s3-devkitc1-n16r8
framework = arduino
board_build.filesystem = littlefs
board_build.partitions = partitions/7MB_app_ota_2MB_littlefs.csv
lib_deps = 
	lvgl/lvgl@^8.3.11
	bblanchon/ArduinoJson@^7.4.2
build_flags = 
	-DCONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
	-DBOARD_HAS_PSRAM
	-D CORE_DEBUG_LEVEL=3
	-D ARDUINO_USB_CDC_ON_BOOT=1
	-D ARDUINO_USB_MODE=1
	-D LV_CONF_INCLUDE_SIMPLE
	-D LV_LVGL_H_INCLUDE_SIMPLE
	-I src
	-I src/ui
	-I include
monitor_speed = 115200
upload_speed = 921600
build_type = debug
monitor_filters = 
	esp32_exception_decoder
	default
```

---

## 5. Compilation & Building

All PlatformIO commands should target the `sketch` subfolder (`-d sketch`):

### A. Compile Firmware
```bash
pio run -d sketch
```
Upon completion, the compiled binaries are located at:
- Firmware: `sketch/.pio/build/esp32-s3-devkitc1-n16r8/firmware.bin`
- ELF Debug File: `sketch/.pio/build/esp32-s3-devkitc1-n16r8/firmware.elf`
- Bootloader: `sketch/.pio/build/esp32-s3-devkitc1-n16r8/bootloader.bin`
- Partition Table: `sketch/.pio/build/esp32-s3-devkitc1-n16r8/partitions.bin`

### B. Build LittleFS Filesystem Image (Data Partition)
The UI assets, audio alerts, and station lists in `sketch/data/` must be packed into a LittleFS binary:
```bash
pio run -d sketch -t buildfs
```
Generated filesystem binary:
- `sketch/.pio/build/esp32-s3-devkitc1-n16r8/littlefs.bin`

---

## 6. Flashing to Device

Connect the device via USB-C to your computer.

### Method 1: Using PlatformIO (Automated)

1. **Flash Firmware:**
   ```bash
   pio run -d sketch -t upload
   ```
2. **Flash LittleFS Data (Required on initial setup or asset changes):**
   ```bash
   pio run -d sketch -t uploadfs
   ```

### Method 2: Standalone `esptool.py` (No PlatformIO needed on target PC)

If flashing on a machine without PlatformIO, install `esptool`:
```bash
pip install esptool
```

1. **Flash Application Firmware (`0x10000`):**
   ```bash
   # Windows (e.g. COM11)
   python -m esptool --chip esp32s3 -p COM11 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x10000 sketch/.pio/build/esp32-s3-devkitc1-n16r8/firmware.bin

   # Linux / macOS (e.g. /dev/ttyACM0 or /dev/cu.usbmodem*)
   python3 -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x10000 sketch/.pio/build/esp32-s3-devkitc1-n16r8/firmware.bin
   ```

2. **Full Flash (Bootloader, Partition Table, App, and LittleFS):**
   ```bash
   python -m esptool --chip esp32s3 -p COM11 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
     0x0000   sketch/.pio/build/esp32-s3-devkitc1-n16r8/bootloader.bin \
     0x8000   sketch/.pio/build/esp32-s3-devkitc1-n16r8/partitions.bin \
     0x10000  sketch/.pio/build/esp32-s3-devkitc1-n16r8/firmware.bin \
     0x710000 sketch/.pio/build/esp32-s3-devkitc1-n16r8/littlefs.bin
   ```

---

## 7. Serial Monitoring & Live Debugging

TuneBar includes a high-frequency serial debug beacon reporting FreeRTOS heap, PSRAM, screen lock state, audio status, and WiFi state every 2 seconds.

### View Serial Log via PlatformIO:
```bash
pio device monitor -d sketch -b 115200
```

### View Serial Log via Python:
```bash
python monitor_serial.py
```

Expected beacon output format:
```text
[BEACON] Heap: 6710840 (min: 6693840) | PSRAM: 6678636 | BL_OFF: 0 | Audio: PLAYING | WiFi: CONNECTED
```

---

## 8. Common Build & Runtime Troubleshooting

| Issue | Cause | Solution |
| :--- | :--- | :--- |
| `Stack canary watchpoint triggered (buttonInputTask)` | FreeRTOS task stack exhaustion | Ensure `buttonInputTask` stack depth is at least `4 * 1024` bytes and `UIStatusPayload` messages are allocated statically. |
| Screen remains black after flashing | Backlight boost power rail (`EXIO1`) not enabled | Confirm `bsp_set_backlight_power(true)` is called in `setup()` and `EXIO1_BIT` is mapped to `0b00000010`. |
| Device shuts off immediately on boot | `SYS_EN` (`EXIO6`) power latch not asserted | `TCA9554` must write `1` to `EXIO6_BIT` during `setup()` to hold the PMIC on. |
| Missing UI Icons / Sound Effects | LittleFS partition not flashed | Run `pio run -d sketch -t uploadfs` to flash all assets in `sketch/data`. |
| `mbedtls` Out of Memory | TLS buffers consuming internal SRAM | Keep `-DCONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` enabled in `platformio.ini`. |
