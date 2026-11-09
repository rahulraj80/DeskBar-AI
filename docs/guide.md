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

**Credential storage:** the saved-network list is written to **both**
`/wifi.json` on LittleFS and a Preferences string in NVS (`wifi`/`list`), and
is re-read from NVS if the file is missing or empty — a filesystem reformat no
longer throws away a password typed on the device. Every **successful**
association also re-saves the exact SSID/password pair that authenticated, so a
credential that only lived in RAM (or came from a built-in default) is
persisted the moment it works. Passwords are never baked into `secrets.h`.
Each save logs `[WIFI] saved N entries fs=1 nvs=1 len=N` so a failed NVS mirror
is visible in the serial log rather than silent.

**Failure behaviour (loud, never silent):**

- The fetch **waits up to 60 s for the Wi-Fi link**, re-nudging
  `wifiConnect()` every second, before its first request. The radio is parked
  in clock mode, so opening the panel right after an idle period used to race
  the re-association and fail on the first GET.
- **Air quality** comes from open-meteo's separate `air-quality-api` endpoint
  (same provider, no key, ~600 B): PM2.5/PM10/CO/NO2/SO2/O3 + `us_aqi`. The
  panel used to show a `0001` placeholder because AQI was computed from the
  weather payload's zeroed pollutants; that path is gone.
- **Chunked responses**: open-meteo answers with
  `Transfer-Encoding: chunked`, and `fetchUrlData()` reads the raw stream, so
  every body must go through `sanitizeJson()` (trim to the first `{` … last
  `}`) before `deserializeJson()`. Forgetting that on the air-quality path made
  every parse fail with a valid-looking payload.
- **Air quality is cached for 60 minutes** — it only changes hourly, while the
  page can refresh every minute, so this halves the TLS round trips.
- **met.no is refused up front** (`weather provider met.no` prints the reason
  and does nothing): its compact product is ~39 KB against a 2 KB fetch buffer.
- **Provider retry/fallback**: open-meteo is tried twice; if both fail the
  firmware falls back to **wttr.in's compact text form**
  (`?format=%t|%f|%h|%P|%w|%C`, ~45 B — `%P` is pressure, `%p` would be precipitation). met.no is *not* a usable fallback —
  its compact JSON is ~39 KB and `fetchUrlData()` stops at the 2 KB buffer.
- **Rotation is ignored while the panel is black** (`BL_OFF` or the breathing
  dark phase): the orientation is tracked silently so the picture is correct
  when it lights up, but a rotation never wakes the device or restarts the
  cycle. The sensor is also held in a 1.5 s **settle window** right after
  `imu_sensor_wake()`, because the first samples out of sleep can read a
  different orientation than the one the device went to sleep with — that was
  what paused breathing ~0.2 s into the first fade-in of every boot. A 3 s
  settle window right after `imu_sensor_init()` does the same for the boot
  baseline. Measured after the change: six consecutive breathing cycles over
  ~70 s with **zero** pauses. The wttr.in fallback was exercised end to end with a temporary
  switch (45 bytes parsed into temp/feels/humidity/pressure/wind, AQI left
  grey) and the switch has been removed again.
- **Missing data is grey, never faked**: every field carries a presence flag;
  absent values render as `--` in `#8a8a8a` (label recolor), the AQI card
  background goes grey instead of a green "Good" colour, the weather wallpaper
  drops to a neutral dark gradient and the sky/home icons are hidden — a stale
  green gradient behind `--` read as fresh data otherwise.
- The 15-minute weather refresh only runs **while the weather page is on
  screen** (`infoPageIndex == 1`). It used to fire on the clock face too, which
  woke the parked radio every quarter hour, failed after the connect timeout
  and left the link up — quietly defeating the Wi-Fi park and the 80 MHz tier.
- Geo resolution is attempted **twice**. If both attempts fail, a provider that
  accepts a place query (weatherapi.com, `q=auto:ip`) is left to resolve the IP
  itself; `open-meteo`/`met.no` need real coordinates, so the fetch **aborts**
  and the panel shows `Location unresolved` instead of silently substituting a
  hardcoded coordinate. There is no built-in default location.
