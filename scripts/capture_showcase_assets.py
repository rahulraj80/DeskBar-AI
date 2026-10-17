import os
import sys
import time
import urllib.request
from PIL import Image

BASE_URL = "http://192.168.3.4"
DOC_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "document")
SCRATCH_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "temp_captures")
os.makedirs(SCRATCH_DIR, exist_ok=True)
os.makedirs(DOC_DIR, exist_ok=True)

def api_get(endpoint):
    url = f"{BASE_URL}{endpoint}"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.read()

def capture_screenshot(save_path):
    data = api_get("/api/screenshot")
    with open(save_path, "wb") as f:
        f.write(data)
    # verify image
    img = Image.open(save_path)
    print(f"Captured {save_path}: {img.size}, {img.mode}")
    return img

def set_screen(action, tab=None, scroll=None):
    params = [f"action={action}"]
    if tab is not None:
        params.append(f"tab={tab}")
    if scroll is not None:
        params.append(f"scroll={scroll}")
    query = "&".join(params)
    api_get(f"/api/screen?{query}")
    time.sleep(0.4)

def main():
    print("Testing connection to TuneBar...")
    status = api_get("/api/status").decode('utf-8', 'replace')
    print("Device online!")

    # 1. Main Menu Scrolling Carousel Frames
    print("\n--- Capturing Main Menu Carousel Frames ---")
    menu_frames = []
    # Test scroll positions across the horizontal menu: 0 to 300 in steps of 30
    scroll_positions = [0, 40, 80, 120, 160, 200, 240, 280, 320]
    for sc in scroll_positions:
        set_screen("menu", scroll=sc)
        frame_path = os.path.join(SCRATCH_DIR, f"menu_scroll_{sc}.bmp")
        capture_screenshot(frame_path)
        menu_frames.append(frame_path)

    # 2. Clock, Weather, Alarm
    print("\n--- Capturing Clock, Weather & Alarm ---")
    set_screen("clock")
    clock_bmp = os.path.join(SCRATCH_DIR, "clock.bmp")
    capture_screenshot(clock_bmp)

    set_screen("weather")
    weather_bmp = os.path.join(SCRATCH_DIR, "weather.bmp")
    capture_screenshot(weather_bmp)

    set_screen("alarm")
    alarm_bmp = os.path.join(SCRATCH_DIR, "alarm.bmp")
    capture_screenshot(alarm_bmp)

    # 3. Power Profile: Top controls and Cloud Calibration Card
    print("\n--- Capturing Power Profile & Cloud Calibration Card ---")
    set_screen("power_profile", scroll=0)
    time.sleep(0.3)
    pp_top_bmp = os.path.join(SCRATCH_DIR, "pp_top.bmp")
    capture_screenshot(pp_top_bmp)

    # Scroll down to bottom where Cloud Calibration Card is located
    for sc_try in [400, 450, 480, 520]:
        set_screen("power_profile", scroll=sc_try)
        time.sleep(0.3)
        pp_cal_bmp = os.path.join(SCRATCH_DIR, f"pp_cal_{sc_try}.bmp")
        capture_screenshot(pp_cal_bmp)

    # 4. Settings Tabs in Exact Order (0: Wi-Fi, 1: Screen, 2: Radio, 3: Music, 4: Region, 5: User Guide)
    print("\n--- Capturing Settings Tabs (Order 0..5) ---")
    settings_bmps = []
    tab_names = ["wifi", "screen", "radio", "music", "region", "userguide"]
    for idx, name in enumerate(tab_names):
        set_screen("settings", tab=idx)
        time.sleep(0.4)
        tab_bmp = os.path.join(SCRATCH_DIR, f"settings_tab_{idx}_{name}.bmp")
        capture_screenshot(tab_bmp)
        settings_bmps.append(tab_bmp)

    print("\nAll captures complete! Returning to clock.")
    set_screen("clock")

if __name__ == "__main__":
    main()
