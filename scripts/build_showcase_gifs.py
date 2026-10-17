import os
import sys
from PIL import Image

BASE_DIR = r"c:\Users\rahul\OpenCode\TuneBar"
DOC_DIR = os.path.join(BASE_DIR, "document")
SCRATCH_DIR = os.path.join(BASE_DIR, "scripts", "temp_captures")

def optimize_and_save_png(bmp_path, png_path):
    im = Image.open(bmp_path)
    # Save optimized 8-bit palette PNG to keep repo size small while preserving quality
    im_quant = im.convert('P', palette=Image.ADAPTIVE, colors=256)
    im_quant.save(png_path, optimize=True)
    size = os.path.getsize(png_path)
    print(f"Saved {os.path.basename(png_path)} ({size} bytes)")
    return im_quant

def make_gif(frames, durations, out_path):
    palette_frames = []
    for f in frames:
        if isinstance(f, str):
            im = Image.open(f)
        else:
            im = f
        im_p = im.convert('P', palette=Image.ADAPTIVE, colors=128)
        palette_frames.append(im_p)

    palette_frames[0].save(
        out_path,
        save_all=True,
        append_images=palette_frames[1:],
        duration=durations,
        loop=0,
        optimize=True
    )
    size = os.path.getsize(out_path)
    print(f"Generated {os.path.basename(out_path)} ({size} bytes, {len(frames)} frames)")

def main():
    print("--- 1. Updating Static PNGs in document/ ---")
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "clock.bmp"), os.path.join(DOC_DIR, "screen_clock.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "weather.bmp"), os.path.join(DOC_DIR, "screen_weather.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "alarm.bmp"), os.path.join(DOC_DIR, "screen_alarm.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "pp_top.bmp"), os.path.join(DOC_DIR, "screen_power_profile_top.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "pp_cal_560.bmp"), os.path.join(DOC_DIR, "screen_power_profile.png"))

    optimize_and_save_png(os.path.join(SCRATCH_DIR, "settings_tab_0_wifi.bmp"), os.path.join(DOC_DIR, "screen_settings_wifi.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "settings_tab_1_screen.bmp"), os.path.join(DOC_DIR, "screen_settings_screen.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "settings_tab_2_radio.bmp"), os.path.join(DOC_DIR, "screen_settings_radio.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "settings_tab_3_music.bmp"), os.path.join(DOC_DIR, "screen_settings_music.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "settings_tab_4_region.bmp"), os.path.join(DOC_DIR, "screen_settings_region.png"))
    optimize_and_save_png(os.path.join(SCRATCH_DIR, "settings_tab_5_userguide.bmp"), os.path.join(DOC_DIR, "screen_settings_userguide.png"))

    print("\n--- 2. Building Main Menu Scrolling GIF ---")
    # Scroll positions: 0, 40, 80, 120, 160, 200, 240, 280, 320
    forward_pos = [0, 40, 80, 120, 160, 200, 240, 280, 320]
    reverse_pos = [280, 240, 200, 160, 120, 80, 40]
    all_pos = forward_pos + reverse_pos

    menu_frames = []
    durations = []
    for sc in all_pos:
        bmp = os.path.join(SCRATCH_DIR, f"menu_scroll_{sc}.bmp")
        im = Image.open(bmp)
        menu_frames.append(im)
        if sc == 0 or sc == 320:
            durations.append(1200) # Pause on endpoints
        else:
            durations.append(250)  # Smooth slide

    make_gif(menu_frames, durations, os.path.join(DOC_DIR, "app_menu.gif"))

    print("\n--- 3. Building Clock Suite GIF (Clock -> Weather -> Alarm) ---")
    clock_frames = [
        Image.open(os.path.join(SCRATCH_DIR, "clock.bmp")),
        Image.open(os.path.join(SCRATCH_DIR, "weather.bmp")),
        Image.open(os.path.join(SCRATCH_DIR, "alarm.bmp")),
    ]
    clock_durations = [2500, 2500, 2500]
    make_gif(clock_frames, clock_durations, os.path.join(DOC_DIR, "app_clock.gif"))

    print("\n--- 4. Building Power Profile GIF (Top Controls -> Cloud Calibration Card) ---")
    pp_frames = [
        Image.open(os.path.join(SCRATCH_DIR, "pp_top.bmp")),
        Image.open(os.path.join(SCRATCH_DIR, "pp_cal_560.bmp")),
    ]
    pp_durations = [2500, 2500]
    make_gif(pp_frames, pp_durations, os.path.join(DOC_DIR, "app_power_profile.gif"))

    print("\n--- 5. Building Settings GIF in Tab Order (Wi-Fi, Screen, Radio, Music, Region, User Guide) ---")
    settings_tabs = ["wifi", "screen", "radio", "music", "region", "userguide"]
    settings_frames = [
        Image.open(os.path.join(SCRATCH_DIR, f"settings_tab_{idx}_{name}.bmp"))
        for idx, name in enumerate(settings_tabs)
    ]
    settings_durations = [2200, 2200, 2200, 2200, 2200, 2200]
    make_gif(settings_frames, settings_durations, os.path.join(DOC_DIR, "app_settings.gif"))

    print("\nAll GIFs and PNGs successfully built!")

if __name__ == "__main__":
    main()