- The geolocation response body is logged when it lacks `lat`/`lon`, so a
  provider change or an HTTP error is visible in the serial log.
  Note that ip-api returns `lat`/`lon` as JSON **numbers** — read them with
  `doc["lat"].as<float>()`; assigning to `const char *` silently yields `""`
  for every successful response.
- `wifi scan` brings the interface up (`WiFi.mode(WIFI_STA)`) first and prints
  `Scan failed (-2: WIFI_SCAN_FAILED)` instead of `Found -2 networks`.

**Wi-Fi parking and uptime:** the radio is never parked during the first two
minutes of uptime (`WIFI_PARK_MIN_UPTIME_MS`), regardless of the idle counter —
that window belongs to the boot association and the NTP sync.

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

**Geo fallback:** there is **no** built-in default location any more (the old
hardcoded `25.5943,85.1352` Patna fallback was removed): if IP geolocation fails
the weather fetch aborts and the panel shows `Location unresolved`.

### AI Voice Assistant endpoint (configurable via /secrets.ini, Web Remote or CLI)

The AI voice assistant has **two** request paths and both must use the same
configured endpoint:

| Path | Trigger | Code |
|---|---|---|
| Voice upload | \[MIC\] button / `mic` | `ai_upload_task()` in `sketch/include/xtask.h` — raw `WiFiClientSecure` POST of a 16 kHz WAV, expects a JSON reply |
| Text query | `ask <text>` / Web Remote `/api/ask` | `main.cpp` / `web_remote.cpp` → `audioPlayHOST(<url>?q=...)`, streams MP3 |

**Endpoint resolution (v2.6.6+):** `secrets_get_ai_url()` is the single source
of truth. `ai_upload_task()` parses it into host / port / path with
`resolve_ai_endpoint()`; before that it connected to the compile-time
`AI_ASSISTANT_HOST` only, so a `/secrets.ini` override silently did nothing for
voice uploads (they kept POSTing to the placeholder host). The upload path is
**https only**; a plain `http://` endpoint is refused with
`AI endpoint not configured. Set it in Settings.`

**Default** (baked into `sketch/include/secrets_example.h`):
`https://autobots.my.to/ask.php` — the complimentary community endpoint. It
whitelists device IDs (`/var/www/data/registered_devices.json`) and answers
`{"status":"restricted", …}` for anything else, which the device shows as
*"AI Assistant Restricted"*.

**Expected reply** (HTTP 200, `application/json`):
`{"status":"ok","question":"…","answer":"…","audio_url":"https://…/ai_cache/<md5>.mp3"}`.
If `audio_url` is present the device streams it and plays it through ES8311.

**Failure messages, and what they mean:**

| On-screen text | Cause |
|---|---|
| `WiFi offline` | radio parked/not associated when the upload task started |
| `Cannot connect to server` | TLS connect to the resolved host failed |
| `Server sent a web page, not an AI reply` | HTTP 200 whose body has no JSON `"status"` key — classic parked-domain / proxy-captive-portal page |
| `Bad server response` | body looked like JSON but `deserializeJson()` failed |
| `AI Assistant Restricted: …` | server refused this device ID |
| `AI Assistant Error (HTTP nnn)` | server answered with a non-200 |
| `AI endpoint not configured` | `ai_url` missing, scheme-less, or plain `http` |

**Setting it:**
```
# serial CLI (persists to /secrets.ini, no reboot)
mic                     # record → uploads → speak the answer

# web remote
curl -X POST http://<device-ip>/api/secrets -d "ai_url=https://your-server/ask.php"
curl http://<device-ip>/api/secrets      # verify
```
`secrets_write_ini_file()` rewrites `/secrets.ini` with the `[ai]`, `[telemetry]`,
`[security]` and `[weather]` sections, so always send the values you want kept
(omitted form fields fall back to the current runtime value in the handler).

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

