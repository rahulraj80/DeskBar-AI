# DeskBar-AI Tiered Guide

Five time-budgeted entry points for understanding DeskBar-AI, Rahul's fork of
[TuneBar](https://github.com/rahulraj80/DeskBar-AI) — an ESP32-S3 media bar
firmware with Nixie clock mode, Wi-Fi radio streaming, and ECO power-saving
features.

---

## 30-Second Pitch

DeskBar-AI is an ESP32-S3 media bar firmware: Nixie clock, Wi-Fi radio
streaming, torch, and ECO power-saving modes. It runs on the Waveshare
3.49" S3 Touch LCD and is managed by the WaveShare 3.49 Launcher, which can
boot DeskBar or Bruce firmware. Key power optimisation: compact `hh:mm`
clock layout during deep-idle breathing, CPU 240→80 MHz scaling, and Wi-Fi
modem sleep when not streaming.

---

## 2-Minute Presentation

DeskBar-AI (Rahul's fork of TuneBar) runs on ESP32-S3 with 8 MB PSRAM,
driving a 640×172 Waveshare 3.49" touch display. It provides:

- **Nixie clock** with breathing backlight and compact `hh:mm` in deep idle
- **Wi-Fi radio** streaming and web remote
- **Torch** (backlight at 100%)
- **ECO power saving**: CPU 240→80 MHz, Wi-Fi PS_MIN_MODEM when idle,
  display buffer invalidation reduced from 3×/second to 1×/minute

Measured: ~121 mA in clock mode (~0.8 Ah cell → ~6.8 hours). A genuine
18650 (~2.0 Ah) would run ~16.5 hours.

---

## 5-Minute Introduction

### Hardware
- ESP32-S3 (QFN56, 240 MHz, 8 MB PSRAM)
- Waveshare 3.49" S3 Touch LCD (640×172, AXS15231B QSPI display)
- ES8311 + ES7210 audio codec, TCA9554 I/O expander, PCF85063 RTC
- 18650 Li-ion battery (~0.8 Ah measured, not 2.0 Ah as labelled)

### Software structure
```
sketch/           — PlatformIO firmware (lvgl, Arduino)
  src/            — C++ source: file.cpp, task_msg/, ui_events.cpp, power/
  include/        — xtask.h (Wi-Fi PS logic), secrets_manager.h
  partitions/     — partition CSVs (spiffs/littlefs data partition)
  data/           — LittleFS content (audio/, stations.csv)
  platformio.ini  — build config (esp32-s3-devkitc1-n16r8 env)
backend/          — PHP telemetry + calibration service
releases/         — published firmware binaries + catalogue
```

### Key power paths
- **LittleFS label must be `spiffs`** for non-launcher builds (Wi-Fi
  credentials stored in `/wifi.json` on LittleFS)
- **Wi-Fi PS_NONE only during streaming** (`ai_recording_active`, not
  `mediaType==0`) — fixes ~55 mA idle penalty
- **Compact clock layout** hides seconds during deep-idle breathing,
  eliminating per-second full-frame invalidations

### Wi-Fi settings panel must mirror live state
The Settings → Wi-Fi panel used to show whatever `wifi_connect_task()`
last wrote. That task exits as soon as it connects, so a connected device
could keep displaying a stale `No network matched.` forever.
`uiRefreshWifiPanel()` (`src/task_msg/task_msg.cpp`) now renders the panel
from `WiFi.status()` / `WiFi.SSID()` / `WiFi.localIP()` and runs from
`lvgl_wifi_check_cb()` every 5 s plus on `wifi settings`. `wifi settings`
now switches to the menu screen and opens the Wi-Fi tab (it used to report
"screen opened" while the clock stayed visible). Use `wifi panel` to dump
the exact on-screen strings and compare with `status`.

Gotcha: never hold `.c_str()` from an Arduino `String` return value
(`WiFi.SSID().c_str()`) — the temporary dies at the end of the statement
and the pointer reads as an empty string. Assign the `String` to a local
first.

System info surface: Utility → System info now opens with header
`DeskBar-AI v<version> (Rahul Mod)` (renamed from `TuneBar v1.2.6`). The
placeholder label uses `DeskBar-AI (Rahul Mod)`.

### Weather & geo settings (configurable via /secrets.ini or CLI)

Weather and geo settings can be overridden at runtime via `/secrets.ini` on
LittleFS or through the serial CLI / Web Remote. No recompile required.

**Defaults** (baked into `sketch/include/secrets_example.h`):
- Geo enabled: `true` — resolve IP geolocation before weather lookup
- Geo provider: `ip-api.com` — `http://ip-api.com/json/`
- Weather provider: `open-meteo` — `https://api.open-meteo.com/v1/forecast`

**Providers** (all free, no API key required):
| Provider | Response size | Pressure | Notes |
|---|---|---|---|
| open-meteo | ~700 bytes | ❌ (not in `current`) | Default; small, reliable response |
| met.no | ~40 KB | ✅ (`air_pressure_at_sea_level`) | May have memory issues on ESP32-S3 |
| weatherapi.com | variable | ✅ | Requires API key; placeholder only |

**Serial CLI commands:**
```
weather geo on|off        # Enable/disable IP geolocation
weather geo ip-api.com    # Set IP geolocation provider
weather geo status        # Show current geo config
weather provider met.no   # Set weather provider
weather provider open-meteo
weather provider weatherapi.com
weather settings          # Show full weather/geo configuration
```

**Web Remote:** `GET/POST /api/secrets` includes `weather_geo_enabled`,
`weather_geo_provider`, `weather_geo_url`, `weather_provider` fields.

**Geo fallback:** When IP geolocation fails (e.g. `ip-api.com` unreachable),
falls back to hardcoded Patna coordinates (25.5943, 85.1352).

---

## 15-Minute Deep Dive

### Power model (measured, Nov 2026)

| Component | Current | Notes |
|---|---|---|
| Base system (BL off) | ~90 mA | MCU ~50, panel ~25, codecs ~10, SD ~5 |
| Backlight 100% (torch) | ~80 mA | Inverted PWM: `setUpduty(255-brightness)` |
| Backlight 40% (clock lit) | ~32 mA | `bl_start`/`bl_end` range 91-120 |
| Wi-Fi PS_NONE streaming | ~100 mA | Only during active audio streaming |
| Wi-Fi PS_MIN_MODEM idle | ~12 mA | When not streaming or recording |

Measured total: ~121 mA clock mode. Cell: ~0.8 Ah → 6.8 hours.

### ECO features (clock mode, below 80% battery)

1. **Compact `hh:mm` layout**: During deep-idle breathing (>30 min
   inactivity), seconds digits and second colon are hidden. Display buffer
   invalidation drops from 3×/second to 1×/minute (with `full_refresh=1`,
   each invalidation re-renders the entire 640×172 frame).

2. **CPU 240→80 MHz**: During `BREATH_FULL_BLACK` (panel dark), CPU drops
   to 80 MHz unless charging, radio active, or audio running. Restored to
   240 MHz on any interaction.

3. **Wi-Fi modem sleep**: `WIFI_PS_NONE` only when `audio.isRunning()` or
   `ai_recording_active`; otherwise `WIFI_PS_MIN_MODEM` with
   `WiFi.setSleep(true)`.

4. **Label guards**: Date and battery labels only update on string change;
   battery voltage quantised to 20 mV; battery label skipped during
   `FULL_BLACK`.

### Partition table

| Label | Type | Offset | Size |
|---|---|---|---|
| nvs | data/nvs | 0x9000 | 20 KB |
| otadata | data/ota | 0xE000 | 8 KB |
| app0 | app/factory | 0x10000 | 1536 KB (Launcher) |
| deskbar | app/ota_0 | 0x1A0000 | 7 MB |
| bruce | app/ota_1 | 0x8A0000 | 4544 KB |
| littlefs | data/littlefs | 0xD10000 | 1920 KB |

Launcher boots first; `bootToApp: false` shows the app selection menu.

---

## 1-Hour Tour

### Build & flash
```bash
# Build on a machine with PlatformIO
pio run -d sketch -e esp32-s3-devkitc1-n16r8

# Flash via esptool (merged bootloader + partitions + app)
esptool write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin

# Or flash individual app partition under Launcher
# (Launcher handles partition creation and app slot management)
```

### Remote build on `auto`
```bash
rsync -az sketch/ auto:~/build/deskbar/sketch/
ssh auto 'cd ~/build/deskbar/sketch && ~/.platformio/penv/bin/pio run -e esp32-s3-devkitc1-n16r8'
scp auto:~/build/deskbar/sketch/.pio/build/esp32-s3-devkitc1-n16r8/firmware.bin .
```

### Serial debugging
```bash
# Launcher serial console (safe commands)
nav Select          # enter Launcher menu
partitions          # view partition table
settings get        # view settings JSON
wifi list           # list saved networks
status              # battery, heap, Wi-Fi status

# DeskBar serial output (115200 baud on /dev/ttyACM0)
# Watch for: ESP_ERR_WIFI_SSID (no credentials), LittleFS mount errors

# DeskBar CLI WiFi management commands:
wifi status         # Show connection status, IP, SSID, RSSI
wifi scan           # Scan for available networks
wifi list           # List saved networks in /wifi.json
wifi add <SSID> <PWD>  # Add a saved WiFi network
wifi del <SSID>       # Delete a saved WiFi network
wifi settings         # Open Settings → WiFi panel via serial CLI
wifi panel            # Print exactly what the WiFi panel is showing
wifi <on|off>         # Enable or disable WiFi
```

### Key files to read first
1. `sketch/src/file/file.cpp` — LittleFS init, `LITTLEFS_LABEL`
2. `sketch/include/xtask.h` — Wi-Fi PS logic (~line 913)
3. `sketch/src/ui/ui_events.cpp` — breathing state machine, CPU scaling
4. `sketch/src/task_msg/task_msg.cpp` — clock layout, label guards
5. `sketch/partitions/7MB_app_ota_2MB_littlefs.csv` — partition table

### Common gotchas
- **Wi-Fi won't connect**: LittleFS label mismatch → `/wifi.json` unreadable
  → `ESP_ERR_WIFI_SSID`. Check partition label matches `LITTLEFS_LABEL`
  (`spiffs` for non-launcher builds). Fixed 2026-11-04:
  `file.cpp` non-launcher branch → `"spiffs"`,
  `partitions/7MB_app_ota_2MB_littlefs.csv` → label `"spiffs"`.
- **Weather shows wrong location**: `auto:ip` relies on weatherapi.com's
  built-in (often inaccurate) IP geolocation. Fixed 2026-11-04: added
  `ip-api.com/json/` geolocation step before weatherapi.com call
  (`src/weather/weather.cpp`, `src/weather/weather.h`,
  `src/ui/ui_events.cpp`). Coordinates cached per boot; reset on settings
  change.
- **Clock breathing saves nothing**: seconds digits still updating every
  second → enable compact mode (>30 min inactivity).
- **NVS erased**: Wi-Fi credentials lost → re-enter via UI or restore
  from full-chip backup.
- **Telemetry not uploading** (`connection refused`): The baked-in default
  endpoint used to be the placeholder `https://api.your-domain.com/myapi.php`
  (meant to be overridden by `/secrets.ini` on LittleFS). With no
  `/secrets.ini` present, flushes failed with
  `start_ssl_client: connect failed: -1` / `connection refused`.
  The default is now `https://autobots.my.to/myapi.php` (the real telemetry
  API accepting batch JSON POST, returning `{"status":"ok"}`). Users can still
  override via their own `/secrets.ini` (`[telemetry] endpoint=...`).
  `telemetry_flush_batch()` now proceeds whenever Wi-Fi is actually
  connected, even if the `wifiEnable` setting is false. Verify with
  `telemetry` (buffered count) and `telemetry flush` from the serial CLI.
- **Device resets on serial port open**: pyserial defaults to DTR=True which
  triggers the ESP32-S3 `rst:0x15 USB_UART_CHIP_RESET` on /dev/ttyACM0.
  Open the port with `DTR=False` (see `tools/deskbar_serial.py`) before
  sending CLI commands.
