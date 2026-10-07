"""
TuneBar Batch Telemetry Backend Reference Implementation
=========================================================
Lightweight FastAPI + SQLite/PostgreSQL Ingestion Server

Requirements:
    pip install fastapi uvicorn

Usage:
    python telemetry_server.py --port 8080
"""

import os
import sys
import sqlite3
from typing import List, Dict, Any
from fastapi import FastAPI, Request, HTTPException
from fastapi.responses import JSONResponse

app = FastAPI(title="TuneBar Telemetry Ingestion API", version="2.0.0")

DB_FILE = os.path.join(os.path.dirname(__file__), "telemetry.db")

def init_db():
    conn = sqlite3.connect(DB_FILE)
    cur = conn.cursor()
    cur.execute("""
        CREATE TABLE IF NOT EXISTS tunebar_telemetry (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            timestamp_str TEXT,
            uptime_sec INTEGER,
            battery_mv INTEGER,
            battery_pct INTEGER,
            wifi_rssi INTEGER,
            free_dram_kb INTEGER,
            free_psram_kb INTEGER,
            dram_used_pct INTEGER,
            psram_used_pct INTEGER,
            current_screen TEXT,
            is_charging INTEGER,
            audio_playing INTEGER,
            recorded_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        )
    """)
    conn.commit()
    conn.close()

init_db()

@app.post("/myapi.php")
@app.post("/api/telemetry")
async def ingest_telemetry(request: Request):
    """Handles batch JSON telemetry payloads from TuneBar circular buffer."""
    data = await request.json()
    records = data.get("records", [])
    table_name = data.get("table", "TUNEBAR_V2")

    if not isinstance(records, list):
        raise HTTPException(status_code=400, detail="Invalid payload: 'records' must be a list")

    inserted = 0
    conn = sqlite3.connect(DB_FILE)
    cur = conn.cursor()
    for r in records:
        cur.execute("""
            INSERT INTO tunebar_telemetry (
                timestamp_str, uptime_sec, battery_mv, battery_pct, wifi_rssi,
                free_dram_kb, free_psram_kb, dram_used_pct, psram_used_pct,
                current_screen, is_charging, audio_playing
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (
            r.get("capture_time", ""),
            r.get("uptime_s", 0),
            r.get("batt_mv", 0),
            r.get("batt_pct", 0),
            r.get("wifi_rssi", 0),
            r.get("dram_free_kb", 0),
            r.get("psram_free_kb", 0),
            r.get("dram_pct", 0),
            r.get("psram_pct", 0),
            r.get("screen", ""),
            1 if r.get("charging") else 0,
            1 if r.get("playing") else 0,
        ))
        inserted += 1
    conn.commit()
    conn.close()

    return JSONResponse(content={
        "status": "ok",
        "database": "sqlite",
        "table_write": {
            "table": table_name,
            "mode": "batch_insert",
            "inserted_rows": inserted
        }
    })

if __name__ == "__main__":
    import uvicorn
    port = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[1] == "--port" else 8080
    print(f"Starting TuneBar Telemetry server on http://0.0.0.0:{port}")
    uvicorn.run(app, host="0.0.0.0", port=port)
