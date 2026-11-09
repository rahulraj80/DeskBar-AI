# Autonomous AI Voice Assistant: Architecture, Self-Hosting & Scaling

DeskBar AI includes an **Autonomous AI Voice Assistant** out of the box, offering a full-duplex conversational voice interface directly from your desktop.

---

## 1. Out-of-the-Box vs. Self-Hosted Notice

> [!WARNING]
> ### ⚠️ Complimentary Community AI Service Notice
> The pre-configured default AI endpoint (`https://autobots.my.to/ask.php`) is hosted as a **complimentary, personal maker service with limited server bandwidth and API quotas**.
> 
> * **Parallel Usage Slowdowns**: When multiple devices or users access the community endpoint simultaneously, response latencies may increase or requests may be throttled.
> * **Recommended Solution**: Users and makers are **strongly encouraged to deploy and host their own backend server**. Self-hosting gives you 100% private data isolation, dedicated throughput, zero quota contention, and the freedom to choose your preferred LLM and TTS engines.

---

## 2. End-to-End Voice Architecture

```
[ User Speaks ]
       │
       ▼
[ Dual MEMS Mics ] ──> [ ES7210 4-Ch ADC ] ──(I2S DIN: GPIO 6)──> [ ESP32-S3 DMA ]
                                                                        │
                                                                   [ VAD Cutoff ]
                                                                        │
                                                             [ LittleFS /rec.wav ]
                                                                        │
                                                            (HTTPS POST /ask.php)
                                                                        ▼
                                                             [ Cloud / Local Server ]
                                                                ├── Whisper STT (<200ms)
                                                                ├── Llama 3.3 70B (<250ms)
                                                                └── Edge-TTS (<300ms)
                                                                        │
                                                            (Chunked Audio Stream)
                                                                        ▼
[ 8Ω 3W Speaker ] <── [ NS4150B Amp ] <── [ ES8311 DAC ] <──(I2S DOUT: GPIO 45)
```

1. **Acoustic Front-End**:
   - Dual onboard MEMS microphones capture sound at 16 kHz / 16-bit mono via the ES7210 ADC.
   - Dynamic Voice Activity Detection (VAD) monitors acoustic energy and terminates capture after silence.
   - Captured audio is buffered to LittleFS flash as `/rec.wav` with a standardized 44-byte RIFF WAV header after DC offset removal and peak normalization.

2. **Backend Processing**:
   - Audio is transmitted via HTTPS to `/ask.php`.
   - Speech-to-Text: Groq Whisper Large V3 Turbo converts speech to text in <200ms.
   - LLM Reasoning: Llama 3.3 70B Versatile generates concise responses in <250ms.
   - Text-to-Speech: Microsoft Edge-TTS (or local Piper TTS) synthesizes speech.

3. **Audio Playback**:
   - The device streams the returned audio over Wi-Fi directly into the ESP32-audioI2S ring buffer in PSRAM and plays it through the onboard DAC and amplifier.

---

## 3. How to Set Up Your Own AI Server

You can host your own backend on any VPS, home lab server, or Raspberry Pi using standard Python or PHP:

### Step 1: Deploy Backend Script (`ask.php` or `server.py`)

A minimal FastAPI/Python backend implementation:

```python
from fastapi import FastAPI, File, UploadFile
from fastapi.responses import Response
import httpx
import os

app = FastAPI()

GROQ_API_KEY = os.getenv("GROQ_API_KEY")

@app.post("/ask.php")
async def handle_ask(audio: UploadFile = File(...)):
    # 1. Transcribe with Whisper
    audio_bytes = await audio.read()
    async with httpx.AsyncClient() as client:
        stt_resp = await client.post(
            "https://api.groq.com/openai/v1/audio/transcriptions",
            headers={"Authorization": f"Bearer {GROQ_API_KEY}"},
            files={"file": ("rec.wav", audio_bytes, "audio/wav")},
            data={"model": "whisper-large-v3-turbo"}
        )
        user_prompt = stt_resp.json().get("text", "")

        # 2. Query LLM
        chat_resp = await client.post(
            "https://api.groq.com/openai/v1/chat/completions",
            headers={"Authorization": f"Bearer {GROQ_API_KEY}"},
            json={
                "model": "llama-3.3-70b-versatile",
                "messages": [
                    {"role": "system", "content": "You are DeskBar, a concise voice assistant. Limit answers to 2 short sentences."},
                    {"role": "user", "content": user_prompt}
                ]
            }
        )
        answer_text = chat_resp.json()["choices"][0]["message"]["content"]

    # 3. Synthesize Speech (Edge-TTS or gTTS fallback)
    import edge_tts
    communicate = edge_tts.Communicate(answer_text, "en-US-JennyNeural")
    tts_audio = b""
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            tts_audio += chunk["data"]

    return Response(content=tts_audio, media_type="audio/mpeg")
```

### Step 2: Configure DeskBar AI via `/secrets.ini`

Create a file named `secrets.ini`:

```ini
[cloud]
consent=1

[ai]
endpoint=https://my-private-server.org/ask.php

[telemetry]
endpoint=https://my-private-server.org/myapi.php
interval_sec=300

[security]
master_pin=1234
```

Upload this file to your device via:
- **Web Remote**: `POST http://<device-ip>/api/secrets` with an `ai_url` field (or upload `/secrets.ini` through the file manager).
  ```bash
  curl -X POST http://<device-ip>/api/secrets \
       -d "ai_url=https://my-private-server.org/ask.php" \
       -d "telem_url=https://my-private-server.org/myapi.php" \
       -d "table=TB_XXXXXXXXXXXX" -d "interval=300" -d "pin=1234" -d "consent=1"
  curl http://<device-ip>/api/secrets   # verify the applied values
  ```
- **USB Serial CLI**: there is no `secrets set` command — use the Web Remote
  POST above, or upload `/secrets.ini` to LittleFS directly.

From v2.6.6 both voice paths (\[MIC\] upload **and** `ask`/`/api/ask`) honour
the configured `ai_url`. Earlier builds only honoured it for text queries:
`ai_upload_task()` connected to the compile-time `AI_ASSISTANT_HOST`, so voice
uploads kept going to the placeholder host whatever `/secrets.ini` said.

**Requirements for a private server:**
- `https://` only for the voice upload path (a plain `http://` endpoint is
  refused with `AI endpoint not configured` on the panel).
- Respond to `POST /ask.php` with HTTP 200 and `application/json`
  (`{"status":"ok","question":…,"answer":…,"audio_url":…}`). A 200 that is not
  JSON (parked page, captive portal, HTML error page) shows
  *"Server sent a web page, not an AI reply"*.
- If you clone the community server, remember it additionally gates on a
  device whitelist (`/var/www/data/registered_devices.json`); devices not in
  the file get `{"status":"restricted"}`.
