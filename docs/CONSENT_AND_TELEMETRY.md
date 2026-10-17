# Privacy, First-Boot Consent & Telemetry Architecture

DeskBar AI implements a **Privacy-by-Design** security model ensuring users have complete transparency, ownership, and control over all network communications.

---

## 1. First-Boot Cloud Consent Architecture

When DeskBar AI powers on for the very first time with blank or erased NVS flash:
1. **Interactive Modal Dialog**: The device halts network telemetry and presents a full-screen consent dialog.
2. **Master Security PIN Setup**:
   - The user is prompted to configure or confirm a 4-digit Master PIN.
   - If unconfigured, a random PIN is generated via the ESP32-S3 hardware True Random Number Generator (TRNG) (`esp_random()`).
   - The PIN is persisted securely in NVS and used to salt the device's private telemetry database hash.
3. **Explicit Consent Decision**:
   - **`[ ACCEPT ]`**: Activates community cloud telemetry and enables the out-of-the-box voice assistant.
   - **`[ DECLINE ]`**: Disables all background telemetry permanently. The device operates in 100% air-gapped local mode (Clock, SD Music, Spirit Level, Utility Torch, and local Wi-Fi Web Remote).

---

## 2. Multi-Tenant Private Telemetry Isolation

```
[ ESP32-S3 Hardware ] ───> [ SHA256(MAC + PIN) ] ───> Table: TB_<16_CHAR_HASH>
                                                                 │
                                                       (HTTPS POST every 300s)
                                                                 ▼
                                                       [ Telemetry Server ]
                                                                 │
                                                     [ Private Web Dashboard ]
                                    https://autobots.my.to/telemetry.php?table=TB_<hash>
```

* **Table Isolation**: Diagnostics are logged to a private, hashed database table: `TB_<device_hash>`.
* **Zero Public Enumeration**: The web portal strictly requires the exact, unguessable table name. It never exposes a directory of other connected devices.
* **5-Minute Telemetry Cadence**: Diagnostics (Battery voltage, Wi-Fi RSSI, DRAM heap, PSRAM free, active audio stream, and system uptime) are sampled every 300 seconds and buffered in PSRAM, minimizing radio wake-ups.

---

## 3. Decoupling Secrets via `/secrets.ini`

All baked community defaults can be overridden without modifying C++ source code or recompiling firmware:

```ini
[cloud]
consent=1

[ai]
endpoint=https://your-server.com/ask.php

[telemetry]
endpoint=https://your-server.com/myapi.php
table=MY_CUSTOM_TABLE
interval_sec=300

[weather]
api_key=your_api_key

[security]
master_pin=5678
```

Place `/secrets.ini` into LittleFS to take complete control of your device endpoints.
