import os
import shutil

ROOT_DIR = r"c:\Users\rahul\OpenCode\TuneBar"
SCRATCH_DIR = os.path.join(ROOT_DIR, "scratch")
os.makedirs(SCRATCH_DIR, exist_ok=True)

KEEP_FILES = {
    ".gitignore",
    "AUDIO_HARDWARE_LEARNINGS.md",
    "BUILD_GUIDE.md",
    "CHANGELOG.md",
    "LICENSE.md",
    "platformio.ini",
    "README.md",
    "release.py",
    "TuneBar.code-workspace",
    "secrets_example.ini",
    "CONTINUATION.md"
}

moved_count = 0
for item in os.listdir(ROOT_DIR):
    item_path = os.path.join(ROOT_DIR, item)
    # Only clean loose files, do not touch directories
    if os.path.isfile(item_path):
        if item not in KEEP_FILES:
            dest_path = os.path.join(SCRATCH_DIR, item)
            # If destination exists, replace or overwrite
            if os.path.exists(dest_path):
                os.remove(dest_path)
            shutil.move(item_path, dest_path)
            moved_count += 1
            print(f"Moved to scratch/: {item}")

print(f"\nCleanup complete! Moved {moved_count} files to scratch/.")
