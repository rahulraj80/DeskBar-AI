#ifndef SECRETS_EXAMPLE_H
#define SECRETS_EXAMPLE_H

/**
 * ==============================================================================
 * TuneBar Secrets & External Services Configuration Template
 * ==============================================================================
 *
 * HOW TO USE THIS FILE:
 * 1. Copy this file to "secrets.h" in the same directory:
 *      cp sketch/include/secrets_example.h sketch/include/secrets.h
 * 2. Replace the placeholder URLs, endpoints, and API keys with your actual values.
 * 3. Keep "secrets.h" UNTRACKED in Git (it is already in .gitignore).
 *    NEVER commit your production domains, passwords, or tokens to a public repository!
 *
 * ==============================================================================
 * SERVICE 1: AI VOICE ASSISTANT (HTTPS TLS, Port 443)
 * ==============================================================================
 * TuneBar captures user voice via the onboard ES7210 microphone, detects voice
 * boundaries using Voice Activity Detection (VAD), and uploads a 16kHz WAV file
 * over a secure TLS connection directly to your server.
 *
 * PROTOCOL & SECURITY:
 * - Transport: HTTPS (Port 443 with TLS 1.2 / 1.3).
 * - Port 80 unencrypted HTTP must strictly redirect (301) to HTTPS for safety.
 * - Pacing: The ESP32 streams audio in 1024-byte chunks with 15ms FreeRTOS yields
 *   to avoid lwIP TCP transmit window congestion.
 *
 * INVOCATION METHODS:
 * A. Voice Query (Onboard [MIC] Button or VAD):
 *    - HTTP Method: POST
 *    - Content-Type: multipart/form-data with field name 'audio' containing
 *      a 16-bit Mono PCM WAV file @ 16,000 Hz, peak normalized.
 *    - Endpoint: AI_ASSISTANT_URL
 *
 * B. Text Query Fallback (Web Remote / Serial CLI):
 *    - HTTP Method: GET
 *    - URL: AI_ASSISTANT_URL + "?q=" + urlencode(query)
 *
 * EXPECTED SERVER RESPONSE (HTTP 200 OK, Content-Type: application/json):
 * {
 *   "status": "ok",
 *   "question": "What is the capital of Japan?",
 *   "answer": "The capital of Japan is Tokyo.",
 *   "audio_url": "https://your-domain.com/ai_cache/47b32cb83c774231565dea61af161d61.mp3"
 * }
 *
 * RESPONSE FIELDS:
 * - status     : String, "ok" on success or "error" on failure.
 * - question   : String, the Speech-to-Text transcribed query (displayed on screen).
 * - answer     : String, the LLM-generated response text (displayed on screen).
 * - audio_url  : String (optional), public HTTPS URL pointing to the synthesized MP3
 *                answer audio. If provided, TuneBar will stream and play this audio
 *                aloud through the onboard ES8311 speaker.
 *
 * SERVER-SIDE BACKEND BLUEPRINT:
 * 1. Speech-to-Text (STT): Groq API using 'whisper-large-v3-turbo' with parameter
 *    "language": "en" and "temperature": "0.0" to prevent subtitle hallucination.
 * 2. LLM Generation: Groq LLaMA 3.3 70B, Gemini 1.5/2.5 Flash, or OpenAI GPT-4o.
 * 3. Text-to-Speech (TTS): Microsoft Edge TTS ('en-US-AriaNeural' or 'en-US-GuyNeural').
 * 4. MP3 Caching: Save MP3 to a public web cache directory and return the URL.
 */
#ifndef AI_ASSISTANT_HOST
#define AI_ASSISTANT_HOST        "api.your-domain.com"
#endif

#ifndef AI_ASSISTANT_PORT
#define AI_ASSISTANT_PORT        443
#endif

#ifndef AI_ASSISTANT_PATH
#define AI_ASSISTANT_PATH        "/ask.php"
#endif

#ifndef AI_ASSISTANT_URL
#define AI_ASSISTANT_URL         "https://" AI_ASSISTANT_HOST AI_ASSISTANT_PATH
#endif


