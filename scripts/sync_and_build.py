#!/usr/bin/env python3
"""
sync_and_build.py - Git-First Build Pipeline for TuneBar.

Workflow:
1. Verifies local git status and pushes commits & tags to GitHub (origin/main).
2. Syncs untracked private secrets (sketch/include/secrets.h) to auto.
3. Instructs auto to clean-fetch origin/main and compile via PlatformIO.
4. Downloads the compiled firmware.bin and partitions.bin to build_phase1/.
"""

import subprocess
import sys
import os

ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT_DIR)

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

SSH_OPTS = [
    "-o", "ClearAllForwardings=yes",
    "-o", "BatchMode=yes",
    "-o", "ConnectTimeout=15",
    "-o", "ServerAliveInterval=10",
    "-o", "ServerAliveCountMax=10",
    "-o", "TCPKeepAlive=yes"
]

def run(cmd_list, check=True):
    cmd_str = " ".join(cmd_list)
    print(">>", cmd_str, flush=True)
    proc = subprocess.Popen(
        cmd_list,
        cwd=ROOT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace"
    )
    for line in proc.stdout:
        print(line.rstrip('\r\n'), flush=True)
    proc.wait()
    if check and proc.returncode != 0:
        print(f"[ERROR] Command failed with code {proc.returncode}: {cmd_str}", flush=True)
        sys.exit(proc.returncode)
    return proc.returncode

def main():
    skip_push = "--no-push" in sys.argv
    skip_secrets = "--no-secrets" in sys.argv

    print("=======================================================", flush=True)
    print(" TuneBar Git-First Build Pipeline", flush=True)
    print("=======================================================", flush=True)

    # 1. Check for uncommitted changes
    status_out = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=ROOT_DIR,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    ).stdout.strip()

    if status_out:
        print("[NOTICE] Working tree has uncommitted local changes:", flush=True)
        for line in status_out.splitlines()[:5]:
            print(f"  {line}", flush=True)
        print("Note: The remote build will compile the latest commit pushed to GitHub origin/main.", flush=True)

    # 2. Push commits and tags to GitHub
    if not skip_push:
        print("\n--- 1. Pushing commits and tags to GitHub origin/main ---", flush=True)
        code = run(["git", "push", "origin", "main", "--tags"], check=False)
        if code != 0:
            print("[WARN] git push returned non-zero code. Proceeding with remote build...", flush=True)
    else:
        print("\n--- 1. Skipping git push (--no-push specified) ---", flush=True)

    # 3. Sync private untracked secrets (secrets.h) to auto if present
    secrets_path = os.path.join(ROOT_DIR, "sketch", "include", "secrets.h")
    if not skip_secrets and os.path.exists(secrets_path):
        print("\n--- 2. Syncing private credentials (sketch/include/secrets.h) to auto ---", flush=True)
        run(["scp", "-O"] + SSH_OPTS + [
            secrets_path,
            "auto:~/scripts/TuneBar/sketch/include/secrets.h"
        ], check=False)

    # 4. Trigger remote git pull and PlatformIO compilation on auto
    print("\n--- 3. Clean-syncing repo on auto from GitHub and compiling ---", flush=True)
    remote_cmd = (
        "cd ~/scripts/TuneBar && "
        "git fetch origin && "
        "git checkout main && "
        "git reset --hard origin/main && "
        "cd sketch && ~/.platformio/penv/bin/pio run"
    )
    code = run(["ssh", "-n"] + SSH_OPTS + ["auto", f"bash -l -c '{remote_cmd}'"])
    if code != 0:
        print("[FATAL ERROR] Remote PlatformIO compilation failed!", flush=True)
        sys.exit(code)

    # 5. Download compiled firmware.bin and partitions.bin via SCP
    print("\n--- 4. Downloading compiled firmware.bin and partitions.bin ---", flush=True)
    os.makedirs(os.path.join(ROOT_DIR, "build_phase1"), exist_ok=True)
    
    run(["scp", "-O"] + SSH_OPTS + [
        "auto:~/scripts/TuneBar/sketch/.pio/build/esp32-s3-devkitc1-n16r8/firmware.bin",
        os.path.join("build_phase1", "firmware.bin")
    ])

    run(["scp", "-O"] + SSH_OPTS + [
        "auto:~/scripts/TuneBar/sketch/.pio/build/esp32-s3-devkitc1-n16r8/bootloader.bin",
        os.path.join("build_phase1", "bootloader.bin")
    ], check=False)

    run(["scp", "-O"] + SSH_OPTS + [
        "auto:~/scripts/TuneBar/sketch/.pio/build/esp32-s3-devkitc1-n16r8/partitions.bin",
        os.path.join("build_phase1", "partitions.bin")
    ], check=False)

    run(["scp", "-O"] + SSH_OPTS + [
        "auto:/home/ubuntu/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin",
        os.path.join("build_phase1", "boot_app0.bin")
    ], check=False)

    bin_size = os.path.getsize(os.path.join(ROOT_DIR, "build_phase1", "firmware.bin"))
    print(f"\n=======================================================", flush=True)
    print(f" BUILD SUCCESS: build_phase1/firmware.bin ready! ({bin_size:,} bytes)", flush=True)
    print(f" Flash to device and test.", flush=True)
    print(f" After testing, publish to OTA with: python scripts/publish_ota.py", flush=True)
    print(f"=======================================================\n", flush=True)

if __name__ == "__main__":
    main()