2. **CPU 240→80 MHz**: While clock breathing is active the CPU is held at
   80 MHz for the **whole cycle**, bright phase included — the digits are
   static imagery once drawn. It returns to 240 MHz for audio playback, AI
   recording, any user interaction (`record_user_interaction()`), **and
   whenever the radio is up**.

   The radio is a precondition, not an afterthought: running Wi-Fi
   association, DHCP, NTP or an HTTP fetch on the parked CPU starves the
   ESP32-S3 Wi-Fi stack and produces `4WAY_HANDSHAKE_TIMEOUT` failures. The
   policy is therefore `!audio && !ai_recording && WiFi.getMode() == WIFI_OFF`
   — 240 MHz while Wi-Fi is in STA, 80 MHz only once Wi-Fi is genuinely
   parked (NTP synced + 2 minutes idle in clock mode). A unit that cannot
   reach its AP stays at 240 MHz rather than dropping and thrashing the link.

   The charger term is not part of it: `bsp_is_battery_charging()` returns
   true for `BATT_CHARGE_FULL` as well as `BATT_CHARGE_CHARGING`, so a fully
   charged pack that is simply sitting off the charger counts as "charging".
   `lvgl_wifi_check_cb()` used to keep the radio up whenever that returned
   true, so a full pack (off the charger) never parked; the keep-alive now
   tests `battery_charge_state() == BATT_CHARGE_CHARGING` instead, so only a
   genuinely rising pack holds Wi-Fi open.

3. **Wi-Fi modem sleep**: `WIFI_PS_NONE` only when `audio.isRunning()` or
   `ai_recording_active`; otherwise `WIFI_PS_MIN_MODEM` with
   `WiFi.setSleep(true)`.

   **Wi-Fi parking** (`lvgl_wifi_check_cb`, every 5 s): the radio is turned
   off (`WiFi.disconnect(true)` + `WiFi.mode(WIFI_OFF)`) only when all of
   these hold — the clock face is breathing (`clock_face_active`), NTP has had
   its chance (either `g_boot_ntp_synced` is set, or the device has been up for
   2 minutes — the flag is only set by `wifi_connect_task`, and the link can
   also come up through direct `WiFi.begin()` paths), `gate_wifi` is enabled in
   the power profile, and the device has been idle for 2 minutes. It is *not*
   parked while the pack is genuinely charging
   (`battery_charge_state() == BATT_CHARGE_CHARGING`), which is what keeps
   the web remote and OTA reachable on USB power. Any user interaction wakes
   it again via `record_user_interaction()`. Parking is also the precondition
   for the 80 MHz CPU tier, so this gate is what the breathing power saving
   depends on.

   Measured on hardware: park at 132.3 s (2 min idle) → `CPU -> 80 MHz` in the
   same tick; a touch restores `CPU -> 240 MHz` immediately and the link
   re-associates ~5 s later.

4. **Label guards**: Date and battery labels only update on string change;
   battery voltage quantised to 20 mV; battery label skipped during
   `FULL_BLACK`.

