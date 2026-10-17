import os
import sys
import time
import urllib.request
from PIL import Image

BASE_DIR = r"c:\Users\rahul\OpenCode\TuneBar"
DOC_DIR = os.path.join(BASE_DIR, "document")
SCRATCH_DIR = os.path.join(BASE_DIR, "scripts", "temp_captures")
DEVICE_URL = "http://192.168.3.4"

def http_get(path):
    url = f"{DEVICE_URL}{path}"
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.read()

def http_post(path):
    url = f"{DEVICE_URL}{path}"
    req = urllib.request.Request(url, data=b"")
    with urllib.request.urlopen(req, timeout=10) as resp:
        return resp.read()

def capture_screen(out_path):
    http_get("/api/screen?action=radio")
    time.sleep(0.4)
    data = http_get("/api/screenshot")
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"Captured: {out_path}")

def optimize_png(bmp_path, png_path):
    im = Image.open(bmp_path)
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
    print("=== 1. Building app_security.gif and Security PNGs ===")
    masked_bmp = os.path.join(SCRATCH_DIR, "screen_consent_masked.bmp")
    unmasked_bmp = os.path.join(SCRATCH_DIR, "screen_consent_unmasked.bmp")

    if os.path.exists(masked_bmp) and os.path.exists(unmasked_bmp):
        optimize_png(masked_bmp, os.path.join(DOC_DIR, "screen_consent_modal.png"))
        optimize_png(unmasked_bmp, os.path.join(DOC_DIR, "screen_pin_setup.png"))

        sec_frames = [
            Image.open(masked_bmp),
            Image.open(unmasked_bmp)
        ]
        make_gif(sec_frames, [2500, 2500], os.path.join(DOC_DIR, "app_security.gif"))
    else:
        print("Error: masked or unmasked BMP not found in scratch dir!")

    print("\n=== 2. Capturing Radio Stations with Stream Quality Telemetry ===")
    # 2.1 Hindi Station
    print("Capturing Hindi station...")
    http_get("/api/radio/lang?set=hi")
    time.sleep(1.0)
    http_post("/api/radio/play?idx=0")
    time.sleep(3.5) # Allow buffer and bitrate calculation to populate
    hi_bmp = os.path.join(SCRATCH_DIR, "screen_radio_player_hi.bmp")
    capture_screen(hi_bmp)

    # 2.2 English Station
    print("Capturing English station...")
    http_get("/api/radio/lang?set=en")
    time.sleep(1.0)
    http_post("/api/radio/play?idx=0")
    time.sleep(3.5)
    en_bmp = os.path.join(SCRATCH_DIR, "screen_radio_player_en.bmp")
    capture_screen(en_bmp)

    # 2.3 German Station
    print("Capturing German station...")
    http_get("/api/radio/lang?set=de")
    time.sleep(1.0)
    http_post("/api/radio/play?idx=0")
    time.sleep(3.5)
    de_bmp = os.path.join(SCRATCH_DIR, "screen_radio_player_de.bmp")
    capture_screen(de_bmp)

    # Restore Hindi radio
    http_get("/api/radio/lang?set=hi")
    http_post("/api/radio/play?idx=0")

    # Save PNGs and Radio GIF
    optimize_png(hi_bmp, os.path.join(DOC_DIR, "screen_radio_player_hi.png"))
    optimize_png(en_bmp, os.path.join(DOC_DIR, "screen_radio_player_en.png"))
    optimize_png(de_bmp, os.path.join(DOC_DIR, "screen_radio_player_de.png"))

    radio_frames = [
        Image.open(hi_bmp),
        Image.open(en_bmp),
        Image.open(de_bmp)
    ]
    make_gif(radio_frames, [2500, 2500, 2500], os.path.join(DOC_DIR, "app_radio.gif"))

    print("\nShowcase assets update complete!")

if __name__ == "__main__":
    main()
