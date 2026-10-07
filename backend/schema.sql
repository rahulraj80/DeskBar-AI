-- ==============================================================================
-- TuneBar Distributed Telemetry Table Schema (MySQL / PostgreSQL / SQLite)
-- ==============================================================================

CREATE TABLE IF NOT EXISTS tunebar_telemetry (
    id BIGINT AUTO_INCREMENT PRIMARY KEY,
    timestamp_str VARCHAR(64),
    uptime_sec INT NOT NULL,
    battery_mv INT NOT NULL,
    battery_pct INT NOT NULL,
    wifi_rssi INT NOT NULL,
    free_dram_kb INT NOT NULL,
    free_psram_kb INT NOT NULL,
    dram_used_pct INT NOT NULL,
    psram_used_pct INT NOT NULL,
    current_screen VARCHAR(32),
    is_charging TINYINT(1) DEFAULT 0,
    audio_playing TINYINT(1) DEFAULT 0,
    recorded_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_recorded_at (recorded_at),
    INDEX idx_uptime (uptime_sec)
);
