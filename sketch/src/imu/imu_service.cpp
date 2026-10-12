#include "imu_service.h"
#include "../qmi8658/qmi8658.h"
#include "../i2c_bsp/i2c_bsp.h"
#include "../lvgl_port/lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>

extern "C" void on_clock_interaction_wake(void);

static const char *TAG = "imu_service";

static qmi8658_dev_t s_qmi_dev;
static bool s_imu_available = false;
static SemaphoreHandle_t s_imu_mux = NULL;
static imu_reading_t s_latest_reading = {0};

// Orientation state & hysteresis debounce filter
static bool s_current_inverted = false;
static bool s_pending_inverted = false;
static uint32_t s_pending_start_ms = 0;
static const uint32_t DEBOUNCE_HOLD_MS = 350; // Stable hold threshold

bool imu_sensor_init(void) {
    if (s_imu_available) return true;
    if (!s_imu_mux) {
        s_imu_mux = xSemaphoreCreateMutex();
    }

    if (!imu_dev_handle) {
        ESP_LOGW(TAG, "imu_dev_handle is NULL, I2C bus not ready");
        return false;
    }

    esp_err_t err = qmi8658_init_with_dev(&s_qmi_dev, imu_dev_handle);
    if (err == ESP_OK) {
        s_imu_available = true;
        ESP_LOGI(TAG, "QMI8658 6-axis IMU initialized successfully on I2C port 0");
        return true;
    } else {
        ESP_LOGE(TAG, "QMI8658 initialization failed with error: %d", err);
        s_imu_available = false;
        return false;
    }
}

bool imu_sensor_is_available(void) {
    return s_imu_available;
}

bool imu_sensor_poll(void) {
    if (!s_imu_available) {
        if (!imu_sensor_init()) return false;
    }

    qmi8658_data_t raw_data;
    esp_err_t err = qmi8658_read_sensor_data(&s_qmi_dev, &raw_data);
    if (err != ESP_OK) {
        return false;
    }

    // Convert raw values (driver returns mg and dps)
    float ax_g = raw_data.accelX / 1000.0f;
    float ay_g = raw_data.accelY / 1000.0f;
    float az_g = raw_data.accelZ / 1000.0f;
    float gx_dps = raw_data.gyroX;
    float gy_dps = raw_data.gyroY;
    float gz_dps = raw_data.gyroZ;
    float temp = raw_data.temperature;

    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000ULL);

    // Orientation analysis:
    // Display is 640x172 (landscape).
    // In normal landscape sitting upright, screen top is up and gravity pulls vertically.
    // If board is held in portrait mode (|ax_g| > 0.75g while |ay_g| < 0.5g), we lock/ignore.
    // If board is lying flat on table (|az_g| > 0.8g), we preserve current landscape.
    bool candidate_inverted = s_current_inverted;
    const char *state_label = s_current_inverted ? "Inverted Landscape" : "Normal Landscape";

    float abs_x = fabsf(ax_g);
    float abs_y = fabsf(ay_g);
    float abs_z = fabsf(az_g);

    if (abs_z > 0.85f && abs_y < 0.40f && abs_x < 0.40f) {
        // Flat on desk or table
        state_label = s_current_inverted ? "Flat (Inverted Landscape)" : "Flat (Normal Landscape)";
    } else if (abs_x > 0.75f && abs_y < 0.45f) {
        // Portrait orientation - strictly ignored per requirements
        state_label = s_current_inverted ? "Portrait (Inverted Lock)" : "Portrait (Normal Lock)";
    } else {
        // Evaluate vertical axis (ay_g):
        // Standard normal landscape (buttons up): ay_g < -0.50g
        // Upside-down inverted landscape (buttons down): ay_g > +0.50g
        if (ay_g < -0.50f) {
            candidate_inverted = false;
        } else if (ay_g > 0.50f) {
            candidate_inverted = true;
        }
    }

    // Debounce / Hysteresis timing
    if (candidate_inverted != s_current_inverted) {
        if (candidate_inverted != s_pending_inverted) {
            s_pending_inverted = candidate_inverted;
            s_pending_start_ms = now_ms;
        } else if (now_ms - s_pending_start_ms >= DEBOUNCE_HOLD_MS) {
            s_current_inverted = candidate_inverted;
            if (lvgl_port_is_autorotate_enabled()) {
                lvgl_port_set_inverted_landscape(s_current_inverted);
                on_clock_interaction_wake();
                ESP_LOGI(TAG, "Screen Auto-Rotated to: %s (breathing paused, steady brightness restored)",
                         s_current_inverted ? "INVERTED (180deg)" : "NORMAL (0deg)");
            }
        }
    } else {
        s_pending_inverted = s_current_inverted;
    }

    if (s_imu_mux && xSemaphoreTake(s_imu_mux, pdMS_TO_TICKS(10)) == pdTRUE) {
        s_latest_reading.ax = ax_g;
        s_latest_reading.ay = ay_g;
        s_latest_reading.az = az_g;
        s_latest_reading.gx = gx_dps;
        s_latest_reading.gy = gy_dps;
        s_latest_reading.gz = gz_dps;
        s_latest_reading.temp_c = temp;
        s_latest_reading.timestamp_ms = now_ms;
        s_latest_reading.is_inverted = lvgl_port_is_inverted_landscape();
        s_latest_reading.orientation_str = state_label;
        xSemaphoreGive(s_imu_mux);
    }

    return true;
}

bool imu_sensor_get_latest(imu_reading_t *out) {
    if (!out) return false;
    if (s_imu_mux && xSemaphoreTake(s_imu_mux, pdMS_TO_TICKS(10)) == pdTRUE) {
        *out = s_latest_reading;
        xSemaphoreGive(s_imu_mux);
        return true;
    }
    return false;
}

void imu_sensor_set_autorotate_enabled(bool enabled) {
    lvgl_port_set_autorotate_enabled(enabled);
}

bool imu_sensor_is_autorotate_enabled(void) {
    return lvgl_port_is_autorotate_enabled();
}

void imu_sensor_force_orientation(bool inverted) {
    s_current_inverted = inverted;
    s_pending_inverted = inverted;
    lvgl_port_set_inverted_landscape(inverted);
}
