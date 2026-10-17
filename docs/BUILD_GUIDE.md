# TuneBar: Environment Setup & Compilation Guide

This guide provides complete instructions to set up the build environment, compile the firmware, build/upload the LittleFS asset filesystem, and flash the device on **Windows**, **Linux**, and **macOS**.

---

## 1. Target Hardware Specifications

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

## 2. Windows Complete Step-by-Step Setup Guide

This section is self-contained for users building and flashing directly on **Windows 10 / 11** using PowerShell or Command Prompt.

### Step 2.1: Install Python & Git on Windows
Open **PowerShell as Administrator** or standard PowerShell:
```powershell
# Install Python 3.11 and Git via Windows Package Manager
winget install Python.Python.3.11 Git.Git
```
*(Make sure to check "Add Python to PATH" if installing manually from python.org).*

Enable script execution in PowerShell:
```powershell
Set-ExecutionPolicy -Scope CurrentUser -ExecutionPolicy RemoteSigned
```

### Step 2.2: Install PlatformIO and Tools
In PowerShell:
```powershell
python -m pip install --upgrade pip
pip install --upgrade platformio esptool pyserial
```

Verify that PlatformIO is available:
```powershell
pio --version
```

### Step 2.3: Clone the Repository
```powershell
git clone https://github.com/rahulraj80/TuneBar.git
cd TuneBar
```

### Step 2.4: Identify your Device COM Port
Connect the TuneBar board via USB-C to your PC. Find the assigned COM port in PowerShell:
```powershell
[System.IO.Ports.SerialPort]::getportnames()
```
or inspect connected serial devices:
```powershell
Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Description
```
*(Example: `COM11`)*

### Step 2.5: Compile the Firmware on Windows
```powershell
pio run -d sketch
```
Output files generated:
- Application Binary: `sketch\.pio\build\esp32-s3-devkitc1-n16r8\firmware.bin`
- Bootloader: `sketch\.pio\build\esp32-s3-devkitc1-n16r8\bootloader.bin`
- Partitions: `sketch\.pio\build\esp32-s3-devkitc1-n16r8\partitions.bin`

### Step 2.6: Build LittleFS Asset Filesystem Image
```powershell
pio run -d sketch -t buildfs
```
Output LittleFS image:
- Filesystem Binary: `sketch\.pio\build\esp32-s3-devkitc1-n16r8\littlefs.bin`

### Step 2.7: Flash to Device on Windows

#### Option A: Automated via PlatformIO
```powershell
# 1. Upload Application Firmware
pio run -d sketch -t upload

# 2. Upload LittleFS Filesystem (Icons, Audio, WiFi, Stations)
pio run -d sketch -t uploadfs
```

#### Option B: Standalone Flashing via `esptool.py` (e.g. on `COM11`)
```powershell
# Flash Application Firmware Only (0x10000)
python -m esptool --chip esp32s3 -p COM11 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m 0x10000 sketch\.pio\build\esp32-s3-devkitc1-n16r8\firmware.bin
```

```powershell
# Full Initial Flash (Bootloader, Partition Table, App Firmware, and LittleFS)
python -m esptool --chip esp32s3 -p COM11 -b 460800 --before default_reset --after hard_reset write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m `
  0x0000   sketch\.pio\build\esp32-s3-devkitc1-n16r8\bootloader.bin `
  0x8000   sketch\.pio\build\esp32-s3-devkitc1-n16r8\partitions.bin `
  0x10000  sketch\.pio\build\esp32-s3-devkitc1-n16r8\firmware.bin `
  0x710000 sketch\.pio\build\esp32-s3-devkitc1-n16r8\littlefs.bin
```

### Step 2.8: Serial Debug Monitor on Windows
```powershell
# Via PlatformIO Monitor
pio device monitor -d sketch -b 115200

# Or via the included Python monitor script
python monitor_serial.py
```

---

## 3. Linux & macOS Setup Guide

### Step 3.1: Install Dependencies
```bash
# Ubuntu / Debian
sudo apt update && sudo apt install -y python3 python3-pip python3-venv git
sudo usermod -a -G dialout $USER

# Install udev rules for USB flashing (Linux)
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger

# macOS
brew install python git
```

### Step 3.2: Install PlatformIO
```bash
pip3 install --upgrade platformio esptool pyserial
```

### Step 3.3: Clone, Build, and Flash
```bash
# Clone
git clone https://github.com/rahulraj80/TuneBar.git
cd TuneBar

# Compile firmware
pio run -d sketch

# Build LittleFS data partition
pio run -d sketch -t buildfs

# Flash firmware & filesystem
pio run -d sketch -t upload
pio run -d sketch -t uploadfs
```

---

## 4. PlatformIO Configuration Details (`sketch/platformio.ini`)

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

## 5. Troubleshooting Matrix

| Symptom | Probable Cause | Fix / Resolution |
| :--- | :--- | :--- |
| `Stack canary watchpoint triggered (buttonInputTask)` | FreeRTOS task stack exhaustion | `buttonInputTask` stack depth must be $\ge$ `4 * 1024` bytes with static payload structures. |
| Screen remains completely black after boot | Backlight boost regulator disabled | Ensure `bsp_set_backlight_power(true)` is invoked and `EXIO1_BIT` is mapped to `0b00000010`. |
| Device shuts down immediately upon power button release | PMIC power latch (`SYS_EN`) not asserted | `TCA9554` must write `1` to `EXIO6_BIT` during `setup()` to keep the system powered on. |
| UI icons or sound alerts missing | LittleFS filesystem partition not flashed | Run `pio run -d sketch -t uploadfs` (or flash `littlefs.bin` to `0x710000`). |
| Windows says COM port in use / access denied | Another process (e.g. monitor/terminal) is open | Close all active serial terminal windows or python monitor scripts before flashing. |
| ESP32-S3 not recognized on Windows | Missing USB CDC / JTAG driver | Use Device Manager; if showing yellow exclamation, install Espressif USB JTAG/serial drivers. |
