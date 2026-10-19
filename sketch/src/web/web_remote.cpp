#include "web_remote.h"
#include <WebServer.h>
#include <WiFi.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "esp_heap_caps.h"
#include "ESP32-audioI2S-master/Audio.h"
#include "file/file.h"
#include <LittleFS.h>
#include "task_msg/task_msg.h"
#include "lcd_bl_bsp/lcd_bl_pwm_bsp.h"
#include "battery/battery.h"
#include "pcf85063/pcf85063.h"
#include "alarm/alarm.h"
#include "telemetry/telemetry.h"
#include "lan_stream/lan_stream.h"
#include "lvgl_port/lvgl_port.h"
#include "ui/ui.h"
#include "ui/ui_events.h"
#include "user_config.h"
#include "secrets/secrets_manager.h"
#include "imu/imu_service.h"
#include "updater/updater.h"
#include "version.h"
#include "generated_catalog.h"
#include "power/power_profile.h"
#include "ui/screens/ui_Screen_PowerProfile.h"

extern Audio audio;
extern Preferences pref;
extern "C" void exit_clock_breathing(void);
extern "C" void record_user_interaction(void);
extern void screenPowerOn(void);
extern void audioSetVolume(uint8_t vol);
extern void updateInfoPanel(uint8_t pages);
extern void on_clock_touch_or_button(void);
extern void alarm_ui_refresh(void);
extern uint8_t mediaType;
extern int lan_track_idx;
extern const char* getRadioCatalogName(uint8_t cat);
extern void switchRadioCatalog(uint8_t cat);
extern void start_ai_voice_recording();
extern void stop_ai_voice_recording_and_process();
extern volatile bool ai_recording_active;
#include "es8311/es8311.h"
extern ES8311 speaker;

static WebServer server(80);
static bool server_running = false;

