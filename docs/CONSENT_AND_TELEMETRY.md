# Privacy, First-Boot Consent & Telemetry Architecture

DeskBar AI implements a **Privacy-by-Design** security model ensuring users have complete transparency, ownership, and control over all network communications.

---

## 1. First-Boot Cloud Consent Architecture

When DeskBar AI powers on for the very first time with blank or erased NVS flash:
1. **Interactive Two-Column Modal Dialog**: The device halts network telemetry and presents an ultra-wide (624x160) two-column setup dialog:
   - **Left Column**:
     - **4-Digit Master PIN**: Tap any digit to cycle 0–9, or keep the initial TRNG-generated PIN.
     - **Prominent Target Table**: Live calculation and preview of the isolated hashed table name (`TABLE: TB_<16_CHAR_HASH>`).
   - **Right Column**:
     - **Sub-header Row**: `"WEB ACCESS CODE:"` with live renewal countdown timer `(%02us)`.
     - **Web Access Code**: Prominent 20pt cyan TOTP security code `[ xxx xxx ]` with `30-DAY ACCESS` badge beside it.
     - **Privacy Guarantee**: Diagnostics contain zero personal identification data.
2. **Master Security PIN Setup**:
   - The user is prompted to configure or confirm a 4-digit Master PIN.
   - If unconfigured, a random PIN is generated via the ESP32-S3 hardware True Random Number Generator (TRNG) (`esp_random()`).
   - The PIN is persisted securely in NVS and used to salt the device's private telemetry database hash.
3. **Explicit Consent Decision**:
   - **`[ CONFIRM & CONNECT ]`**: Activates community cloud telemetry, writes auth credentials to the cloud registry, and enables the out-of-the-box voice assistant.
   - **`[ STAY OFFLINE ]`**: Disables all background telemetry permanently. The device operates in 100% air-gapped local mode (Clock, SD Music, Spirit Level, Utility Torch, and local Wi-Fi Web Remote).

---

## 2. Multi-Tenant Private Telemetry & Hardware TOTP Authentication

```
[ ESP32-S3 Hardware ] ───> [ SHA256(MAC + PIN) ] ───> Table: TB_<16_CHAR_HASH>
         │                                                       │
 (Hardware TOTP Seed)                                  (HTTPS POST every 300s)
         ▼                                                       ▼
[ TB_DEVICE_AUTH Registry ]                            [ Telemetry Server ]
(device_id, auth_seed, pin_hash)                                 │
         │                                             [ Private Web Dashboard ]
         └─────────── [ 30-Day TOTP Challenge ] ─────> telemetry.php?table=TB_<hash>
                       (Web Access Code on LCD)
```

* **Table Isolation**: Diagnostics are logged to a private, hashed database table: `TB_<device_hash>`.
* **Hardware-Bound Dynamic TOTP**:
  - The device generates a time-based 6-digit rolling web access code on-demand.
  - When accessing `telemetry.php` or `graph.php` in a web browser, the user is challenged to enter the 6-digit code currently visible on the physical screen.
  - Validating the code issues an encrypted, HTTP-only 30-day session cookie, preventing unauthorized access even if the table name is discovered.
* **Device Registry (`TB_DEVICE_AUTH`)**:
  - Automatically provisioned and maintained in Oracle Cloud DB.
  - Stores device identity, TRNG-generated `auth_seed`, hashed PIN, and session validation metadata.
* **Zero Public Enumeration**: The web portal strictly requires the exact, unguessable table name and valid TOTP session. It never exposes a directory of connected devices.
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