5. **Touchscreen polling disabled during `BREATH_FULL_BLACK`**: The touch
   controller (`AXS15231B` over I2C) is polled every LVGL tick (~30ms) even
   while the panel is dark. `BL_OFF` is set to `true` during full dark, which
   makes the touch callback return early without I2C reads. Touch polling is
   restored during `BREATH_FADE_IN` before the backlight fades back up.

   **Important**: The AP3032 backlight boost rail is NO LONGER cut during full
   dark. Previously `bsp_set_backlight_power(false)` cut the rail via I2C
   (TCA9554 expander), but once cut it could not be re-enabled, leaving the
   screen permanently dark. The boost rail now stays on during `BREATH_FULL_BLACK`
   and only PWM duty is set to `bl_black` (dark via inverted PWM:
   `setUpduty(0)` = full on, `setUpduty(255)` = full off).

   No tap-to-wake during dark — BOOT button wake only. This saves power during
   the multi-second dark phase of every breathing cycle.

   **Why the LVGL task must keep ticking**: `BL_OFF` also gates
   `lv_timer_handler()` in `lvgl_port.cpp`. Stopping the scheduler on `BL_OFF`
   during breathing dark also stops the breath timer that clears `BL_OFF`, so
   the clock parks in `FULL_BLACK` forever. The gate is therefore
   `if (BL_OFF && !clock_face_active)`: the scheduler keeps running during
   breathing, while the flush callback and the touch read still return early
   on `BL_OFF` — no QSPI traffic, no touch I2C polling.

   **Repaint on wake**: while `BL_OFF` is set, the flush callback answers
   `lv_disp_flush_ready()` and drops the pixels, so the panel keeps the frame
   from before the dark phase. When the dark phase ends the active screen is
   marked dirty (`lv_obj_invalidate(lv_scr_act())`) so the first visible frame
   of the fade-in shows the current time rather than the stale timestamp.

   **Safety net**: If the display is stuck in `BREATH_FULL_BLACK` for more than
   30 seconds, it forces a wake to `BREATH_HOLD_HIGH` (steady brightness).

6. **Clock breathing initialization**: `init_main_menu_task()` (which loads
   the info screen, calls `enter_clock_breathing()`, and creates the
   `breath_timer` LVGL timer running `update_clock_breathing()` every 30ms)
   is called directly from `setup()` after `lvgl_port_init()`. The original
   path via `appStart()` → boot screen `LV_EVENT_SCREEN_LOADED` did not
   reliably fire during `ui_init()`, leaving `breath_timer` uncreated and the
   clock permanently at steady brightness. `enter_clock_breathing()` is
   called from `updateInfoPanel(0)` when the clock page is shown, and is
   idempotent (guarded by `clock_face_active`).

   **Verification** (laptop camera, /dev/video0, continuous host-timestamped
   frames — 3 s stills alias a 15 s cycle and camera auto-exposure flattens a
   dim cycle):

   - measured cycle at the reference profile (`bl_start` 91, `bl_end` 96,
     ramp 1250 ms, hold 2500 ms, dark 5000 ms): **5.00 s bright / 10.10 s dark**
     when the 30-minute inactivity tier doubles the dark hold, 5 s dark otherwise
   - `BL_OFF` is 1 for exactly the dark phase and 0 for the bright phase while
     the state machine keeps advancing
   - the first frame of the fade-in carries the current time (e.g. last frame
     before dark 20:23:18, first frame after 20:23:24)
   - serial state trace: `FADE_OUT → FULL_BLACK (5 s) → FADE_IN → HOLD_HIGH`,
     repeating

   Note: opening `/dev/ttyACM0` can reset the ESP32-S3 (USB-CDC DTR), so a
   measurement run should either hold one connection open or use the camera
   only.

**Setup-time backlight**: `setup()` calls `bsp_set_backlight_power(true)` and
`setUpduty(0)` at end to ensure the AP3032 boost rail is on and PWM duty is
full brightness (inverted PWM: duty 0 = LED on, duty 255 = LED off) before
`init_main_menu_task()` starts breathing.

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

### Launcher autoboot to DeskBar (v2.6.4+)

The WaveShare Launcher's second-stage bootloader chooses the boot partition by
reset reason: power-on or deep-sleep wake starts the Launcher (`app0`,
factory), a soft reset starts the selected OTA app (`deskbar`, ota_0). A
`boot_app` NVS override (partition label) is honoured by
`launcherBootCurrentApp()`. Final device state: `bootToApp=1`,
`bootTimer=15` (~16 s window), `boot_app="deskbar"`, `dimtime=60`. A CLI
`reboot` lands straight in DeskBar (~0.77 s from ROM); the hardware power
button lands in the Launcher menu.

