# 🌐 TuneBar Web Remote REST API Reference

TuneBar hosts an embedded, high-performance HTTP REST API on port 80. All endpoints are accessible directly over your local Wi-Fi network.

---

## 📡 Core API Endpoints

| Endpoint | Method | Parameters | Description |
| :--- | :---: | :--- | :--- |
| `/api/status` | `GET` | *none* | Complete device telemetry JSON (active screen, free heap, battery mV/%, Wi-Fi RSSI, playback state). |
| `/api/screen` | `GET/POST` | `action=<target>&tab=<N>&scroll=<px>` | Switch active screen (`clock`, `weather`, `alarm`, `menu`, `settings`, `utility`, `music`, `radio`, `chat`). |
| `/api/touch` | `GET/POST` | `x=<X>&y=<Y>` or `swipe=<dir>` | Injects simulated capacitive touch tap coordinates or directional swipe gestures. |
| `/api/radio/lang` | `GET/POST` | `set=<hi\|en\|es\|cn\|de\|ja>` | Dynamically loads the pre-curated 10-station catalog for the chosen language. |
| `/api/radio/catalog` | `POST` | `id=<0\|1>` | Toggles between OnlineRadioFM (`0`) and RadioIndia (`1`) sub-catalogs. |
| `/api/radio/play` | `POST` | `idx=<0..N>` | Plays a specific station index from the currently active catalog. |
| `/api/radio/resume` | `POST` | *none* | Resumes playback of the current radio station. |
| `/api/radio/stop` | `POST` | *none* | Stops all audio playback and powers down the NS4150B amplifier. |
| `/api/vol` | `POST` | `val=<0..21>` | Sets master audio volume level (0=mute, 21=max). |
| `/api/bl` | `POST` | `state=<0\|1\|2>` | Sets backlight brightness preset (0=Low: 35%, 1=Med: 65%, 2=High: 100%). |
| `/api/alarm` | `GET/POST` | `enabled=<0\|1>&time=<HH:MM>` | Reads or configures RTC alarm wake-up time and armed state. |
| `/api/alarm/stop` | `POST` | *none* | Silences currently active ringing alarm buzzer. |
| `/api/alarm/test` | `POST` | *none* | Triggers immediate test alarm audio sequence. |
| `/api/lan/server` | `GET/POST` | `srv=<ip:port/path>` | Configures or inspects LAN music server target URL. |
| `/api/lan/fetch` | `GET/POST` | *none* | Triggers asynchronous indexing of remote LAN music tracks. |
| `/api/lan/play` | `GET/POST` | `idx=<0..N>` | Plays track index from indexed LAN library. |
| `/api/rec` | `GET/POST` | `action=<start\|stop\|play>` | Triggers voice recording, stops & normalizes, or plays local `/rec.wav` loopback. |
| `/api/ask` | `GET/POST` | `q=<text_query>` | Sends text query to the AI Assistant pipeline over Wi-Fi. |
| `/api/telemetry` | `POST` | `action=<snap\|flush>` | Captures an immediate telemetry sample or flushes the ring buffer to the server. |
| `/api/imu` | `GET/POST` | `autorotate=<0\|1>&rotate=<0\|180>` | Returns 6-axis accelerometer, gyroscope, temperature, and orientation JSON; controls auto-rotation. |
| `/api/imu/stream` | `GET` | *none* | Server-Sent Events (SSE) live streaming 10 Hz real-time motion and orientation telemetry. |
| `/api/update` | `POST` | `multipart/form-data (firmware.bin)` | Direct browser/curl firmware upload & flash endpoint over Wi-Fi LAN; verifies binary and restarts. |
| `/api/screenshot` | `GET` | *none* | Dumps live 640×172 RGB24 BMP framebuffer screenshot over HTTP (dynamically rotated to match orientation). |

---

## 💻 USB Serial CLI Commands

Connect over USB serial at **115200 baud** (using `python scripts/safe_serial_monitor.py listen` to avoid resets):

```bash
# System & Status
status                    # Print uptime, battery voltage, WiFi RSSI, and audio state
heap                      # Print detailed internal DRAM and external PSRAM breakdown
touch                     # Wake screen and reset sleep timer

# 6-Axis IMU & Orientation
imu                       # Print real-time accelerometer, gyroscope, temperature, and orientation
imu stream                # Stream 20 consecutive 10Hz motion readings to serial terminal
rotate 0                  # Force display to Normal Landscape (0 deg)
rotate 180                # Force display to Inverted Landscape (180 deg)
autorotate on / off       # Enable or disable automatic landscape orientation switching

# Radio Subsystem
radio lang hi             # Switch radio to Hindi
radio lang en             # Switch radio to English
radio lang es             # Switch radio to Spanish
radio lang cn             # Switch radio to Chinese
radio lang de             # Switch radio to German
radio lang ja             # Switch radio to Japanese
radio play <N>            # Play station index N
radio stop                # Stop playback

# Media & LAN Streaming
music                     # Switch UI to Music Player
lan server <ip:port>      # Set LAN HTTP media server address
lan fetch                 # Index LAN media tracks
lan play <N>              # Play LAN track N

# Display & Backlight
bl <low|med|high>         # Set brightness preset
bl <0-255>                # Set raw PWM duty
wifi on / wifi off        # Control Wi-Fi radio power state
```