/**
 * ==============================================================================
 * SERVICE 2: TELEMETRY & DIAGNOSTICS LOGGING (HTTPS TLS)
 * ==============================================================================
 * TuneBar periodically captures system operational metrics (memory headroom,
 * battery health, WiFi RSSI, audio playback state) into an internal ring buffer
 * and batches flushes to your remote logging server.
 *
 * PROTOCOL & TIMING:
 * - Capture Rate: 1 snapshot every 60 seconds.
 * - Flush Rate: Every 5 minutes or when >= 5 snapshots accumulate.
 * - Concurrency: Flushes are automatically deferred while voice recording,
 *   uploading, or playing audio to prevent TLS socket contention.
 *
 * INVOCATION METHOD:
 * - HTTP Method: POST
 * - Content-Type: application/json
 * - Payload:
 *   {
 *     "table": "TUNEBAR_V2",
 *     "rows": [
 *       {
 *         "capture_time": "2026-10-06 21:55:56",
 *         "uptime_s": 64,
 *         "dram_free_kb": 83,
 *         "dram_used_pct": 74,
 *         "sram_total_kb": 333,
 *         "psram_free_kb": 5463,
 *         "psram_used_pct": 34,
 *         "batt_mv": 4120,
 *         "batt_pct": 98,
 *         "rssi": -52,
 *         "audio": 0,
 *         "screen": 1,
 *         "reset_reason": 11
 *       }
 *     ]
 *   }
 *
 * EXPECTED SERVER RESPONSE (HTTP 200 OK, application/json):
 * {
 *   "status": "ok",
 *   "database": "ok",
 *   "table_write": {
 *     "table": "TUNEBAR_V2",
 *     "mode": "batch_insert",
 *     "action": "inserted 5 rows",
 *     "count": 5
 *   }
 * }
 */
#ifndef TELEMETRY_HOST
#define TELEMETRY_HOST           "api.your-domain.com"
#endif

#ifndef TELEMETRY_PORT
#define TELEMETRY_PORT           443
#endif

#ifndef TELEMETRY_ENDPOINT
#define TELEMETRY_ENDPOINT       "https://" TELEMETRY_HOST "/myapi.php"
#endif

#ifndef TELEMETRY_TABLE
#define TELEMETRY_TABLE          "TUNEBAR_V2"
#endif


/**
 * ==============================================================================
 * SERVICE 3: WEATHER & AIR QUALITY INDEX (WeatherAPI.com)
 * ==============================================================================
 * Provides live weather conditions, temperature, humidity, and atmospheric pollutant
 * concentrations used by the Info screen and ambient weather animations.
 *
 * PROVIDER:
 * - Free API keys available at: https://www.weatherapi.com/
 *
 * INVOCATION METHOD:
 * - HTTP Method: GET
 * - Query format: /v1/current.json?key=<API_KEY>&q=<LOCATION>&aqi=yes
 *   where <LOCATION> is dynamically formatted with "auto:ip", coordinates "lat,lon",
 *   or a city name ("Tokyo").
 *
 * EXPECTED FIELDS:
 * - current.temp_c (temperature)
 * - current.condition.code (mapped to weather icons)
 * - current.air_quality (PM2.5, PM10, CO, NO2, O3, SO2) used to compute US EPA AQI.
 */
#ifndef WEATHER_API_KEY
#define WEATHER_API_KEY          "YOUR_WEATHERAPI_COM_KEY"
#endif

#ifndef WEATHER_API_URL
#define WEATHER_API_URL          "http://api.weatherapi.com/v1/current.json?key=%s&q=%s&aqi=yes"
#endif


/**
 * ==============================================================================
 * SERVICE 4: LAN AUDIO STREAMING (LocalShare / HTTP Audio Server)
 * ==============================================================================
 * TuneBar can stream MP3/WAV tracks directly across your local network from a
 * home media server, Jellyfin, Icecast, or the LocalShare file sharing tool.
 *
 * FORMAT:
 * - "IP:PORT/PATH" (e.g. "192.168.1.100:8080/music" or "10.0.0.5:8080")
 * - Can also be changed at runtime via Settings or the Web Remote interface.
 */
#ifndef DEFAULT_LAN_STREAM_SRV
#define DEFAULT_LAN_STREAM_SRV   "192.168.1.100:8080/audio"
#endif


/**
 * ==============================================================================
 * SERVICE 5: OVER-THE-AIR (OTA) FIRMWARE UPDATES
 * ==============================================================================
 * Automatically checks for firmware updates on boot or via Settings -> System Info.
 *
 * MANIFEST FORMAT:
 * {
 *   "version": "1.2.2",
 *   "url": "https://your-domain.com/firmware/tunebar.bin"
 * }
 */
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL         "https://vaandcob.github.io/webpage/firmware/tunebar/tunebar_manifest.json"
#endif

#ifndef OTA_FIRMWARE_URL
#define OTA_FIRMWARE_URL         "https://vaandcob.github.io/webpage/firmware/tunebar/tunebar.bin"
#endif


/**
 * ==============================================================================
 * SERVICE 6: OPTIONAL FIRST-BOOT WI-FI CREDENTIALS
 * ==============================================================================
 * If left empty, TuneBar will enter access point / configuration mode or look for
 * existing credentials stored in LittleFS ("wifi.json"). If filled, TuneBar will
 * attempt to connect to this network on initial startup.
 */
#ifndef DEFAULT_WIFI_SSID
#define DEFAULT_WIFI_SSID        ""
#endif

#ifndef DEFAULT_WIFI_PASS
#define DEFAULT_WIFI_PASS        ""
#endif

#endif // SECRETS_EXAMPLE_H