**OTA releases** are published to `releases/bin/` and tracked in
`releases/firmware_catalog.json`. Max 2 versions are retained in git
(to prevent Git bloat); older binaries are pruned on each publish. The
catalog URL baked into the firmware (`sketch/src/ui/screens/ui_Screen_OTA.cpp`)
is `https://raw.githubusercontent.com/rahulraj80/DeskBar-AI/main/releases/firmware_catalog.json`;
until v2.6.7 it named the pre-rename slug `rahulraj80/TuneBar` and only
resolved through GitHub's rename redirect (whose `cache-control: max-age=300`
is why a device can show the previous catalog for minutes after a publish).
The same slug was still baked into the gzipped web page
(`sketch/web/index.html` → `tools/gzip_web_page.py` → `sketch/include/web_page_gz.h`,
so `strings` on the binary will not show it) until v2.6.8 — check that file
too when touching repo URLs, and verify on a live device with:
```bash
curl -s --compressed http://<device-ip>/ | grep -o "rahulraj80/[A-Za-z-]*" | sort | uniq -c
curl -s http://<device-ip>/api/ota/repos | grep -o "raw.githubusercontent.com/rahulraj80/[A-Za-z-]*"
```

---

## 1-Hour Tour

### Build & flash
```bash
# Build on a machine with PlatformIO
pio run -d sketch -e esp32-s3-devkitc1-n16r8

# Git-first remote build (resets auto's checkout to origin/main, builds both
# envs, downloads build_phase1/firmware.bin) — the normal release build
python3 scripts/sync_and_build.py --no-push
```

**Flashing this device (Waveshare 3.49 V2 under the Launcher):** the partition
table is the Launcher's — `app0` (Launcher) at `0x10000`, `deskbar` (`ota_0`)
at `0x1A0000`, `bruce` (`ota_1`) at `0x8A0000`. **Flash the app image only,
into the `deskbar` slot**; writing the app at `0x10000` overwrites the
Launcher:
```bash
esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 write-flash -z \
        --flash-mode dio --flash-freq 80m --flash-size detect \
        0x1A0000 build_phase1/firmware.bin
```
Never flash `bootloader.bin` / `partitions.bin` / `boot_app0.bin` on this
unit: that replaces the Launcher's table (and unless the `littlefs` data
partition survives, WiFi credentials are lost).

**Releasing (OTA):** bump `sketch/include/version.h` (or `release.py patch`),
commit, tag `vX.Y.Z`, run `scripts/generate_catalog.py`
(`releases/firmware_catalog.json` → `sketch/include/generated_catalog.h`,
embedded in the firmware), then `scripts/publish_ota.py vX.Y.Z` to stage the
binary in `releases/bin/`, enforce the retention limit and push. Keep catalog
entries and `releases/bin/` in step — an entry whose binary was pruned is a
404 for every device running the updater. `publish_ota.py` regenerates
`generated_catalog.h` itself, so the embedded list always matches the
catalog it just wrote; currently retained: `v2.6.8` + `v2.6.7`.

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

### LAN Devices app (Main Menu, after AI Voice)

HTTP-only local device control — no SSH, no OOM risk. Discovered via
mDNS and controlled through plain HTTP REST.

**Home Assistant** (the hub):
- mDNS discovery: `_home-assistant._tcp.local.` finds HA servers on LAN
- Manual add: host, port, long-lived access token (stored in NVS)
- `GET /api/states` → all entities with `entity_id`, `state`, `attributes`
- `POST /api/services/{domain}/{service}` → control (toggle, turn_on, etc.)
- Credentials stored in NVS namespace `lan_dev`, never in `secrets.h`

**Generic HTTP agents** (Pi, mobile Termux, desktop):
- Tiny HTTP listener on each machine exposing `POST /command`
- Discover via mDNS or manual add (name, host, port, base path)
- No SSH key provisioning — agent runs on the target, ESP32 just POSTs

**UI**: Main Menu icon after AI Voice opens a screen with:
- `[X]` close button top-right → returns to menu
- Swipeable vertical panel with location group headers ("Living Room", "Kitchen", etc.)
- Each row: icon + name + location + toggle switch (controllable) or state text (read-only)
- Entity types: switch, light, cover, fan, sensor

