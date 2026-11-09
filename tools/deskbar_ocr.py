"""DeskBar-AI screen OCR helper.

Takes a screenshot from the device and runs tesseract OCR so the agent can
read exactly what is on screen before deciding where to tap. Also provides a
`find_word` helper that locates a word's pixel bounding box.

Usage:
    python3 tools/deskbar_ocr.py shot /tmp/screen.bmp
    python3 tools/deskbar_ocr.py ocr /tmp/screen.bmp
    python3 tools/deskbar_ocr.py shotocr        # capture + OCR in one step
    python3 tools/deskbar_ocr.py find LAN        # capture + locate word 'LAN'
"""

import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image, ImageOps  # noqa: E402
from deskbar_serial import DeskBarSerial  # noqa: E402


def preprocess(path, scale=4):
    """Upscale + autocontrast for better OCR on the 640x172 panel."""
    img = Image.open(path).convert("L")
    img = ImageOps.autocontrast(img)
    if scale != 1:
        img = img.resize((img.width * scale, img.height * scale), Image.LANCZOS)
    out = path + ".prep.png"
    img.save(out)
    return out, img


def ocr_words(path):
    """Return list of (text, x, y, w, h) in ORIGINAL 640x172 coordinates."""
    prep, img = preprocess(path)
    # TSV gives per-word bounding boxes
    res = subprocess.run(
        ["tesseract", prep, "stdout", "--psm", "11", "tsv"],
        capture_output=True, text=True,
    )
    scale = img.width / 640
    words = []
    for line in res.stdout.splitlines()[1:]:
        parts = line.split("\t")
        if len(parts) < 12:
            continue
        text = parts[11].strip()
        if not text:
            continue
        try:
            conf = float(parts[10])
        except ValueError:
            continue
        if conf < 30:
            continue
        x, y, w, h = (int(parts[6]) / scale, int(parts[7]) / scale,
                      int(parts[8]) / scale, int(parts[9]) / scale)
        words.append((text, x, y, w, h))
    return words


def ocr_text(path):
    prep, _ = preprocess(path)
    res = subprocess.run(["tesseract", prep, "stdout", "--psm", "11"],
                         capture_output=True, text=True)
    return res.stdout


def capture(path="/tmp/deskbar_screen.bmp"):
    with DeskBarSerial() as dbs:
        # Make sure we are not mid-transition.
        time.sleep(0.8)
        return dbs.screenshot(path)


def cmd_shot(args):
    path = args[0] if args else "/tmp/deskbar_screen.bmp"
    print(capture(path))


def cmd_ocr(args):
    path = args[0] if args else "/tmp/deskbar_screen.bmp"
    print(ocr_text(path))


def cmd_shotocr(args):
    path = args[0] if args else "/tmp/deskbar_screen.bmp"
    capture(path)
    print(f"=== OCR {path} ===")
    print(ocr_text(path))
    print("=== WORDS ===")
    for t, x, y, w, h in ocr_words(path):
        print(f"  {t!r:20s} at x={x:.0f} y={y:.0f} w={w:.0f} h={h:.0f} (center x={x+w/2:.0f})")


def cmd_find(args):
    if not args:
        print("usage: find <word> [image]")
        return
    word = args[0].lower()
    path = args[1] if len(args) > 1 else "/tmp/deskbar_screen.bmp"
    if not os.path.exists(path):
        capture(path)
    for t, x, y, w, h in ocr_words(path):
        if word in t.lower():
            print(f"{t!r} at x={x:.0f} y={y:.0f} w={w:.0f} h={h:.0f} "
                  f"-> TAP ({x + w/2:.0f}, {y + h/2:.0f})")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return
    cmd = sys.argv[1]
    args = sys.argv[2:]
    if cmd == "shot":
        cmd_shot(args)
    elif cmd == "ocr":
        cmd_ocr(args)
    elif cmd == "shotocr":
        cmd_shotocr(args)
    elif cmd == "find":
        cmd_find(args)
    else:
        print(__doc__)


if __name__ == "__main__":
    main()
