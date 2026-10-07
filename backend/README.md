# 🚀 TuneBar Backend Server Deployment Guide

This directory contains standalone, self-hosted reference implementations for the two core services required by the TuneBar Advanced Edition firmware:

1. **AI Voice Assistant Service (`/ask.php` or `/api/ask`)** — `ai_assistant_server.py`
2. **Distributed Telemetry Ingest Service (`/myapi.php` or `/api/telemetry`)** — `telemetry_server.py`

---

## 1. AI Voice Assistant Backend Architecture

```
[ TuneBar ES7210 Mic ]
       │  (HTTPS POST /ask.php with WAV audio)
       ▼
┌────────────────────────────────────────────────────────┐
│               ai_assistant_server.py                   │
│                                                        │
│  1. Speech-to-Text: Groq Whisper Large V3 Turbo        │
│     - Latency: ~150-220 ms                             │
│     - Language forced to "en" to eliminate hallucination│
│                                                        │
│  2. LLM Reasoning: Llama 3.3 70B Versatile             │
│     - Latency: ~250-320 ms                             │
│     - Strict system prompt: 1-2 concise spoken sentences│
│                                                        │
│  3. Speech Synthesis: Microsoft Edge-TTS               │
│     - Voice: en-US-GuyNeural / en-US-JennyNeural       │
│     - Instant streaming chunk generation (zero cost)   │
└────────────────────────────────────────────────────────┘
       │  (Streaming audio/mpeg MP3 response + X-Query / X-Answer headers)
       ▼
[ TuneBar ES8311 DAC & Speaker ]
```

### Why We Chose This Stack
* **Groq Whisper Large V3 Turbo vs Self-Hosted / OpenAI**: Groq's custom LPU hardware processes audio files in under 200 ms. Self-hosted Whisper on typical CPU takes 2–4 seconds; OpenAI Whisper API takes 1.5–3 seconds. Low latency is critical for interactive conversation on an embedded device.
* **Llama 3.3 70B via Groq**: Delivers sub-300 ms time-to-first-token while maintaining high reasoning quality.
* **Edge-TTS vs Paid TTS (ElevenLabs / Google Cloud)**: High naturalness, zero subscription cost, and supports asynchronous chunk-by-chunk streaming directly over the open HTTP connection without intermediate file writes.

### Quick Start (Ubuntu / Debian / Raspberry Pi)
```bash
# 1. Install dependencies
sudo apt-get update && sudo apt-get install -y python3-pip ffmpeg
pip install fastapi uvicorn edge-tts groq python-multipart

# 2. Set your Groq API key (free tier available at https://console.groq.com)
export GROQ_API_KEY="gsk_your_groq_api_key_here"

# 3. Launch server
python3 ai_assistant_server.py --port 8000
```

### Test Voice Upload via Curl
```bash
curl -X POST "http://localhost:8000/ask.php" \
  -F "file=@sample_question.wav;type=audio/wav" \
  --output response.mp3
```

---

## 2. Distributed Telemetry Backend

TuneBar periodically flushes in-memory performance metrics (battery mV, WiFi RSSI, free DRAM/PSRAM, active screen, charging status) in compact batch JSON payloads.

### Quick Start
```bash
# 1. Install dependencies
pip install fastapi uvicorn

# 2. Launch telemetry ingest server
python3 telemetry_server.py --port 8080
```

### Test Telemetry Ingestion via Curl
```bash
curl -X POST "http://localhost:8080/myapi.php" \
  -H "Content-Type: application/json" \
  -d '{
    "table": "TUNEBAR_V2",
    "records": [
      {
        "capture_time": "2026-10-07 23:45:00",
        "uptime_s": 3600,
        "batt_mv": 4120,
        "batt_pct": 94,
        "wifi_rssi": -52,
        "dram_free_kb": 204,
        "psram_free_kb": 7372,
        "dram_pct": 40,
        "psram_pct": 10,
        "screen": "PLAYER",
        "charging": false,
        "playing": false
      }
    ]
  }'
```

---

## 3. Production Nginx Reverse Proxy & SSL Configuration

To expose these services over HTTPS port 443 with Let's Encrypt SSL:

```nginx
server {
    listen 443 ssl http2;
    server_name your-domain.com;

    ssl_certificate /etc/letsencrypt/live/your-domain.com/fullchain.pem;
    ssl_certificate_key /etc/letsencrypt/live/your-domain.com/privkey.pem;

    # AI Voice Assistant
    location /ask.php {
        proxy_pass http://127.0.0.1:8000/ask.php;
        proxy_buffering off; # Essential for instant audio streaming!
        proxy_read_timeout 60s;
        proxy_send_timeout 60s;
    }

    # Batch Telemetry
    location /myapi.php {
        proxy_pass http://127.0.0.1:8080/myapi.php;
        proxy_read_timeout 30s;
    }
}
```

---

## 4. Systemd Service Setup (Auto-Start on Boot)

Create `/etc/systemd/system/tunebar-ai.service`:
```ini
[Unit]
Description=TuneBar AI Voice Assistant Backend
After=network.target

[Service]
Type=simple
User=ubuntu
WorkingDirectory=/opt/tunebar/backend
Environment="GROQ_API_KEY=gsk_your_groq_api_key_here"
Environment="TTS_VOICE=en-US-GuyNeural"
ExecStart=/usr/bin/python3 ai_assistant_server.py --port 8000
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Enable and start:
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now tunebar-ai
```
