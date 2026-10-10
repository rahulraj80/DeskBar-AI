#include "web_remote.h"
#include <WebServer.h>
#include <WiFi.h>
#include <Preferences.h>
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
#include "ui/screens/ui_Screen_Player.h"
#include "user_config.h"

extern Audio audio;
extern Preferences pref;
extern "C" void exit_clock_breathing(void);
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
    <div class="btn-group" style="margin-top:2px">
      <button class="btn" onclick="callApi('/api/telemetry?action=snap')">📸 Snap Telemetry</button>
      <button class="btn" onclick="callApi('/api/telemetry?action=flush')">☁ Flush Telemetry</button>
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
  document.getElementById('stat-wifi').innerText = data.rssi + ' dBm';
  document.getElementById('stat-screen').innerText = data.bl_off ? 'Locked/Off' : ('BL: ' + ['Low','Med','High'][data.bl_state]);
  document.getElementById('stat-boot').innerText = (data.boot_count || 1) + ' boots';
  document.getElementById('stat-uptime').innerText = 'Uptime: ' + Math.floor(data.uptime_s / 60) + 'm ' + (data.uptime_s % 60) + 's';

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

function refresh() {
  fetch('/api/status')
    .then(r => r.json())
    .then(refreshData)
    .catch(console.error);
}

setInterval(refresh, 2500);
refresh();
</script>
</body>
</html>)rawliteral";

static void handle_root() {
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
        "\"media_type\":%d,\"lan_server\":\"%s\",\"lan_count\":%d,\"lan_track_idx\":%d,\"stations\":[",
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
        lan_track_idx
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
    if (server.hasArg("swipe")) {
        String dir = server.arg("swipe");
        if (dir == "up") {
            lvgl_port_inject_touch(320, 140, 150);
        } else if (dir == "down") {
            lvgl_port_inject_touch(320, 20, 150);
        } else if (dir == "left") {
            lvgl_port_inject_touch(550, 86, 150);
        } else if (dir == "right") {
            lvgl_port_inject_touch(90, 86, 150);
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

static void handle_ask() {
    if (server.hasArg("q")) {
        String q = server.arg("q");
        log_i("[ASSISTANT] Prompt received: %s", q.c_str());
        q.trim();
        q.replace(" ", "+");
        mediaType = 2; // AI Assistant mode
        String bridgeUrl = String(AI_ASSISTANT_URL) + "?q=" + q;
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
    for (int y = (int)height - 1; y >= 0; y--) {
        memset(row_buf, 0, row_stride);
        const uint16_t *src_row = fb + (y * width);
        uint8_t *dst = row_buf;
        for (uint32_t x = 0; x < width; x++) {
            uint16_t raw = src_row[x];
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

void web_remote_init(void) {
    if (server_running) return;

    server.on("/", HTTP_GET, handle_root);
    server.on("/api/status", HTTP_GET, handle_status);
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
