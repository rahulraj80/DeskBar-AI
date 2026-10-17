#!/usr/bin/env python3
"""
scripts/publish_ota.py
Publishes tested firmware to the official OTA catalog in releases/bin/.
Enforces strict retention limit (max 2 versions) to keep repository size lean and prevent Git bloat.

Usage:
    python scripts/publish_ota.py [vX.Y.Z] [--max-versions 2]
"""

import os
import sys
import shutil
import subprocess
import json
import datetime

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT_DIR)

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

MAX_OTA_VERSIONS = 2
CATALOG_JSON_PATH = os.path.join(ROOT_DIR, "releases", "firmware_catalog.json")
RELEASES_BIN_DIR = os.path.join(ROOT_DIR, "releases", "bin")
BUILD_BIN_PATH = os.path.join(ROOT_DIR, "build_phase1", "firmware.bin")

def run_cmd(cmd_list, check=True):
    cmd_str = " ".join(cmd_list)
    print(f">> {cmd_str}", flush=True)
    res = subprocess.run(
        cmd_list,
        cwd=ROOT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace"
    )
    if check and res.returncode != 0:
        print(f"[ERROR] Command failed with code {res.returncode}:\n{res.stderr}", flush=True)
        sys.exit(res.returncode)
    return res.stdout.strip()

def parse_semver(tag_str):
    clean = tag_str.lstrip("v").replace(".bin", "").replace("firmware-", "")
    parts = clean.split(".")
    major = int(parts[0]) if len(parts) > 0 and parts[0].isdigit() else 0
    minor = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 0
    patch = int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else 0
    return major, minor, patch

def get_current_version():
    # 1. Try reading sketch/include/version.h
    vh = os.path.join(ROOT_DIR, "sketch", "include", "version.h")
    if os.path.exists(vh):
        try:
            with open(vh, "r", encoding="utf-8") as f:
                for line in f:
                    if "TUNEBAR_VERSION_STR" in line:
                        v = line.split('"')[1].strip()
                        if v:
                            return v
        except Exception:
            pass
    # 2. Fallback to git tag
    tag = run_cmd(["git", "describe", "--tags", "--abbrev=0"], check=False)
    if tag and tag.startswith("v"):
        return tag.lstrip("v")
    return "2.0.5"

def main():
    max_ver = MAX_OTA_VERSIONS
    custom_ver = None
    args = sys.argv[1:]
    
    idx = 0
    while idx < len(args):
        a = args[idx]
        if a == "--max-versions" and idx + 1 < len(args):
            max_ver = int(args[idx + 1])
            idx += 2
        elif a.startswith("v") or (len(a.split(".")) == 3 and a.replace(".", "").isdigit()):
            custom_ver = a.lstrip("v")
            idx += 1
        else:
            idx += 1

    ver = custom_ver if custom_ver else get_current_version()
    ver_tag = f"v{ver}"
    target_bin_name = f"firmware-{ver_tag}.bin"
    target_bin_path = os.path.join(RELEASES_BIN_DIR, target_bin_name)

    print(f"=======================================================", flush=True)
    print(f" TuneBar OTA Release Publisher: {ver_tag}", flush=True)
    print(f" Retention Policy: Maximum {max_ver} versions in Git", flush=True)
    print(f"=======================================================", flush=True)

    # 1. Verify build_phase1/firmware.bin exists
    if not os.path.exists(BUILD_BIN_PATH):
        print(f"[FATAL ERROR] Source binary not found at: {BUILD_BIN_PATH}", flush=True)
        print("Compile the firmware first using sync_and_build.py or release.py.", flush=True)
        sys.exit(1)

    bin_size = os.path.getsize(BUILD_BIN_PATH)
    if bin_size < 1000000:
        print(f"[FATAL ERROR] Binary size {bin_size} bytes is suspiciously small (<1MB)!", flush=True)
        sys.exit(1)

    # 2. Copy binary to releases/bin/
    os.makedirs(RELEASES_BIN_DIR, exist_ok=True)
    shutil.copy2(BUILD_BIN_PATH, target_bin_path)
    print(f"[OK] Staged {target_bin_path} ({bin_size:,} bytes)", flush=True)

    # 3. Discover all binaries in releases/bin/ and enforce retention limit
    all_bins = [f for f in os.listdir(RELEASES_BIN_DIR) if f.startswith("firmware-") and f.endswith(".bin")]
    
    def bin_sort_key(fname):
        return parse_semver(fname)

    all_bins.sort(key=bin_sort_key, reverse=True)

    keep_bins = all_bins[:max_ver]
    prune_bins = all_bins[max_ver:]

    print(f"\n[OTA Active Versions] (Keeping top {len(keep_bins)}):", flush=True)
    for b in keep_bins:
        print(f"  * {b}", flush=True)

    if prune_bins:
        print(f"\n[OTA Pruning] Removing older binaries to prevent Git bloat:", flush=True)
        for old_b in prune_bins:
            old_path = os.path.join(RELEASES_BIN_DIR, old_b)
            # Check if tracked in git
            tracked = run_cmd(["git", "ls-files", old_path], check=False)
            if tracked:
                run_cmd(["git", "rm", "-f", old_path], check=False)
                print(f"  - Removed from Git index: {old_b}", flush=True)
            if os.path.exists(old_path):
                os.remove(old_path)
                print(f"  - Deleted from disk: {old_b}", flush=True)

    # 4. Update firmware_catalog.json with accurate file size and entry
    if os.path.exists(CATALOG_JSON_PATH):
        try:
            with open(CATALOG_JSON_PATH, "r", encoding="utf-8") as f:
                cat_data = json.load(f)
            releases = cat_data.get("releases", [])
            for r in releases:
                if r.get("version") in [ver, ver_tag]:
                    r["size"] = bin_size
                    break
            with open(CATALOG_JSON_PATH, "w", encoding="utf-8") as f:
                json.dump(cat_data, f, indent=2)
        except Exception as e:
            print(f"[WARN] Failed to update catalog file size: {e}", flush=True)

    # 5. Synchronize catalog headers
    gen_cat_script = os.path.join(ROOT_DIR, "scripts", "generate_catalog.py")
    if os.path.exists(gen_cat_script):
        subprocess.run([sys.executable, gen_cat_script], check=True)

    # 6. Stage and commit release binary and catalog
    print("\n--- Staging and committing OTA release ---", flush=True)
    run_cmd(["git", "add", "releases/bin", "releases/firmware_catalog.json", "sketch/include/generated_catalog.h"])
    
    # Check if there are changes to commit
    st = run_cmd(["git", "status", "--porcelain"])
    if st:
        run_cmd([
            "git", "commit", "-m", f"ota: publish TuneBar {ver_tag} for OTA deployment (retained top {max_ver} versions)",
            "--author=Rahul_Azure <rahul.raj@gmail.com>"
        ])
        print("--- Pushing to GitHub origin main ---", flush=True)
        run_cmd(["git", "push", "origin", "main"])
    else:
        print("[INFO] No catalog or binary changes needed to be committed.", flush=True)

    print(f"\n=======================================================", flush=True)
    print(f" SUCCESS: DeskBar AI {ver_tag} published to official OTA!", flush=True)
    print(f" URL: https://raw.githubusercontent.com/rahulraj80/DeskBar-AI/main/releases/bin/{target_bin_name}", flush=True)
    print(f" Active OTA versions in repo: {', '.join(keep_bins)}", flush=True)
    print(f"=======================================================\n", flush=True)

if __name__ == "__main__":
    main()