**Flash/RAM**: ~300 KB added (mDNS + JSON parsing), well within the ~4 MB app-slot headroom.

**v2.6.9** — LAN CLI commands released (3,256,384 bytes). Serial CLI for HA
server/agent configuration. Flash at ~44.5% of 7 MB app slot.
**v2.6.10** — LAN Devices icon + empty state (3,257,888 bytes). Dedicated
house+network icon replaces climate icon; empty state shows config hint when no
HA servers, device hint when none found. Flash at ~44.5% of 7 MB app slot.

**LAN Devices icon-based UI** — action icons in header (`LV_SYMBOL_REFRESH`,
`LV_SYMBOL_PLUS`, `LV_SYMBOL_SETTINGS`, `LV_SYMBOL_CLOSE`); multi-step keyboard
flows for adding servers (host → port → token) and agents (name → host → port
→ path), borrowing the Settings → WiFi keyboard pattern. Devices grouped by
location with toggles. Do NOT use a literal unicode cross for a button glyph —
`LV_SYMBOL_CLOSE` is what the Montserrat subset actually carries.

**CLI commands for LAN Devices configuration (serial CLI):**
```
lan open                                # Open the LAN Devices screen
lan close                               # Close it (back to menu)
lan add <host> <port> <token> [ssl]     # Add Home Assistant server
lan agent add <name> <host> <port> [base]  # Add HTTP agent (Pi/mobile/desktop)
lan list                                # List servers, agents, devices
lan refresh                             # Refresh device list from HA
lan toggle <entity_id>                  # Toggle a device (e.g. switch.kitchen)
lan remove <index>                      # Remove HA server by index
```
Credentials stored in NVS namespace `lan_dev`. Example: `lan add 192.168.3.50 8123 eyJ...`

**Add server / agent keyboard** mirrors Settings → Wi-Fi: a LEFT form panel
(title + textarea + OK/Cancel) with the LVGL keyboard on the RIGHT (352 px,
full height). Host uses the text keyboard, port uses the number keyboard.

**Testing on device (mandatory before hand-off):** use `tools/deskbar_ocr.py`
(`shot`/`ocr`/`shotocr`/`find <word>`) to read the real screen before tapping —
never guess coordinates. Capture the screenshot in the SAME serial session as
the triggering command, because each `DeskBarSerial` open/close cycle resets the
ESP32-S3 over USB-CDC and drops the UI back to the clock.

### Key files to read first
1. `sketch/src/file/file.cpp` — LittleFS init, `LITTLEFS_LABEL`
2. `sketch/include/xtask.h` — Wi-Fi PS logic (~line 913)
3. `sketch/src/ui/ui_events.cpp` — breathing state machine, CPU scaling
4. `sketch/src/task_msg/task_msg.cpp` — clock layout, label guards
5. `sketch/partitions/7MB_app_ota_2MB_littlefs.csv` — partition table

### Common gotchas
- **AI Voice answers "Bad server response"**: the device was POSTing the WAV to
  the compile-time placeholder `https://api.your-domain.com/ask.php` (a parked
  "domain for sale" page, HTTP 200 + text/html whose `<style>body{…}</style>`
  supplied the `{` the parser then choked on). Two bugs: the baked default was
  never moved to `autobots.my.to` (unlike `TELEMETRY_HOST`), and
  `ai_upload_task()` ignored `secrets_get_ai_url()` entirely, so an `ai_url`
  override only affected the text path. Fixed 2026-11-09: default host changed,
  `resolve_ai_endpoint()` added, non-JSON HTTP 200 now reports
  `Server sent a web page, not an AI reply`. Check the live value with
  `GET /api/secrets` → `ai_url`. Server side, the device ID must exist in
  `/var/www/data/registered_devices.json` or the reply is
  `{"status":"restricted"}`.
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