static const char HTML_PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TuneBar Web Remote</title>
<style>
  :root {
    --bg: #0b0f17;
    --card: #151d2a;
    --card-hover: #1c2637;
    --accent: #00d2ff;
    --accent-glow: rgba(0, 210, 255, 0.3);
    --text: #f1f5f9;
    --sub: #94a3b8;
    --danger: #ef4444;
    --success: #10b981;
    --border: #263346;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 14px; display: flex; justify-content: center; }
  .container { width: 100%; max-width: 560px; display: flex; flex-direction: column; gap: 12px; }
  .header { display: flex; justify-content: space-between; align-items: center; padding: 12px 16px; background: var(--card); border-radius: 12px; border: 1px solid var(--border); box-shadow: 0 4px 12px rgba(0,0,0,0.4); }
  .title { font-size: 1.25rem; font-weight: 700; color: var(--accent); display: flex; align-items: center; gap: 8px; }
  .badge { padding: 4px 10px; border-radius: 20px; font-size: 0.75rem; font-weight: 700; background: rgba(0,210,255,0.15); color: var(--accent); border: 1px solid rgba(0,210,255,0.3); }
  .card { background: var(--card); border: 1px solid var(--border); border-radius: 12px; padding: 14px; display: flex; flex-direction: column; gap: 10px; box-shadow: 0 4px 14px rgba(0,0,0,0.3); }
  .card-title { font-size: 0.9rem; font-weight: 700; color: var(--sub); text-transform: uppercase; letter-spacing: 0.06em; display: flex; justify-content: space-between; align-items: center; }
  .stats-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 8px; }
  .stat-chip { background: #0f1522; padding: 8px 10px; border-radius: 8px; border: 1px solid #1f2b3e; }
  .stat-chip span { color: var(--sub); display: block; font-size: 0.7rem; margin-bottom: 2px; }
  .stat-chip strong { color: #fff; font-size: 0.9rem; font-weight: 700; }
  .btn-group { display: flex; gap: 6px; flex-wrap: wrap; }
  .btn { flex: 1; min-width: 80px; padding: 8px 12px; border: 1px solid var(--border); border-radius: 8px; background: #1c2637; color: var(--text); font-weight: 600; font-size: 0.82rem; cursor: pointer; transition: 0.15s; text-align: center; }
  .btn:hover { background: #26344a; border-color: var(--accent); }
  .btn.active, .btn.primary { background: linear-gradient(135deg, #00d2ff, #3a7bd5); color: #fff; border-color: transparent; box-shadow: 0 2px 8px var(--accent-glow); }
  .btn.danger { background: #dc2626; color: #fff; border-color: transparent; }
  .btn.success { background: #059669; color: #fff; border-color: transparent; }
  .btn.warning { background: #d97706; color: #fff; border-color: transparent; }
  .slider-box { display: flex; align-items: center; gap: 10px; background: #0f1522; padding: 8px 12px; border-radius: 8px; border: 1px solid #1f2b3e; }
  .slider-box input[type="range"] { flex: 1; accent-color: var(--accent); cursor: pointer; height: 6px; }
  .station-list { display: flex; flex-direction: column; gap: 5px; max-height: 200px; overflow-y: auto; padding-right: 4px; }
  .station-item { padding: 8px 10px; background: #0f1522; border-radius: 6px; font-size: 0.82rem; cursor: pointer; border: 1px solid transparent; display: flex; justify-content: space-between; align-items: center; transition: 0.15s; }
  .station-item:hover { border-color: var(--accent); background: #162030; }
  .station-item.active { border-color: var(--accent); background: rgba(0,210,255,0.12); color: var(--accent); font-weight: 700; }
  .input-row { display: flex; gap: 6px; }
  .input-row input { flex: 1; background: #0f1522; border: 1px solid var(--border); border-radius: 8px; padding: 8px 10px; color: #fff; font-size: 0.85rem; outline: none; }
  .input-row input:focus { border-color: var(--accent); }
  .ringing-alert { background: rgba(239, 68, 68, 0.2); border: 1px solid var(--danger); border-radius: 8px; padding: 10px; display: none; align-items: center; justify-content: space-between; }
  .touch-screen-preview { width: 100%; height: 86px; background: #050810; border: 2px dashed #2a3b52; border-radius: 10px; position: relative; cursor: crosshair; display: flex; align-items: center; justify-content: center; overflow: hidden; user-select: none; }
  .touch-screen-preview:hover { border-color: var(--accent); }
  .touch-screen-label { position: absolute; font-size: 0.72rem; color: #4b6382; pointer-events: none; }
  .touch-coord { position: absolute; bottom: 4px; right: 6px; font-size: 0.68rem; color: var(--accent); font-family: monospace; }
  .dpad-container { display: grid; grid-template-columns: repeat(3, 1fr); gap: 6px; width: 160px; margin: 0 auto; }
  .chip-pills { display: flex; flex-wrap: wrap; gap: 6px; }
  .chip { padding: 4px 9px; border-radius: 12px; background: #1a2333; font-size: 0.74rem; color: #cbd5e1; cursor: pointer; border: 1px solid #28374d; transition: 0.15s; }
  .chip:hover { border-color: var(--accent); color: var(--accent); background: #202d42; }
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <div class="title">📻 TuneBar Web Remote</div>
    <div class="badge" id="badge-status">ONLINE</div>
  </div>

  <div id="ringing-box" class="ringing-alert">
    <span style="color:#ef4444;font-weight:700">⏰ ALARM RINGING!</span>
    <button class="btn danger" style="flex:none" onclick="callApi('/api/alarm/stop')">Dismiss Alarm</button>
  </div>

  <!-- CARD 1: DIAGNOSTICS & TELEMETRY -->
  <div class="card">
    <div class="card-title">
      <span>📊 Device Telemetry & Memory</span>
      <span id="stat-uptime" style="font-size:0.75rem;color:var(--sub);font-weight:normal">--</span>
    </div>
    <div class="stats-grid">
      <div class="stat-chip"><span>Internal DRAM</span><strong id="stat-dram">--</strong></div>
      <div class="stat-chip"><span>Ext PSRAM</span><strong id="stat-psram">--</strong></div>
      <div class="stat-chip"><span>Battery</span><strong id="stat-batt">--</strong></div>
      <div class="stat-chip"><span>WiFi RSSI</span><strong id="stat-wifi">--</strong></div>
      <div class="stat-chip"><span>Display</span><strong id="stat-screen">--</strong></div>
      <div class="stat-chip"><span>Boot Count</span><strong id="stat-boot">--</strong></div>
    </div>
    <div style="background:#0c121e;padding:8px 10px;border-radius:8px;border:1px solid #1f2b3e;display:flex;flex-direction:column;gap:5px;font-size:0.78rem">
      <div style="display:flex;justify-content:space-between;align-items:center">
        <span style="color:var(--sub)">Cloud Consent:</span>
        <span id="telem-consent-badge" style="padding:2px 8px;border-radius:10px;font-size:0.7rem;font-weight:bold;background:#1e293b;color:#94a3b8">CHECKING</span>
      </div>
      <div style="display:flex;justify-content:space-between;align-items:center">
        <span style="color:var(--sub)">Database Table:</span>
        <code id="telem-table-name" style="background:#151d2a;color:#00d2ff;padding:2px 6px;border-radius:4px;font-family:monospace">TB_--------</code>
      </div>
      <div style="display:flex;gap:6px;align-items:center;margin-top:2px">
        <input type="text" id="telem-full-url" readonly style="flex:1;background:#151d2a;border:1px solid #263346;border-radius:6px;padding:4px 8px;font-size:0.72rem;color:#94a3b8;font-family:monospace" value="https://autobots.my.to/telemetry.php?table=...">
        <button class="btn" style="flex:none;padding:4px 8px;font-size:0.72rem" onclick="copyTelemUrl()">📋 Copy URL</button>
      </div>
    </div>
    <div class="btn-group" style="margin-top:2px">
      <button class="btn" onclick="callApi('/api/telemetry?action=snap')">📸 Snap Telemetry</button>
      <button class="btn" onclick="callApi('/api/telemetry?action=flush')">☁ Flush Now</button>
      <button class="btn success" id="telem-consent-btn" onclick="toggleCloudConsent()">Accept Consent</button>
      <a id="telem-cloud-btn" class="btn primary" target="_blank" href="#" style="text-decoration:none;display:flex;align-items:center;justify-content:center">📊 Open Dashboard</a>
    </div>
  </div>

  <!-- CARD 2: REMOTE TOUCH & SWIPE CONTROLLER -->
  <div class="card">
    <div class="card-title">
      <span>👆 Remote Touch & Swipe Controller</span>
      <span style="font-size:0.75rem;color:var(--accent)">640 × 172 Display</span>
    </div>
    <div class="touch-screen-preview" id="touch-canvas" onclick="handleCanvasClick(event)" onmousemove="handleCanvasHover(event)">
      <div class="touch-screen-label">Click anywhere to Tap Screen (X, Y)</div>
      <div class="touch-coord" id="canvas-coord">X: --, Y: --</div>
    </div>
    <div style="display:flex;justify-content:space-between;align-items:center;margin-top:4px">
      <div class="btn-group" style="flex:1">
        <button class="btn" onclick="sendTouch(80, 86)">⏰ Clock</button>
        <button class="btn" onclick="sendTouch(230, 86)">📻 Radio</button>
        <button class="btn" onclick="sendTouch(380, 86)">🎵 Music</button>
        <button class="btn" onclick="sendTouch(530, 86)">🤖 AI Chat</button>
        <button class="btn warning" onclick="sendTouch(35, 140)">↩ Return</button>
      </div>
      <div class="dpad-container" style="flex:none;margin-left:10px">
        <div></div>
        <button class="btn" onclick="sendSwipe('up')">⬆</button>
        <div></div>
        <button class="btn" onclick="sendSwipe('left')">⬅</button>
        <button class="btn primary" onclick="sendTouch(320, 140)">⏯</button>
        <button class="btn" onclick="sendSwipe('right')">➡</button>
        <div></div>
        <button class="btn" onclick="sendSwipe('down')">⬇</button>
        <div></div>
      </div>
    </div>
  </div>

  <!-- CARD 3: AI VOICE ASSISTANT -->
  <div class="card">
    <div class="card-title">
      <span>🤖 AI Voice Assistant</span>
      <span id="ai-status-badge" class="badge" style="display:none">STREAMING</span>
    </div>
    <div class="input-row">
      <input type="text" id="ask-query" placeholder="Ask AI Voice Assistant..." onkeydown="if(event.key==='Enter') askAssistant()">
      <button class="btn primary" onclick="askAssistant()">Ask</button>
    </div>
    <div class="chip-pills">
      <div class="chip" onclick="askPreset('Tell me a fun fact about space')">🚀 Fun Fact</div>
      <div class="chip" onclick="askPreset('What is the weather in Delhi?')">🌤 Weather</div>
      <div class="chip" onclick="askPreset('Tell me a short funny joke')">😂 Tell Joke</div>
      <div class="chip" onclick="askPreset('Give me a motivational thought')">💡 Motivation</div>
    </div>
    <div id="ai-feedback" style="font-size:0.8rem;color:var(--accent);min-height:16px"></div>
    <div class="btn-group" style="margin-top:6px">
      <button class="btn primary" onclick="callApi('/api/rec?action=start')">🎤 Record Voice</button>
      <button class="btn warning" onclick="callApi('/api/rec?action=stop')">⏹ Stop & Play</button>
      <button class="btn success" onclick="callApi('/api/rec?action=play')">🔊 Play /rec.wav</button>
      <a class="btn" href="/rec.wav" target="_blank" download="rec.wav" style="text-decoration:none;display:flex;align-items:center;justify-content:center">⬇ WAV</a>
    </div>
  </div>

  <!-- CARD 4: NOW PLAYING & AUDIO CONTROLS -->
  <div class="card">
    <div class="card-title">
      <span>Now Playing</span>
      <span id="stat-site" style="color:var(--accent);font-size:0.8rem">--</span>
    </div>
    <div id="stat-track" style="font-size:1.05rem;font-weight:700;min-height:24px;color:#fff">--</div>
    <div class="slider-box">
      <span style="font-size:0.85rem">🔊</span>
      <input type="range" id="vol-slider" min="0" max="21" value="15" onchange="setVolume(this.value)">
      <span id="vol-val" style="width:24px;text-align:right;font-weight:700">15</span>
    </div>
    <div class="btn-group">
      <button class="btn danger" onclick="callApi('/api/radio/stop')">⏹ Stop</button>
      <button class="btn primary" onclick="callApi('/api/radio/resume')">▶ Play</button>
    </div>
  </div>

  <!-- CARD 5: RADIO STATIONS CATALOG -->
  <div class="card">
    <div class="card-title">Radio Stream Catalog</div>
    <div class="btn-group">
      <button class="btn" id="cat-btn-0" onclick="switchCatalog(0)">OnlineRadioFM.in</button>
      <button class="btn" id="cat-btn-1" onclick="switchCatalog(1)">RadioIndia.in</button>
    </div>
    <div class="station-list" id="stations-box">
      <!-- Populated via JS -->
    </div>
  </div>

  <!-- CARD 6: MUSIC (LOCAL & LAN) -->
  <div class="card">
    <div class="card-title">🎵 MUSIC (Local & LAN)</div>
    <div class="input-row">
      <input type="text" id="lan-server" value="" placeholder="IP:port/path">
      <button class="btn primary" onclick="updateLanServer()">Connect</button>
      <button class="btn" onclick="fetchLanFiles()">Fetch</button>
    </div>
    <div id="lan-status" style="font-size:0.82rem;font-weight:600;min-height:18px;color:var(--sub);margin-top:2px">Indexed: 0 files</div>
    <div class="station-list" id="lan-files-box" style="margin-top:4px;max-height:180px">
      <!-- Populated via JS -->
    </div>
    <div class="btn-group" style="margin-top:4px">
      <button class="btn success" onclick="playLanTrack(0)">▶ Play First Track</button>
    </div>
  </div>

  <!-- CARD 7: ALARM CLOCK -->
  <div class="card">
    <div class="card-title">Alarm Clock</div>
    <div class="input-row">
      <input type="time" id="alarm-time" value="07:00" onfocus="isEditingAlarm=true" onblur="isEditingAlarm=false">
      <button class="btn primary" onclick="setAlarmTime()">Set Time</button>
      <button class="btn" id="alarm-toggle-btn" onclick="toggleAlarm()">Enable</button>
      <button class="btn" onclick="callApi('/api/alarm/test')">Test</button>
    </div>
    <div id="alarm-feedback" style="font-size:0.8rem;color:#10b981;margin-top:2px;min-height:16px"></div>
  </div>

  <!-- CARD 8: SCREEN BRIGHTNESS -->
  <div class="card">
    <div class="card-title">Screen Brightness Presets</div>
    <div class="btn-group">
      <button class="btn" id="bl-btn-0" onclick="setBL(0)">Dim (Low)</button>
      <button class="btn" id="bl-btn-1" onclick="setBL(1)">Normal (Med)</button>
      <button class="btn" id="bl-btn-2" onclick="setBL(2)">Bright (High)</button>
    </div>
  </div>

  <!-- CARD 9: 6-AXIS MOTION & LANDSCAPE AUTO-ROTATION -->
  <div class="card">
    <div class="card-title">
      <span>📐 6-Axis IMU & Orientation</span>
      <span id="imu-state" style="color:var(--accent);font-size:0.78rem">--</span>
    </div>
    <div class="stats-grid">
      <div class="stat-chip"><span>Accel X, Y, Z (g)</span><strong id="imu-accel">--</strong></div>
      <div class="stat-chip"><span>Gyro X, Y, Z (dps)</span><strong id="imu-gyro">--</strong></div>
      <div class="stat-chip"><span>Die Temp</span><strong id="imu-temp">--</strong></div>
    </div>
    <div class="btn-group">
      <button class="btn" id="imu-rot-0" onclick="callApi('/api/imu?rotate=0')">Normal (0°)</button>
      <button class="btn" id="imu-rot-180" onclick="callApi('/api/imu?rotate=180')">Invert (180°)</button>
      <button class="btn primary" id="imu-autorot-btn" onclick="toggleAutoRotate()">Auto-Rotate: ON</button>
      <button class="btn" id="imu-stream-btn" onclick="toggleImuLiveStream()">📡 Live Stream</button>
    </div>
    <div id="imu-stream-status" style="font-size:0.75rem;color:var(--sub);min-height:16px"></div>
  </div>

  <!-- CARD 10: MULTI-REPO FIRMWARE OTA UPDATE -->
  <div class="card">
    <div class="card-title">
      <span>🚀 Firmware OTA & GitHub Releases</span>
      <span id="ota-batt-badge" style="font-size:0.75rem;padding:2px 8px;border-radius:10px;background:#063025;color:#00FF88">🔋 Battery OK</span>
    </div>

    <!-- Section A: Approved Repositories -->
    <div style="font-size:0.8rem;color:var(--sub);margin-bottom:4px;display:flex;justify-content:space-between">
      <span>Active Repository (Approved Community / Official):</span>
      <a href="https://github.com/rahulraj80/TuneBar/blob/main/releases/approved_repos.json" target="_blank" style="color:var(--accent);font-size:0.75rem;text-decoration:none">View Registry ↗</a>
    </div>
    <select id="ota-repo-select" onchange="onRepoChanged()" style="width:100%;padding:6px;background:#18202c;border:1px solid #334455;color:var(--text);border-radius:4px;margin-bottom:6px">
      <option value="official">TuneBar Official — by rahulraj80 [Official]</option>
    </select>
    <div class="stats-grid" style="margin-bottom:8px">
      <div class="stat-chip"><span>Avg Free DRAM</span><strong id="repo-dram" style="color:#00FF88">182 KB</strong></div>
      <div class="stat-chip"><span>Avg Free PSRAM</span><strong id="repo-psram" style="color:#00E5FF">4.8 MB</strong></div>
      <div class="stat-chip"><span>Clock Battery</span><strong id="repo-clock-batt" style="color:#FFB800">14.5 Hours</strong></div>
      <div class="stat-chip"><span>Active Battery</span><strong id="repo-active-batt" style="color:#FF8800">5.2 Hours</strong></div>
    </div>

    <!-- Section B: Releases Selector -->
    <div style="font-size:0.8rem;color:var(--sub);margin-bottom:4px">Select Curated Release (Last 2 Major, 2 Minor, 2 Patch):</div>
    <div style="display:flex;gap:6px;margin-bottom:6px">
      <select id="ota-release-select" onchange="onReleaseChanged()" style="flex:1;padding:6px;background:#18202c;border:1px solid #334455;color:var(--text);border-radius:4px">
        <option value="0">🟢 v1.2.6 (PATCH) — Oct 12, 2026 [6.03 MB]</option>
        <option value="1">🟢 v1.2.5 (PATCH) — Oct 10, 2026 [6.02 MB]</option>
        <option value="2">🔵 v1.2.0 (MINOR) — Oct 05, 2026 [5.98 MB]</option>
        <option value="3">🔵 v1.1.0 (MINOR) — Sep 28, 2026 [5.85 MB]</option>
        <option value="4">🟣 v2.0.0 (MAJOR) — Nov 01, 2026 [6.20 MB]</option>
        <option value="5">🟣 v1.0.0 (MAJOR) — Sep 01, 2026 [5.70 MB]</option>
      </select>
      <button class="btn primary" id="ota-github-btn" onclick="flashSelectedRelease()">⚡ Flash from GitHub</button>
    </div>
    <div id="ota-release-notes" style="font-size:0.75rem;color:#AAA;background:#111822;padding:6px 8px;border-radius:4px;border:1px solid #223344;min-height:36px;white-space:pre-line;margin-bottom:8px">Loading release notes...</div>

    <!-- Section C: Custom Numeric Version & Local Upload -->
    <div style="display:flex;gap:8px;flex-wrap:wrap;align-items:center;padding-top:4px;border-top:1px solid #223344">
      <div style="display:flex;gap:4px;align-items:center;font-size:0.8rem">
        <span>Custom: v</span>
        <input type="number" id="v-maj" min="0" max="9" value="1" style="width:34px;padding:3px;background:#18202c;border:1px solid #334455;color:#fff;border-radius:3px">.
        <input type="number" id="v-min" min="0" max="99" value="2" style="width:38px;padding:3px;background:#18202c;border:1px solid #334455;color:#fff;border-radius:3px">.
        <input type="number" id="v-pat" min="0" max="99" value="6" style="width:38px;padding:3px;background:#18202c;border:1px solid #334455;color:#fff;border-radius:3px">
        <button class="btn" style="padding:4px 8px;font-size:0.75rem" onclick="flashCustomVersion()">🔍 Pull & Flash</button>
      </div>
      <div style="display:flex;gap:6px;align-items:center;flex:1;min-width:240px">
        <input type="file" id="ota-file-input" accept=".bin" style="font-size:0.75rem;color:var(--text);flex:1">
        <button class="btn" id="ota-upload-btn" style="padding:4px 10px;font-size:0.75rem" onclick="uploadFirmware()">Upload .bin</button>
      </div>
    </div>

    <!-- Progress & Status Box -->
    <div id="ota-progress-box" style="display:none;margin-top:8px">
      <div style="background:#111;border-radius:4px;overflow:hidden;height:14px;margin-bottom:4px;border:1px solid #00E5FF">
        <div id="ota-progress-bar" style="background:#00E5FF;width:0%;height:100%;transition:width 0.1s"></div>
      </div>
      <div id="ota-status-text" style="font-size:0.78rem;color:#00E5FF;font-weight:bold">Ready</div>
    </div>
  </div>

  <!-- CARD 11: CLOUD ENDPOINTS & SECRETS MANAGER (/secrets.ini) -->
  <div class="card">
    <div class="card-title">
      <span>🔐 Cloud Endpoints & Secrets (/secrets.ini)</span>
      <span id="secrets-ini-badge" style="font-size:0.75rem;padding:2px 8px;border-radius:10px;background:#1e293b;color:#94a3b8">DEFAULT</span>
    </div>
    <div style="font-size:0.75rem;color:var(--sub)">
      Override built-in cloud endpoints with your own private servers for total privacy. Values are saved to flash as <code>/secrets.ini</code>.
    </div>
    <div style="display:flex;flex-direction:column;gap:6px">
      <div>
        <label style="font-size:0.72rem;color:var(--sub);display:block;margin-bottom:2px">AI Assistant URL:</label>
        <input type="text" id="sec-ai-url" style="width:100%;background:#0f1522;border:1px solid var(--border);border-radius:6px;padding:6px;color:#fff;font-size:0.78rem" placeholder="https://yourserver.com/ask.php">
      </div>
      <div>
        <label style="font-size:0.72rem;color:var(--sub);display:block;margin-bottom:2px">Telemetry Ingestion Endpoint:</label>
        <input type="text" id="sec-telem-url" style="width:100%;background:#0f1522;border:1px solid var(--border);border-radius:6px;padding:6px;color:#fff;font-size:0.78rem" placeholder="https://yourserver.com/myapi.php">
      </div>
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:6px">
        <div>
          <label style="font-size:0.72rem;color:var(--sub);display:block;margin-bottom:2px">Interval (seconds):</label>
          <input type="number" id="sec-interval" min="60" max="86400" value="300" style="width:100%;background:#0f1522;border:1px solid var(--border);border-radius:6px;padding:6px;color:#fff;font-size:0.78rem">
        </div>
        <div>
          <label style="font-size:0.72rem;color:var(--sub);display:block;margin-bottom:2px">Master PIN / Salt:</label>
          <input type="password" id="sec-pin" style="width:100%;background:#0f1522;border:1px solid var(--border);border-radius:6px;padding:6px;color:#fff;font-size:0.78rem" placeholder="Optional PIN">
        </div>
      </div>
    </div>
    <div class="btn-group" style="margin-top:4px">
      <button class="btn primary" onclick="saveSecretsIni()">💾 Save /secrets.ini</button>
      <button class="btn warning" onclick="restoreSecretsDefault()">↺ Restore Defaults</button>
    </div>
    <div id="secrets-feedback" style="font-size:0.75rem;min-height:16px"></div>
  </div>
</div>

<script>
let alarmState = { enabled: false, time: "07:00" };
let currentCat = 0;
let lastLanCount = -1;
let isEditingAlarm = false;
let lastAlarmEditTime = 0;

function callApi(url, method='POST') {
  fetch(url, { method: method })
    .then(r => r.json())
    .then(refreshData)
    .catch(console.error);
}

function copyTelemUrl() {
  const inp = document.getElementById('telem-full-url');
  if (inp && inp.value) {
    navigator.clipboard.writeText(inp.value).then(() => {
      alert("Telemetry Dashboard URL copied to clipboard!\n\n" + inp.value);
    }).catch(() => {
      inp.select();
      document.execCommand('copy');
      alert("Copied: " + inp.value);
    });
  }
}

function toggleCloudConsent() {
  const btn = document.getElementById('telem-consent-btn');
  const isCurrentlyGranted = (btn && btn.innerText.includes('Revoke'));
  const action = isCurrentlyGranted ? 'decline' : 'accept';
  callApi('/api/consent?action=' + action);
}

function saveSecretsIni() {
  const ai = document.getElementById('sec-ai-url').value.trim();
  const telem = document.getElementById('sec-telem-url').value.trim();
  const interval = document.getElementById('sec-interval').value.trim();
  const pin = document.getElementById('sec-pin').value.trim();
  const fb = document.getElementById('secrets-feedback');

  fb.innerHTML = '<span style="color:#00d2ff">⏳ Saving /secrets.ini to device flash...</span>';
  const params = new URLSearchParams();
  if (ai) params.append('ai_url', ai);
  if (telem) params.append('telem_url', telem);
  if (interval) params.append('interval', interval);
  if (pin) params.append('pin', pin);
  params.append('consent', '1');

  fetch('/api/secrets', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: params.toString()
  })
  .then(r => r.json())
  .then(res => {
    if (res.status === 'ok') {
      fb.innerHTML = '<span style="color:#10b981">✓ Saved! /secrets.ini is now active on device.</span>';
      setTimeout(() => { fb.innerHTML = ''; }, 4000);
      callApi('/api/status', 'GET');
    } else {
      fb.innerHTML = '<span style="color:#ef4444">⚠ Error: ' + res.message + '</span>';
    }
  })
  .catch(err => {
    fb.innerHTML = '<span style="color:#ef4444">⚠ Failed to save /secrets.ini</span>';
    console.error(err);
  });
}

function restoreSecretsDefault() {
  if (!confirm("Are you sure you want to remove /secrets.ini and restore built-in firmware endpoints?")) return;
  const fb = document.getElementById('secrets-feedback');
  fb.innerHTML = '<span style="color:#00d2ff">⏳ Restoring firmware defaults...</span>';
  fetch('/api/secrets?action=restore', { method: 'POST' })
    .then(r => r.json())
    .then(res => {
      fb.innerHTML = '<span style="color:#10b981">✓ Restored firmware defaults.</span>';
      setTimeout(() => { fb.innerHTML = ''; }, 4000);
      callApi('/api/status', 'GET');
    })
    .catch(err => {
      fb.innerHTML = '<span style="color:#ef4444">⚠ Failed to restore defaults.</span>';
      console.error(err);
    });
}

function handleCanvasClick(e) {
  const rect = e.currentTarget.getBoundingClientRect();
  const relX = (e.clientX - rect.left) / rect.width;
  const relY = (e.clientY - rect.top) / rect.height;
  const devX = Math.round(relX * 640);
  const devY = Math.round(relY * 172);
  sendTouch(devX, devY);
}

function handleCanvasHover(e) {
  const rect = e.currentTarget.getBoundingClientRect();
  const devX = Math.round(((e.clientX - rect.left) / rect.width) * 640);
  const devY = Math.round(((e.clientY - rect.top) / rect.height) * 172);
  document.getElementById('canvas-coord').innerText = `X: ${devX}, Y: ${devY}`;
}

function sendTouch(x, y) {
  fetch(`/api/touch?x=${x}&y=${y}`, { method: 'POST' })
    .then(r => r.json())
    .then(refreshData)
    .catch(console.error);
}

function sendSwipe(dir) {
  fetch(`/api/touch?swipe=${dir}`, { method: 'POST' })
    .then(r => r.json())
    .then(refreshData)
    .catch(console.error);
}

function askPreset(txt) {
  document.getElementById('ask-query').value = txt;
  askAssistant();
}

function askAssistant() {
  const q = document.getElementById('ask-query').value.trim();
  if (q) {
    const fb = document.getElementById('ai-feedback');
    fb.innerHTML = `<span>⏳ Querying AI Assistant & streaming audio response...</span>`;
    fetch('/api/ask?q=' + encodeURIComponent(q), { method: 'POST' })
      .then(r => r.json())
      .then(data => {
        refreshData(data);
        fb.innerHTML = `<span style="color:#10b981">✓ Response streaming on TuneBar speaker</span>`;
        setTimeout(() => { fb.innerHTML = ''; }, 6000);
      })
      .catch(err => {
        fb.innerHTML = `<span style="color:#ef4444">⚠ Error querying AI</span>`;
        console.error(err);
      });
    document.getElementById('ask-query').value = '';
  }
}

function updateLanServer() {
  const s = document.getElementById('lan-server').value.trim();
  const st = document.getElementById('lan-status');
  st.innerHTML = '<span style="color:#00d2ff">⏳ Connecting and indexing tracks from ' + s + '...</span>';
  fetch('/api/lan/server?srv=' + encodeURIComponent(s), { method: 'POST' })
    .then(r => r.json())
    .then(data => {
      refreshData(data);
      if (data.lan_count > 0) {
        st.innerHTML = '<span style="color:#10b981">✓ Connected! Indexed ' + data.lan_count + ' audio files.</span>';
      } else {
        st.innerHTML = '<span style="color:#ef4444">⚠ 0 audio files found or server unreachable.</span>';
      }
    })
    .catch(err => {
      st.innerHTML = '<span style="color:#ef4444">⚠ Error connecting to server.</span>';
      console.error(err);
    });
}

function fetchLanFiles() {
  const st = document.getElementById('lan-status');
  st.innerHTML = '<span style="color:#00d2ff">⏳ Fetching audio catalog from LAN server...</span>';
  fetch('/api/lan/fetch', { method: 'POST' })
    .then(r => r.json())
    .then(data => {
      refreshData(data);
      if (data.lan_count > 0) {
        st.innerHTML = '<span style="color:#10b981">✓ Successfully fetched ' + data.lan_count + ' audio files.</span>';
      } else {
        st.innerHTML = '<span style="color:#ef4444">⚠ 0 audio files found or server unreachable.</span>';
      }
    })
    .catch(err => {
      st.innerHTML = '<span style="color:#ef4444">⚠ Error fetching files.</span>';
      console.error(err);
    });
}

function playLanTrack(idx) {
  callApi('/api/lan/play?idx=' + idx);
}

function setVolume(v) {
  document.getElementById('vol-val').innerText = v;
  callApi('/api/vol?val=' + v);
}

function setBL(s) {
  callApi('/api/bl?state=' + s);
}

function switchCatalog(id) {
  callApi('/api/radio/catalog?id=' + id);
}

function playStation(idx) {
  callApi('/api/radio/play?idx=' + idx);
}

function setAlarmTime() {
  lastAlarmEditTime = Date.now();
  const t = document.getElementById('alarm-time').value;
  if (!t) return;
  alarmState.time = t;
  const fb = document.getElementById('alarm-feedback');
  if (fb) {
    fb.innerHTML = '<span style="color:#10b981">✓ Alarm time saved: ' + t + '</span>';
    setTimeout(() => { if (fb && Date.now() - lastAlarmEditTime >= 3000) fb.innerHTML = ''; }, 3000);
  }
  fetch('/api/alarm', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'time=' + encodeURIComponent(t) + '&enabled=' + (alarmState.enabled ? 1 : 0)
  })
  .then(r => r.json())
  .then(refreshData)
  .catch(console.error);
}

function toggleAlarm() {
  lastAlarmEditTime = Date.now();
  const t = document.getElementById('alarm-time').value;
  const nextEn = alarmState.enabled ? 0 : 1;
  alarmState.enabled = (nextEn === 1);
  const ab = document.getElementById('alarm-toggle-btn');
  ab.innerText = alarmState.enabled ? 'Disable' : 'Enable';
  ab.className = 'btn ' + (alarmState.enabled ? 'success' : '');
  fetch('/api/alarm', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: 'enabled=' + nextEn + '&time=' + encodeURIComponent(t)
  })
  .then(r => r.json())
  .then(refreshData)
  .catch(console.error);
}

function refreshData(data) {
  document.getElementById('stat-dram').innerText = (data.dram_free || Math.round(data.heap/1024)) + ' KB Free (' + (data.dram_pct || 65) + '% used)';
  document.getElementById('stat-psram').innerText = (data.psram / 1048576).toFixed(1) + ' MB Free';
  document.getElementById('stat-batt').innerText = data.batt_v + 'V (' + data.batt_pct + '%)';
  currentBattPct = data.batt_pct || 100;
  const otaBadge = document.getElementById('ota-batt-badge');
  if (otaBadge) {
    if (currentBattPct >= 70) {
      otaBadge.style.background = '#063025';
      otaBadge.style.color = '#00FF88';
      otaBadge.innerText = '🔋 Battery ' + currentBattPct + '% (Safe to Flash)';
    } else {
      otaBadge.style.background = '#3A1E00';
      otaBadge.style.color = '#FFA500';
      otaBadge.innerText = '⚠️ Battery ' + currentBattPct + '% (Blocked < 70%)';
    }
  }
  document.getElementById('stat-wifi').innerText = data.rssi + ' dBm';
  document.getElementById('stat-screen').innerText = data.bl_off ? 'Locked/Off' : ('BL: ' + ['Low','Med','High'][data.bl_state]);
  document.getElementById('stat-boot').innerText = (data.boot_count || 1) + ' boots';
  document.getElementById('stat-uptime').innerText = 'Uptime: ' + Math.floor(data.uptime_s / 60) + 'm ' + (data.uptime_s % 60) + 's';

  const telemBtn = document.getElementById('telem-cloud-btn');
  if (telemBtn && data.telem_url) {
    telemBtn.href = data.telem_url;
  }
  const telemTbl = document.getElementById('telem-table-name');
  if (telemTbl && data.telem_table) {
    telemTbl.innerText = data.telem_table;
  }
  const telemFullUrl = document.getElementById('telem-full-url');
  if (telemFullUrl && data.telem_url) {
    telemFullUrl.value = data.telem_url;
  }
  const telemConsentBadge = document.getElementById('telem-consent-badge');
  const telemConsentBtn = document.getElementById('telem-consent-btn');
  if (telemConsentBadge) {
    if (data.consent) {
      telemConsentBadge.style.background = '#063025';
      telemConsentBadge.style.color = '#00FF88';
      telemConsentBadge.innerText = 'GRANTED';
      if (telemConsentBtn) {
        telemConsentBtn.innerText = 'Revoke Consent';
        telemConsentBtn.className = 'btn warning';
      }
    } else {
      telemConsentBadge.style.background = '#3A1E00';
      telemConsentBadge.style.color = '#FFA500';
      telemConsentBadge.innerText = data.consent_decided ? 'DECLINED (OFFLINE)' : 'PENDING NOTICE';
      if (telemConsentBtn) {
        telemConsentBtn.innerText = 'Accept Consent';
        telemConsentBtn.className = 'btn success';
      }
    }
  }

  // Update Secrets Manager card fields if not actively editing
  const secIniBadge = document.getElementById('secrets-ini-badge');
  if (secIniBadge) {
    if (data.custom_ini) {
      secIniBadge.style.background = '#063025';
      secIniBadge.style.color = '#00FF88';
      secIniBadge.innerText = 'ACTIVE (/secrets.ini)';
    } else {
      secIniBadge.style.background = '#1e293b';
      secIniBadge.style.color = '#94a3b8';
      secIniBadge.innerText = 'FIRMWARE DEFAULT';
    }
  }
  if (document.activeElement !== document.getElementById('sec-ai-url') && data.ai_url) {
    document.getElementById('sec-ai-url').value = data.ai_url;
  }
  if (document.activeElement !== document.getElementById('sec-telem-url') && data.telem_endpoint) {
    document.getElementById('sec-telem-url').value = data.telem_endpoint;
  }
  if (document.activeElement !== document.getElementById('sec-interval') && data.telem_interval) {
    document.getElementById('sec-interval').value = data.telem_interval;
  }

  document.getElementById('stat-site').innerText = data.catalog_name;
  document.getElementById('stat-track').innerText = data.station_name || (data.audio_running ? (data.media_type === 2 ? 'AI Speech Active' : 'Streaming') : 'Idle');

  document.getElementById('vol-slider').value = data.vol;
  document.getElementById('vol-val').innerText = data.vol;

  document.getElementById('bl-btn-0').className = 'btn' + (data.bl_state === 0 ? ' active' : '');
  document.getElementById('bl-btn-1').className = 'btn' + (data.bl_state === 1 ? ' active' : '');
  document.getElementById('bl-btn-2').className = 'btn' + (data.bl_state === 2 ? ' active' : '');

  document.getElementById('cat-btn-0').className = 'btn' + (data.catalog === 0 ? ' active' : '');
  document.getElementById('cat-btn-1').className = 'btn' + (data.catalog === 1 ? ' active' : '');

  alarmState.enabled = data.alarm_enabled;
  alarmState.time = data.alarm_time;
  if (!isEditingAlarm && (Date.now() - lastAlarmEditTime > 4000) && document.activeElement !== document.getElementById('alarm-time')) {
    document.getElementById('alarm-time').value = data.alarm_time;
  }
  const ab = document.getElementById('alarm-toggle-btn');
  ab.innerText = data.alarm_enabled ? 'Disable' : 'Enable';
  ab.className = 'btn ' + (data.alarm_enabled ? 'success' : '');

  if (document.activeElement !== document.getElementById('lan-server')) {
    document.getElementById('lan-server').value = data.lan_server || '';
  }

  const st = document.getElementById('lan-status');
  if (!st.innerHTML.includes('✓') && !st.innerHTML.includes('⏳') && !st.innerHTML.includes('⚠')) {
    st.innerText = 'Indexed: ' + (data.lan_count || 0) + ' files on ' + (data.lan_server || '');
  }

  document.getElementById('ringing-box').style.display = data.alarm_active ? 'flex' : 'none';

  // Render station list if catalog changed or empty
  if (currentCat !== data.catalog || document.getElementById('stations-box').children.length === 0) {
    currentCat = data.catalog;
    let html = '';
    data.stations.forEach((st, i) => {
      const isActive = (i === data.station_idx && data.audio_running && data.media_type === 0);
      html += `<div class="station-item ${isActive ? 'active' : ''}" onclick="playStation(${i})">
        <span>${i+1}. ${st.name}</span>
        <span style="font-size:0.75rem;color:var(--sub)">${isActive ? '▶ PLAYING' : 'Tap to play'}</span>
      </div>`;
    });
    document.getElementById('stations-box').innerHTML = html;
  }

  // Render LAN media file list
  if (data.lan_files && (lastLanCount !== data.lan_count || document.getElementById('lan-files-box').children.length === 0)) {
    lastLanCount = data.lan_count;
    let lanHtml = '';
    data.lan_files.forEach((f, i) => {
      const isActive = (i === data.lan_track_idx && data.audio_running && data.media_type === 1);
      lanHtml += `<div class="station-item ${isActive ? 'active' : ''}" onclick="playLanTrack(${i})">
        <span>${i+1}. ${f.name}</span>
        <span style="font-size:0.75rem;color:var(--sub)">${isActive ? '▶ PLAYING' : 'Tap to play'}</span>
      </div>`;
    });
    document.getElementById('lan-files-box').innerHTML = lanHtml;
  } else if (!data.lan_files || data.lan_files.length === 0) {
    document.getElementById('lan-files-box').innerHTML = '<div style="font-size:0.8rem;color:var(--sub);padding:8px">No media files indexed yet. Tap Connect or Fetch above.</div>';
  }
}

let imuEventSource = null;
let autoRotateEnabled = true;

function toggleAutoRotate() {
  autoRotateEnabled = !autoRotateEnabled;
  fetch('/api/imu?autorotate=' + (autoRotateEnabled ? 1 : 0), { method: 'POST' })
    .then(r => r.json())
    .then(updateImuUI)
    .catch(console.error);
}

function updateImuUI(data) {
  if (!data || !data.available) {
    document.getElementById('imu-state').innerText = 'Offline';
    return;
  }
  document.getElementById('imu-state').innerText = data.orientation + (data.is_inverted ? ' (180°)' : ' (0°)');
  document.getElementById('imu-accel').innerText = `${data.ax.toFixed(2)}, ${data.ay.toFixed(2)}, ${data.az.toFixed(2)}`;
  document.getElementById('imu-gyro').innerText = `${data.gx.toFixed(1)}, ${data.gy.toFixed(1)}, ${data.gz.toFixed(1)}`;
  document.getElementById('imu-temp').innerText = `${data.temp_c ? data.temp_c.toFixed(1) : (data.temp ? data.temp.toFixed(1) : '--')} °C`;
  
  autoRotateEnabled = data.autorotate;
  const btn = document.getElementById('imu-autorot-btn');
  if (btn) {
    btn.className = 'btn ' + (data.autorotate ? 'primary' : '');
    btn.innerText = 'Auto-Rotate: ' + (data.autorotate ? 'ON' : 'OFF');
  }
  document.getElementById('imu-rot-0').className = 'btn' + (!data.is_inverted ? ' active' : '');
  document.getElementById('imu-rot-180').className = 'btn' + (data.is_inverted ? ' active' : '');
}

function pollImu() {
  if (imuEventSource) return; // SSE is streaming
  fetch('/api/imu')
    .then(r => r.json())
    .then(updateImuUI)
    .catch(console.error);
}

function toggleImuLiveStream() {
  const btn = document.getElementById('imu-stream-btn');
  const status = document.getElementById('imu-stream-status');
  if (imuEventSource) {
    imuEventSource.close();
    imuEventSource = null;
    btn.innerText = '📡 Live Stream';
    btn.className = 'btn';
    status.innerText = 'Stream stopped.';
  } else {
    btn.innerText = '⏹ Stop Stream';
    btn.className = 'btn danger';
    status.innerText = 'Live 10Hz SSE stream connected.';
    imuEventSource = new EventSource('/api/imu/stream');
    imuEventSource.onmessage = function(e) {
      try {
        const d = JSON.parse(e.data);
        d.available = true;
        d.orientation = d.state;
        d.is_inverted = d.inverted;
        d.autorotate = autoRotateEnabled;
        updateImuUI(d);
      } catch (err) { console.error(err); }
    };
    imuEventSource.onerror = function() {
      if (imuEventSource) {
        imuEventSource.close();
        imuEventSource = null;
      }
      btn.innerText = '📡 Live Stream';
      btn.className = 'btn';
      status.innerText = 'Stream ended / closed.';
    };
  }
}

let currentBattPct = 100;
let otaReleases = [
  { ver: 'v1.2.6', type: 'PATCH', date: 'Oct 12, 2026', size: '6.03 MB', notes: '• 10s breathing cycle (5s black, 2s hold high).\n• Wi-Fi power down in clock mode.\n• Real-time battery telemetry beacon.', url: 'https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.2.6.bin' },
  { ver: 'v1.2.5', type: 'PATCH', date: 'Oct 10, 2026', size: '6.02 MB', notes: '• AP3032 boost rail fix on EXIO1.\n• QMI8658 IMU orientation calibration.\n• Clock face timeout guard.', url: 'https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.2.5.bin' },
  { ver: 'v1.2.0', type: 'MINOR', date: 'Oct 05, 2026', size: '5.98 MB', notes: '• Web Remote controller with D-pad.\n• 10Hz IMU live stream.\n• LAN MP3 indexer & player.', url: 'https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.2.0.bin' },
  { ver: 'v1.1.0', type: 'MINOR', date: 'Sep 28, 2026', size: '5.85 MB', notes: '• ES7210 dual I2S mic driver.\n• Groq Whisper voice assistant.\n• Acoustic echo cancellation.', url: 'https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.1.0.bin' },
  { ver: 'v2.0.0', type: 'MAJOR', date: 'Nov 01, 2026', size: '6.20 MB', notes: '• FreeRTOS SMP architecture.\n• LittleFS asset filesystem.\n• Multi-repo rollback watchdog.\n• Spirit Level app.', url: 'https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v2.0.0.bin' },
  { ver: 'v1.0.0', type: 'MAJOR', date: 'Sep 01, 2026', size: '5.70 MB', notes: '• Initial commercial release.\n• Internet Radio, Clock, Alarm, Weather.', url: 'https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-v1.0.0.bin' }
];

function checkBatterySafe() {
  if (currentBattPct < 70) {
    alert('⚠️ Battery is at ' + currentBattPct + '%. Minimum 70% required to prevent corruption during firmware update. Please connect USB charger.');
    return false;
  }
  return true;
}

function onReleaseChanged() {
  const sel = document.getElementById('ota-release-select');
  if (!sel) return;
  const idx = parseInt(sel.value, 10);
  const r = otaReleases[idx];
  if (r) {
    document.getElementById('ota-release-notes').innerText = r.notes + '\n\nFile Size: ' + r.size + ' • Date: ' + r.date;
  }
}

function onRepoChanged() {
  const repo = document.getElementById('ota-repo-select').value;
  if (repo === 'official') {
    document.getElementById('repo-dram').innerText = '182 KB';
    document.getElementById('repo-psram').innerText = '4.8 MB';
    document.getElementById('repo-clock-batt').innerText = '14.5 Hours';
    document.getElementById('repo-active-batt').innerText = '5.2 Hours';
  }
}

function flashSelectedRelease() {
  if (!checkBatterySafe()) return;
  const sel = document.getElementById('ota-release-select');
  const idx = parseInt(sel.value, 10);
  const r = otaReleases[idx];
  if (r) streamFirmwareFromUrl(r.url, r.ver);
}

function flashCustomVersion() {
  if (!checkBatterySafe()) return;
  const maj = document.getElementById('v-maj').value || 1;
  const min = document.getElementById('v-min').value || 0;
  const pat = document.getElementById('v-pat').value || 0;
  const ver = 'v' + maj + '.' + min + '.' + pat;
  const url = 'https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/bin/firmware-' + ver + '.bin';
  streamFirmwareFromUrl(url, ver);
}

function streamFirmwareFromUrl(url, verName) {
  const box = document.getElementById('ota-progress-box');
  const bar = document.getElementById('ota-progress-bar');
  const status = document.getElementById('ota-status-text');
  const btn = document.getElementById('ota-github-btn');
  if (btn) btn.disabled = true;
  box.style.display = 'block';
  bar.style.width = '10%';
  bar.style.background = '#00E5FF';
  status.innerText = 'Downloading ' + verName + ' from repository...';

  fetch(url)
    .then(r => {
      if (!r.ok) throw new Error('HTTP ' + r.status + ' downloading firmware');
      return r.blob();
    })
    .then(blob => {
      bar.style.width = '30%';
      status.innerText = 'Streaming ' + verName + ' (' + Math.round(blob.size/1024) + ' KB) to TuneBar over Wi-Fi...';
      const formData = new FormData();
      formData.append('firmware', blob, 'firmware-' + verName + '.bin');

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/update', true);
      xhr.upload.onprogress = function(e) {
        if (e.lengthComputable) {
          const pct = Math.round((e.loaded / e.total) * 100);
          bar.style.width = pct + '%';
          status.innerText = 'Flashing: ' + pct + '% (' + Math.round(e.loaded/1024) + ' / ' + Math.round(e.total/1024) + ' KB)';
        }
      };
      xhr.onload = function() {
        if (xhr.status === 200) {
          bar.style.width = '100%';
          bar.style.background = '#00FF88';
          status.innerText = '✅ Update to ' + verName + ' successful! Device rebooting...';
        } else {
          bar.style.background = '#F44336';
          status.innerText = '❌ Update failed: ' + xhr.responseText;
          if (btn) btn.disabled = false;
        }
      };
      xhr.onerror = function() {
        bar.style.background = '#F44336';
        status.innerText = '❌ Network error during firmware stream';
        if (btn) btn.disabled = false;
      };
      xhr.send(formData);
    })
    .catch(err => {
      bar.style.background = '#F44336';
      status.innerText = '❌ Download error: ' + err.message;
      if (btn) btn.disabled = false;
    });
}

function uploadFirmware() {
  if (!checkBatterySafe()) return;
  const fileInput = document.getElementById('ota-file-input');
  if (!fileInput.files || fileInput.files.length === 0) {
    alert('Please select a firmware.bin file first.');
    return;
  }
  const file = fileInput.files[0];
  const btn = document.getElementById('ota-upload-btn');
  const box = document.getElementById('ota-progress-box');
  const bar = document.getElementById('ota-progress-bar');
  const status = document.getElementById('ota-status-text');

  btn.disabled = true;
  box.style.display = 'block';
  bar.style.width = '0%';
  bar.style.background = '#00E5FF';
  status.innerText = 'Uploading ' + file.name + ' (' + Math.round(file.size / 1024) + ' KB)...';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/update', true);

  xhr.upload.onprogress = function(e) {
    if (e.lengthComputable) {
      const pct = Math.round((e.loaded / e.total) * 100);
      bar.style.width = pct + '%';
      status.innerText = 'Uploading: ' + pct + '% (' + Math.round(e.loaded/1024) + ' / ' + Math.round(e.total/1024) + ' KB)';
    }
  };

  xhr.onload = function() {
    if (xhr.status === 200) {
      bar.style.width = '100%';
      bar.style.background = '#00FF88';
      status.innerText = '✅ Update Successful! Rebooting...';
    } else {
      bar.style.background = '#F44336';
      status.innerText = '❌ Update Failed: ' + xhr.responseText;
      btn.disabled = false;
    }
  };

  xhr.onerror = function() {
    bar.style.background = '#F44336';
    status.innerText = '❌ Network error during firmware upload';
    btn.disabled = false;
  };

  const formData = new FormData();
  formData.append('firmware', file, file.name);
  xhr.send(formData);
}

function refresh() {
  fetch('/api/status')
    .then(r => r.json())
    .then(refreshData)
    .catch(console.error);
  pollImu();
}

setInterval(refresh, 2000);
refresh();
pollImu();
</script>
</body>
</html>)rawliteral";

static void handle_root() {
    record_user_interaction();
    server.send_P(200, "text/html", HTML_PAGE);
}

static void handle_status() {
    float volt = 0.0f; uint8_t pct = 0;
    getBatteryStatus(&volt, &pct);

    uint8_t a_hr = 0, a_min = 0;
    alarm_get_time(&a_hr, &a_min);
    char alarm_str[8];
    snprintf(alarm_str, sizeof(alarm_str), "%02d:%02d", a_hr, a_min);

    size_t dram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t dram_total = 318 * 1024;
    uint8_t dram_used_pct = (dram_free < dram_total) ? (uint8_t)((dram_total - dram_free) * 100 / dram_total) : 0;
    uint32_t uptime_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    // Build JSON buffer in PSRAM (8KB)
    const size_t json_sz = 8192;
    char *buf = (char *)heap_caps_malloc(json_sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        server.send(500, "application/json", "{\"error\":\"out_of_mem\"}");
        return;
    }

    int offset = snprintf(buf, json_sz,
        "{\"active_screen\":\"%s\",\"clock_active\":%d,\"bl_off\":%d,\"bl_state\":%d,\"vol\":%d,"
        "\"batt_v\":%.2f,\"batt_pct\":%d,\"heap\":%u,\"psram\":%u,"
        "\"dram_free\":%u,\"dram_pct\":%d,\"uptime_s\":%u,"
        "\"rssi\":%d,\"audio_running\":%d,\"alarm_enabled\":%d,"
        "\"alarm_time\":\"%s\",\"alarm_active\":%d,\"catalog\":%d,"
        "\"catalog_name\":\"%s\",\"radio_lang\":%d,\"radio_lang_code\":\"%s\",\"radio_lang_name\":\"%s\","
        "\"station_idx\":%d,\"station_name\":\"%s\","
        "\"media_type\":%d,\"lan_server\":\"%s\",\"lan_count\":%d,\"lan_track_idx\":%d,"
        "\"telem_table\":\"%s\",\"telem_url\":\"https://autobots.my.to/telemetry.php?table=%s\","
        "\"telem_endpoint\":\"%s\",\"telem_interval\":%u,\"ai_url\":\"%s\","
        "\"consent\":%d,\"consent_decided\":%d,\"custom_ini\":%d,\"stations\":[",
        lvgl_port_get_active_screen_name(),
        (int)clock_face_active, (int)BL_OFF, (int)backlight_state,
        (int)audio.getVolume(), volt, pct,
        (unsigned int)dram_free, (unsigned int)ESP.getFreePsram(),
        (unsigned int)(dram_free / 1024), (int)dram_used_pct, uptime_s,
        (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0,
        audio.isRunning() ? 1 : 0,
        alarm_is_enabled() ? 1 : 0,
        alarm_str,
        alarm_is_active() ? 1 : 0,
        (int)currentRadioCatalog,
        getRadioCatalogName(currentRadioCatalog),
        (int)currentRadioLang,
        getRadioLanguageCode(currentRadioLang),
        getRadioLanguageName(currentRadioLang),
        (int)stationIndex,
        (stationIndex < stationListLength) ? stations[stationIndex].name : "",
        (int)mediaType,
        lan_get_server(),
        lan_get_file_count(),
        lan_track_idx,
        secrets_get_telemetry_table(),
        secrets_get_telemetry_table(),
        secrets_get_telemetry_endpoint(),
        (unsigned)secrets_get_telemetry_interval(),
        secrets_get_ai_url(),
        secrets_has_cloud_consent() ? 1 : 0,
        secrets_has_decided_consent() ? 1 : 0,
        secrets_has_custom_ini() ? 1 : 0
    );

    for (uint8_t i = 0; i < stationListLength; i++) {
        offset += snprintf(buf + offset, json_sz - offset,
            "%s{\"name\":\"%s\"}",
            (i > 0 ? "," : ""), stations[i].name);
        if (offset >= (int)json_sz - 200) break;
    }
    offset += snprintf(buf + offset, json_sz - offset, "],\"lan_files\":[");
    for (int i = 0; i < lan_get_file_count(); i++) {
        const LanFileEntry *f = lan_get_file(i);
        if (f) {
            offset += snprintf(buf + offset, json_sz - offset,
                "%s{\"name\":\"%s\"}",
                (i > 0 ? "," : ""), f->name);
            if (offset >= (int)json_sz - 50) break;
        }
    }
    snprintf(buf + offset, json_sz - offset, "]}");

    server.send(200, "application/json", buf);
    heap_caps_free(buf);
}

static void handle_vol() {
    if (server.hasArg("val")) {
        int v = server.arg("val").toInt();
        if (v < 0) v = 0;
        if (v > 21) v = 21;
        audioSetVolume(v);
    }
    handle_status();
}

static void handle_radio_lang() {
    if (server.hasArg("set")) {
        String lang_str = server.arg("set");
        uint8_t lang = RADIO_LANG_HI;
        if (lang_str.equalsIgnoreCase("hi")) lang = RADIO_LANG_HI;
        else if (lang_str.equalsIgnoreCase("en")) lang = RADIO_LANG_EN;
        else if (lang_str.equalsIgnoreCase("es")) lang = RADIO_LANG_ES;
        else if (lang_str.equalsIgnoreCase("cn")) lang = RADIO_LANG_CN;
        else if (lang_str.equalsIgnoreCase("de")) lang = RADIO_LANG_DE;
        else if (lang_str.equalsIgnoreCase("ja")) lang = RADIO_LANG_JA;
        switchRadioLanguage(lang);
    } else if (server.hasArg("id")) {
        int id = server.arg("id").toInt();
        if (id >= 0 && id <= 5) {
            switchRadioLanguage((uint8_t)id);
        }
    }
    handle_status();
}

static void handle_radio_catalog() {
    if (server.hasArg("id")) {
        int id = server.arg("id").toInt();
        switchRadioCatalog((uint8_t)id);
    }
    handle_status();
}

static void handle_radio_play() {
    if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        if (idx >= 0 && idx < stationListLength) {
            stationIndex = idx;
            audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
        }
    }
    handle_status();
}

static void handle_radio_resume() {
    if (stationIndex < stationListLength) {
        audioPlayHOST(stations[stationIndex].url, stations[stationIndex].name);
    }
    handle_status();
}

static void handle_radio_stop() {
    if (audio.isRunning()) {
        audio.stopSong();
    }
    handle_status();
}

static void handle_screen() {
    exit_clock_breathing();
    screenPowerOn();
    resetScreenOffTimer(NULL);
    UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
    xQueueSend(ui_status_queue, &ui_msg, 100);
    if (server.hasArg("action")) {
        String act = server.arg("action");
        if (lvgl_port_lock(500)) {
            if (act == "clock") {
                infoPageIndex = 0;
                updateInfoPanel(0);
                _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Info_screen_init);
            } else if (act == "weather") {
                infoPageIndex = 1;
                updateInfoPanel(1);
                _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Info_screen_init);
            } else if (act == "alarm") {
                infoPageIndex = 2;
                updateInfoPanel(2);
                _ui_screen_change(&ui_Screen_Info, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Info_screen_init);
            } else if (act == "menu") {
                _ui_screen_change(&ui_Screen_MainMenu, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_MainMenu_screen_init);
                if (server.hasArg("scroll")) {
                    int sc = server.arg("scroll").toInt();
                    lv_obj_scroll_to_x(ui_MainMenu_Panel_Menu, sc, LV_ANIM_OFF);
                }
            } else if (act == "settings") {
                _ui_screen_change(&ui_Screen_MainMenu, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_MainMenu_screen_init);
                _ui_flag_modify(ui_MainMenu_Button_closeConfig, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
                _ui_flag_modify(ui_MainMenu_Tabview_ConfigPanel, LV_OBJ_FLAG_HIDDEN, _UI_MODIFY_FLAG_REMOVE);
                if (server.hasArg("tab")) {
                    int tab_idx = server.arg("tab").toInt();
                    lv_tabview_set_act(ui_MainMenu_Tabview_ConfigPanel, tab_idx, LV_ANIM_OFF);
                }
            } else if (act == "utility") {
                _ui_screen_change(&ui_Screen_Utility, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Utility_screen_init);
                utilityMode(NULL);
            } else if (act == "power_profile") {
                _ui_screen_change(&ui_Screen_Utility, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Utility_screen_init);
                utilityMode(NULL);
                ui_power_profile_open();
                if (server.hasArg("scroll")) {
                    int sc = server.arg("scroll").toInt();
                    ui_power_profile_scroll_to((int16_t)sc);
                }
            } else if (act == "music" || act == "lan") {
                _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
                musicPlayerMode(NULL);
            } else if (act == "player" || act == "radio") {
                _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
                livestreamMode(NULL);
            } else if (act == "chat") {
                _ui_screen_change(&ui_Screen_Player, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen_Player_screen_init);
                chatBotMode(NULL);
            } else if (act == "touch") {
                on_clock_touch_or_button();
            }
            lvgl_port_unlock();
        }
    }
    handle_status();
}

static void handle_touch() {
    exit_clock_breathing();
    screenPowerOn();
    on_clock_touch_or_button();
    resetScreenOffTimer(NULL);

    if (server.hasArg("x") && server.hasArg("y")) {
        int x = server.arg("x").toInt();
        int y = server.arg("y").toInt();
        lvgl_port_inject_touch((int16_t)x, (int16_t)y, 150);
        server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"tap\"}");
        return;
    }
    if (server.hasArg("x1") && server.hasArg("y1") && server.hasArg("x2") && server.hasArg("y2")) {
        int x1 = server.arg("x1").toInt();
        int y1 = server.arg("y1").toInt();
        int x2 = server.arg("x2").toInt();
        int y2 = server.arg("y2").toInt();
        uint32_t dur = server.hasArg("dur") ? server.arg("dur").toInt() : 350;
        lvgl_port_inject_swipe((int16_t)x1, (int16_t)y1, (int16_t)x2, (int16_t)y2, dur);
        server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"swipe\"}");
        return;
    }
    if (server.hasArg("swipe")) {
        String dir = server.arg("swipe");
        if (dir == "up") {
            lvgl_port_inject_swipe(320, 150, 320, 20, 350);
        } else if (dir == "down") {
            lvgl_port_inject_swipe(320, 20, 320, 150, 350);
        } else if (dir == "left") {
            lvgl_port_inject_swipe(550, 86, 90, 86, 350);
        } else if (dir == "right") {
            lvgl_port_inject_swipe(90, 86, 550, 86, 350);
        }
        server.send(200, "application/json", "{\"status\":\"ok\",\"action\":\"swipe\"}");
        return;
    }
    handle_status();
}

static void handle_bl() {
    if (server.hasArg("state")) {
        int s = server.arg("state").toInt();
        if (s == 0) {
            backlight_state = 0;
            pref.begin("config", false); pref.putUChar("bl_state", 0); pref.end();
            setUpduty(LCD_BL_LOW);
        } else if (s == 1) {
            backlight_state = 1;
            pref.begin("config", false); pref.putUChar("bl_state", 1); pref.end();
            setUpduty(LCD_BL_MED);
        } else if (s == 2) {
            backlight_state = 2;
            pref.begin("config", false); pref.putUChar("bl_state", 2); pref.end();
            setUpduty(LCD_BL_HIGH);
        }
    }
    handle_status();
}

static void handle_alarm() {
    if (server.hasArg("enabled")) {
        alarm_set_enabled(server.arg("enabled").toInt() == 1);
    }
    if (server.hasArg("time")) {
        String t = server.arg("time");
        int c = t.indexOf(':');
        if (c > 0) {
            int h = t.substring(0, c).toInt();
            int m = t.substring(c + 1).toInt();
            alarm_set_time(h, m);
        }
    }
    if (lvgl_port_lock(200)) {
        alarm_ui_refresh();
        lvgl_port_unlock();
    }
    handle_status();
}

static void handle_alarm_stop() {
    alarm_stop();
    handle_status();
}

static void handle_alarm_test() {
    alarm_trigger();
    handle_status();
}

static void handle_telemetry() {
    if (server.hasArg("action")) {
        String act = server.arg("action");
        if (act == "snap") {
            telemetry_collect_snapshot();
        } else if (act == "flush") {
            telemetry_flush_batch();
        }
    }
    handle_status();
}

static void handle_consent() {
    if (server.hasArg("action")) {
        String act = server.arg("action");
        if (act == "accept") {
            secrets_set_cloud_consent(true);
            telemetry_collect_snapshot();
            telemetry_flush_batch();
        } else if (act == "decline") {
            secrets_set_cloud_consent(false);
        } else if (act == "reset") {
            secrets_reset_consent();
        }
    }
    handle_status();
}

static void handle_secrets() {
    if (server.method() == HTTP_POST) {
        String ai_url = server.hasArg("ai_url") ? server.arg("ai_url") : "";
        String telem_url = server.hasArg("telem_url") ? server.arg("telem_url") : "";
        String table = server.hasArg("table") ? server.arg("table") : "";
        uint32_t interval = server.hasArg("interval") ? (uint32_t)server.arg("interval").toInt() : 300;
        String pin = server.hasArg("pin") ? server.arg("pin") : "";
        bool consent = server.hasArg("consent") ? (server.arg("consent") == "1" || server.arg("consent") == "true") : true;

        if (server.hasArg("action") && server.arg("action") == "restore") {
            secrets_delete_ini_file();
            server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Restored firmware defaults\"}");
            return;
        }

        bool ok = secrets_write_ini_file(
            ai_url.length() ? ai_url.c_str() : secrets_get_ai_url(),
            telem_url.length() ? telem_url.c_str() : secrets_get_telemetry_endpoint(),
            table.length() ? table.c_str() : secrets_get_telemetry_table(),
            interval > 0 ? interval : 300,
            pin.length() ? pin.c_str() : secrets_get_master_pin(),
            consent
        );

        if (ok) {
            server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Secrets saved to /secrets.ini\"}");
        } else {
            server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to write /secrets.ini\"}");
        }
        return;
    }

    // GET: return current secrets configuration
    char json_buf[1024];
    snprintf(json_buf, sizeof(json_buf),
        "{\"ai_url\":\"%s\",\"telem_endpoint\":\"%s\",\"telem_table\":\"%s\",\"interval\":%u,"
        "\"custom_ini\":%d,\"consent\":%d,\"consent_decided\":%d,\"pin\":\"%s\"}",
        secrets_get_ai_url(),
        secrets_get_telemetry_endpoint(),
        secrets_get_telemetry_table(),
        (unsigned)secrets_get_telemetry_interval(),
        secrets_has_custom_ini() ? 1 : 0,
        secrets_has_cloud_consent() ? 1 : 0,
        secrets_has_decided_consent() ? 1 : 0,
        secrets_get_master_pin()
    );
    server.send(200, "application/json", json_buf);
}

static void handle_ask() {
    if (server.hasArg("q")) {
        String q = server.arg("q");
        log_i("[ASSISTANT] Prompt received: %s", q.c_str());
        q.trim();
        q.replace(" ", "+");
        mediaType = 2; // AI Assistant mode
        String bridgeUrl = String(secrets_get_ai_url()) + "?device=" + secrets_get_telemetry_table() + "&q=" + q;
        audioPlayHOST(bridgeUrl.c_str(), "AI Assistant");
    }
    handle_status();
}

static void handle_lan_server() {
    if (server.hasArg("srv")) {
        String srv = server.arg("srv");
        srv.trim();
        lan_set_server(srv.c_str());
    }
    handle_status();
}

static void handle_lan_fetch() {
    lan_fetch_files();
    handle_status();
}

static void handle_lan_play() {
    if (server.hasArg("idx")) {
        int idx = server.arg("idx").toInt();
        lan_play(idx);
    }
    handle_status();
}

static void handle_screenshot() {
    exit_clock_breathing();
    screenPowerOn();
    resetScreenOffTimer(NULL);
    UIStatusPayload ui_msg = {.type = STATUS_SCREEN_UNLOCK};
    xQueueSend(ui_status_queue, &ui_msg, 100);
    lvgl_port_take_screenshot();
    const uint16_t *fb = lvgl_port_get_framebuffer();
    if (!fb) {
        server.send(500, "text/plain", "Framebuffer unavailable");
        return;
    }

    const uint32_t width = WAVESHARE_349_LCD_H_RES;
    const uint32_t height = WAVESHARE_349_LCD_V_RES;
    const uint32_t row_stride = ((width * 3 + 3) / 4) * 4;
    const uint32_t image_size = row_stride * height;
    const uint32_t file_size = 54 + image_size;

    uint8_t bmp_hdr[54];
    memset(bmp_hdr, 0, sizeof(bmp_hdr));
    bmp_hdr[0] = 'B'; bmp_hdr[1] = 'M';
    bmp_hdr[2] = (uint8_t)(file_size);
    bmp_hdr[3] = (uint8_t)(file_size >> 8);
    bmp_hdr[4] = (uint8_t)(file_size >> 16);
    bmp_hdr[5] = (uint8_t)(file_size >> 24);
    bmp_hdr[10] = 54;
    bmp_hdr[14] = 40;
    bmp_hdr[18] = (uint8_t)(width);
    bmp_hdr[19] = (uint8_t)(width >> 8);
    bmp_hdr[20] = (uint8_t)(width >> 16);
    bmp_hdr[21] = (uint8_t)(width >> 24);
    bmp_hdr[22] = (uint8_t)(height);
    bmp_hdr[23] = (uint8_t)(height >> 8);
    bmp_hdr[24] = (uint8_t)(height >> 16);
    bmp_hdr[25] = (uint8_t)(height >> 24);
    bmp_hdr[26] = 1;
    bmp_hdr[28] = 24;
    bmp_hdr[34] = (uint8_t)(image_size);
    bmp_hdr[35] = (uint8_t)(image_size >> 8);
    bmp_hdr[36] = (uint8_t)(image_size >> 16);
    bmp_hdr[37] = (uint8_t)(image_size >> 24);

    WiFiClient client = server.client();
    server.setContentLength(file_size);
    server.send(200, "image/bmp", "");
    client.write(bmp_hdr, 54);

    uint8_t *row_buf = (uint8_t *)heap_caps_malloc(row_stride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row_buf) {
        server.send(500, "text/plain", "Out of memory for screenshot buffer");
        return;
    }
    const bool is_flipped = lvgl_port_is_inverted_landscape();
    for (int y = (int)height - 1; y >= 0; y--) {
        memset(row_buf, 0, row_stride);
        int src_y = is_flipped ? ((int)height - 1 - y) : y;
        const uint16_t *src_row = fb + (src_y * width);
        uint8_t *dst = row_buf;
        for (uint32_t x = 0; x < width; x++) {
            uint32_t src_x = is_flipped ? (width - 1 - x) : x;
            uint16_t raw = src_row[src_x];
#if LV_COLOR_16_SWAP
            uint16_t p = (uint16_t)((raw >> 8) | (raw << 8));
#else
            uint16_t p = raw;
#endif
            uint8_t r = (p >> 11) & 0x1F;
            uint8_t g = (p >> 5) & 0x3F;
            uint8_t b = p & 0x1F;
            *dst++ = (b * 255) / 31;
            *dst++ = (g * 255) / 63;
            *dst++ = (r * 255) / 31;
        }
        client.write(row_buf, row_stride);
    }
    heap_caps_free(row_buf);
}

static void handle_rec_wav() {
    if (LittleFS.exists("/rec.wav")) {
        File f = LittleFS.open("/rec.wav", "r");
        if (f) {
            server.streamFile(f, "audio/wav");
            f.close();
            return;
        }
    }
    server.send(404, "text/plain", "No recorded audio found.");
}

static void handle_rec_action() {
    String act = server.hasArg("action") ? server.arg("action") : "";
    if (act == "start" || act == "rec") {
        start_ai_voice_recording();
        server.send(200, "application/json", "{\"status\":\"recording\"}");
    } else if (act == "stop") {
        stop_ai_voice_recording_and_process();
        server.send(200, "application/json", "{\"status\":\"playback\"}");
    } else if (act == "play") {
        mediaType = 2;
        speaker.setVolume(90);
        audio.setVolume(21);
        audio.connecttoFS(LittleFS, "/rec.wav");
        server.send(200, "application/json", "{\"status\":\"playing\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"invalid action\"}");
    }
}

static void handle_imu() {
    if (server.hasArg("autorotate")) {
        String ar = server.arg("autorotate");
        if (ar == "1" || ar == "true" || ar == "on") {
            imu_sensor_set_autorotate_enabled(true);
        } else if (ar == "0" || ar == "false" || ar == "off") {
            imu_sensor_set_autorotate_enabled(false);
        }
    }
    if (server.hasArg("rotate")) {
        int r = server.arg("rotate").toInt();
        if (r == 180) {
            imu_sensor_force_orientation(true);
        } else if (r == 0) {
            imu_sensor_force_orientation(false);
        }
    }

    imu_reading_t imu;
    bool ok = imu_sensor_get_latest(&imu);
    char buf[384];
    if (ok) {
        snprintf(buf, sizeof(buf),
            "{\"available\":true,\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,"
            "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"temp_c\":%.1f,"
            "\"is_inverted\":%s,\"orientation\":\"%s\",\"autorotate\":%s}",
            imu.ax, imu.ay, imu.az,
            imu.gx, imu.gy, imu.gz, imu.temp_c,
            imu.is_inverted ? "true" : "false",
            imu.orientation_str,
            imu_sensor_is_autorotate_enabled() ? "true" : "false"
        );
    } else {
        snprintf(buf, sizeof(buf), "{\"available\":false,\"error\":\"imu_unavailable\"}");
    }
    server.send(200, "application/json", buf);
}

static void handle_imu_stream() {
    WiFiClient client = server.client();
    server.sendHeader("Cache-Control", "no-cache");
    server.sendHeader("Connection", "keep-alive");
    server.send(200, "text/event-stream", "");

    // Stream live sensor updates for up to 30 seconds or until client disconnects
    uint32_t start_ms = millis();
    while (client.connected() && (millis() - start_ms < 30000)) {
        imu_reading_t imu;
        if (imu_sensor_get_latest(&imu)) {
            char chunk[256];
            int len = snprintf(chunk, sizeof(chunk),
                "data: {\"ax\":%.3f,\"ay\":%.3f,\"az\":%.3f,\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,\"temp\":%.1f,\"inverted\":%s,\"state\":\"%s\"}\n\n",
                imu.ax, imu.ay, imu.az, imu.gx, imu.gy, imu.gz, imu.temp_c,
                imu.is_inverted ? "true" : "false", imu.orientation_str);
            client.write((const uint8_t *)chunk, len);
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // 10Hz live stream
    }
}

static bool s_ota_active = false;
static size_t s_ota_bytes_last = 0;
static uint32_t s_ota_ms_last = 0;

static void handle_ota_repos(void) {
    const char *json = "{\"active_repo\":\"official\",\"approved_repos\":["
      "{\"id\":\"official\",\"name\":\"TuneBar Official (rahulraj80)\",\"author\":\"rahulraj80\","
      "\"catalog_url\":\"https://raw.githubusercontent.com/rahulraj80/TuneBar/main/releases/firmware_catalog.json\","
      "\"repo_url\":\"https://github.com/rahulraj80/TuneBar\","
      "\"description\":\"Official firmware with HiFi radio, LAN streaming, Nixie clock, vector UI & IMU auto-rotation\","
      "\"metrics\":{\"dram_free_avg_kb\":182,\"psram_free_avg_mb\":4.8,\"battery_life_clock_hours\":14.5,\"battery_life_active_hours\":5.2}},"
      "{\"id\":\"upstream\",\"name\":\"TuneBar Upstream (VaAndCob)\",\"author\":\"VaAndCob\","
      "\"catalog_url\":\"https://raw.githubusercontent.com/VaAndCob/TuneBar/main/releases/firmware_catalog.json\","
      "\"repo_url\":\"https://github.com/VaAndCob/TuneBar\","
      "\"description\":\"Original upstream baseline repository\","
      "\"metrics\":{\"dram_free_avg_kb\":160,\"psram_free_avg_mb\":4.5,\"battery_life_clock_hours\":12.0,\"battery_life_active_hours\":4.5}}"
      "]}";
    server.send(200, "application/json", json);
}

static void handle_ota_catalog(void) {
    String json = get_generated_catalog_json();
    server.send(200, "application/json", json);
}

static void handle_update_done(void) {
    uint8_t batt_pct = 0; float batt_v = 0.0f;
    if (!ota_check_battery_allowed(&batt_pct, &batt_v)) {
        server.sendHeader("Connection", "close");
        server.send(403, "application/json", "{\"status\":\"blocked\",\"reason\":\"battery_too_low\",\"battery_pct\":" + String(batt_pct) + ",\"required_pct\":70,\"message\":\"OTA blocked: Battery below 70% (" + String(batt_pct) + "%). Connect USB charger.\"}");
        return;
    }

    bool ok = !Update.hasError();
    server.sendHeader("Connection", "close");
    if (ok) {
        server.send(200, "application/json", "{\"status\":\"ok\",\"version\":\"" TUNEBAR_VERSION_STR "\",\"build\":" + String(TUNEBAR_BUILD_NUM) + ",\"message\":\"Update successful! Rebooting now...\"}");
        log_i("[OTA] Firmware update successful! Restarting in 2s...");
        delay(2000);
        ESP.restart();
    } else {
        String err = "{\"status\":\"error\",\"message\":\"Update failed (code " + String(Update.getError()) + ")\"}";
        server.send(500, "application/json", err);
        log_e("[OTA] Firmware update failed with error code: %d", Update.getError());
    }
}

static size_t s_ota_total_expected = 0;

static void handle_update_upload(void) {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
#ifdef LAUNCHER_BUILD
        // The next OTA slot may hold another app the Launcher installed.
        log_w("[OTA] Disabled in the Launcher build; update DeskBar from the Launcher");
        s_ota_active = false;
        return;
#endif
        log_i("[OTA] Starting firmware upload: %s", upload.filename.c_str());

        int clen = server.header("Content-Length").toInt();
        if (clen > 100000) {
            s_ota_total_expected = (size_t)clen;
            log_i("[OTA] Dynamic Content-Length parsed: %u bytes", (unsigned)s_ota_total_expected);
        } else {
            s_ota_total_expected = 6350000ULL;
            log_w("[OTA] Content-Length missing/small (%d), using fallback %u bytes", clen, (unsigned)s_ota_total_expected);
        }

        uint8_t batt_pct = 0; float batt_v = 0.0f;
        if (!ota_check_battery_allowed(&batt_pct, &batt_v)) {
            log_w("[OTA] BLOCKED: Battery is too low (%u%%, %.2fV)", (unsigned)batt_pct, batt_v);
            ota_ui_show_blocked_battery(batt_pct);
            s_ota_active = false;
            return;
        }

        s_ota_active = true;
        s_ota_bytes_last = 0;
        s_ota_ms_last = millis();

        ota_pre_update_teardown();
        ota_ui_show_neon_progress(upload.filename.c_str());

        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
            log_e("[OTA] Update.begin failed!");
            s_ota_active = false;
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (!s_ota_active) return;

        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
            log_e("[OTA] Update.write failed!");
        } else {
            uint32_t now = millis();
            if (now - s_ota_ms_last >= 250) {
                uint32_t dt_ms = now - s_ota_ms_last;
                size_t dbytes = upload.totalSize - s_ota_bytes_last;
                uint32_t speed_kb = (dt_ms > 0) ? (uint32_t)((dbytes * 1000ULL) / (dt_ms * 1024ULL)) : 0;
                s_ota_ms_last = now;
                s_ota_bytes_last = upload.totalSize;

                size_t total_exp = (s_ota_total_expected > 0) ? s_ota_total_expected : 6350000ULL;
                uint8_t pct = (uint8_t)((upload.totalSize * 100ULL) / total_exp);
                if (pct > 99) pct = 99;
                ota_ui_update_progress(pct, upload.totalSize, total_exp, speed_kb);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!s_ota_active) return;

        if (Update.end(true)) {
            log_i("[OTA] Firmware upload completed! Total size: %u bytes", (unsigned)upload.totalSize);
            ota_ui_update_progress(100, upload.totalSize, upload.totalSize, 0);
            ota_ui_show_complete(TUNEBAR_VERSION_STR);
        } else {
            Update.printError(Serial);
            log_e("[OTA] Update.end failed!");
        }
        s_ota_active = false;
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        Update.end();
        s_ota_active = false;
        log_w("[OTA] Firmware upload aborted by client");
    }
}

static void handle_power_profile_get() {
    const power_profile_t *pwr = power_profile_get();
    float volts[BATTERY_CURVE_PTS];
    uint8_t pcts[BATTERY_CURVE_PTS];
    uint8_t count = 0;
    battery_get_curve_points(volts, pcts, &count);

    String json = "{";
    json += "\"status\":\"ok\",";
    json += "\"bl_start\":" + String(pwr->bl_start) + ",";
    json += "\"bl_end\":" + String(pwr->bl_end) + ",";
    json += "\"bl_black\":" + String(pwr->bl_black) + ",";
    json += "\"ramp_up_ms\":" + String(pwr->ramp_up_ms) + ",";
    json += "\"hold_high_ms\":" + String(pwr->hold_high_ms) + ",";
    json += "\"ramp_down_ms\":" + String(pwr->ramp_down_ms) + ",";
    json += "\"hold_dark_ms\":" + String(pwr->hold_dark_ms) + ",";
    json += "\"inactivity_tier\":" + String(pwr->inactivity_tier ? "true" : "false") + ",";
    json += "\"gate_wifi\":" + String(pwr->gate_wifi ? "true" : "false") + ",";
    json += "\"gate_audio\":" + String(pwr->gate_audio ? "true" : "false") + ",";
    json += "\"gate_sensors\":" + String(pwr->gate_sensors ? "true" : "false") + ",";
    json += "\"cal_source\":\"" + String(battery_get_cal_source()) + "\",";
    json += "\"cal_source_label\":\"" + String(battery_get_cal_source_label()) + "\",";
    json += "\"cal_time\":\"" + String(battery_get_cal_timestamp()) + "\",";
    json += "\"volts\":[";
    for (int i = 0; i < count; i++) {
        json += String(volts[i], 3) + (i < count - 1 ? "," : "");
    }
    json += "],\"pcts\":[";
    for (int i = 0; i < count; i++) {
        json += String(pcts[i]) + (i < count - 1 ? "," : "");
    }
    json += "]}";

    server.send(200, "application/json", json);
}

static void handle_power_profile_post() {
    power_profile_t pwr = *power_profile_get();

    if (server.hasArg("plain")) {
        DynamicJsonDocument doc(1024);
        DeserializationError err = deserializeJson(doc, server.arg("plain"));
        if (!err) {
            if (doc.containsKey("bl_start")) pwr.bl_start = doc["bl_start"];
            if (doc.containsKey("bl_end")) pwr.bl_end = doc["bl_end"];
            if (doc.containsKey("bl_black")) pwr.bl_black = doc["bl_black"];
            if (doc.containsKey("ramp_up_ms")) pwr.ramp_up_ms = doc["ramp_up_ms"];
            if (doc.containsKey("hold_high_ms")) pwr.hold_high_ms = doc["hold_high_ms"];
            if (doc.containsKey("ramp_down_ms")) pwr.ramp_down_ms = doc["ramp_down_ms"];
            if (doc.containsKey("hold_dark_ms")) pwr.hold_dark_ms = doc["hold_dark_ms"];
            if (doc.containsKey("inactivity_tier")) pwr.inactivity_tier = doc["inactivity_tier"];
            if (doc.containsKey("gate_wifi")) pwr.gate_wifi = doc["gate_wifi"];
            if (doc.containsKey("gate_audio")) pwr.gate_audio = doc["gate_audio"];
            if (doc.containsKey("gate_sensors")) pwr.gate_sensors = doc["gate_sensors"];
        }
    } else {
        if (server.hasArg("bl_start")) pwr.bl_start = server.arg("bl_start").toInt();
        if (server.hasArg("bl_end")) pwr.bl_end = server.arg("bl_end").toInt();
        if (server.hasArg("bl_black")) pwr.bl_black = server.arg("bl_black").toInt();
        if (server.hasArg("ramp_up_ms")) pwr.ramp_up_ms = server.arg("ramp_up_ms").toInt();
        if (server.hasArg("hold_high_ms")) pwr.hold_high_ms = server.arg("hold_high_ms").toInt();
        if (server.hasArg("ramp_down_ms")) pwr.ramp_down_ms = server.arg("ramp_down_ms").toInt();
        if (server.hasArg("hold_dark_ms")) pwr.hold_dark_ms = server.arg("hold_dark_ms").toInt();
        if (server.hasArg("inactivity_tier")) pwr.inactivity_tier = (server.arg("inactivity_tier") == "1" || server.arg("inactivity_tier") == "true");
        if (server.hasArg("gate_wifi")) pwr.gate_wifi = (server.arg("gate_wifi") == "1" || server.arg("gate_wifi") == "true");
        if (server.hasArg("gate_audio")) pwr.gate_audio = (server.arg("gate_audio") == "1" || server.arg("gate_audio") == "true");
        if (server.hasArg("gate_sensors")) pwr.gate_sensors = (server.arg("gate_sensors") == "1" || server.arg("gate_sensors") == "true");
    }

    power_profile_set(&pwr);
    handle_power_profile_get();
}

static void handle_battery_sync_cal() {
    bool ok = battery_sync_from_cloud(NULL);
    if (ok) {
        if (lvgl_port_lock(500)) {
            ui_power_profile_refresh();
            lvgl_port_unlock();
        }
        handle_power_profile_get();
    } else {
        server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"Failed to sync battery curve from cloud\"}");
    }
}

void web_remote_init(void) {
    if (server_running) return;

    const char *headerkeys[] = {"Content-Length", "User-Agent"};
    server.collectHeaders(headerkeys, 2);

    server.on("/", HTTP_GET, handle_root);
    server.on("/api/status", HTTP_GET, handle_status);
    server.on("/api/update", HTTP_POST, handle_update_done, handle_update_upload);
    server.on("/api/ota/repos", HTTP_GET, handle_ota_repos);
    server.on("/api/ota/catalog", HTTP_GET, handle_ota_catalog);
    server.on("/api/vol", HTTP_POST, handle_vol);
    server.on("/api/radio/lang", HTTP_GET, handle_radio_lang);
    server.on("/api/radio/lang", HTTP_POST, handle_radio_lang);
    server.on("/api/radio/catalog", HTTP_POST, handle_radio_catalog);
    server.on("/api/radio/play", HTTP_POST, handle_radio_play);
    server.on("/api/radio/resume", HTTP_POST, handle_radio_resume);
    server.on("/api/radio/stop", HTTP_POST, handle_radio_stop);
    server.on("/api/screen", HTTP_GET, handle_screen);
    server.on("/api/screen", HTTP_POST, handle_screen);
    server.on("/api/touch", HTTP_GET, handle_touch);
    server.on("/api/touch", HTTP_POST, handle_touch);
    server.on("/api/bl", HTTP_POST, handle_bl);
    server.on("/api/alarm", HTTP_GET, handle_alarm);
    server.on("/api/alarm", HTTP_POST, handle_alarm);
    server.on("/api/alarm/stop", HTTP_POST, handle_alarm_stop);
    server.on("/api/alarm/test", HTTP_POST, handle_alarm_test);
    server.on("/api/telemetry", HTTP_POST, handle_telemetry);
    server.on("/api/consent", HTTP_GET, handle_consent);
    server.on("/api/consent", HTTP_POST, handle_consent);
    server.on("/api/secrets", HTTP_GET, handle_secrets);
    server.on("/api/secrets", HTTP_POST, handle_secrets);
    server.on("/api/ask", HTTP_GET, handle_ask);
    server.on("/api/ask", HTTP_POST, handle_ask);
    server.on("/api/lan/server", HTTP_GET, handle_lan_server);
    server.on("/api/lan/server", HTTP_POST, handle_lan_server);
    server.on("/api/lan/fetch", HTTP_GET, handle_lan_fetch);
    server.on("/api/lan/fetch", HTTP_POST, handle_lan_fetch);
    server.on("/api/lan/play", HTTP_GET, handle_lan_play);
    server.on("/api/lan/play", HTTP_POST, handle_lan_play);
    server.on("/rec.wav", HTTP_GET, handle_rec_wav);
    server.on("/api/rec", HTTP_GET, handle_rec_action);
    server.on("/api/rec", HTTP_POST, handle_rec_action);
    server.on("/api/screenshot", HTTP_GET, handle_screenshot);
    server.on("/screenshot.bmp", HTTP_GET, handle_screenshot);
    server.on("/api/imu", HTTP_GET, handle_imu);
    server.on("/api/imu", HTTP_POST, handle_imu);
    server.on("/api/imu/stream", HTTP_GET, handle_imu_stream);
    server.on("/api/power_profile", HTTP_GET, handle_power_profile_get);
    server.on("/api/power_profile", HTTP_POST, handle_power_profile_post);
    server.on("/api/battery/sync_cal", HTTP_POST, handle_battery_sync_cal);

    server.enableCORS(true);
    server.begin();
    server_running = true;
    log_i("[WEB REMOTE] Server started on port 80. Accessible at http://%s/",
          WiFi.localIP().toString().c_str());
}

void web_remote_loop(void) {
    if (server_running) {
        server.handleClient();
    }
}
