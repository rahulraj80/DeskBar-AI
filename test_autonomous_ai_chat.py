import serial
import time
import subprocess
import threading
import urllib.request
import urllib.parse
import io
import os
import sys
import base64
import sounddevice as sd
import soundfile as sf
import numpy as np

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
if hasattr(sys.stderr, 'reconfigure'):
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')

PORT = 'COM11'
BAUD = 115200
ARTIFACT_DIR = r"C:\Users\rahul\.gemini\antigravity\brain\d15cb0dd-3139-4610-a24e-919fdef85c3d"

print("==================================================================", flush=True)
print("     AUTONOMOUS PHYSICAL AI CHAT VALIDATION VIA USB & ACOUSTICS    ", flush=True)
print("==================================================================", flush=True)

# -------------------------------------------------------------
# STEP 1: PREPARE ACOUSTIC QUESTION AUDIO FOR LAPTOP SPEAKERS
# -------------------------------------------------------------
QUESTION_TEXT = "What is the weather in Mumbai"
print(f"\n[1. ACOUSTIC PREP] Generating acoustic speech audio for query: '{QUESTION_TEXT}'...", flush=True)
tts_url = "https://autobots.my.to/ask.php?q=" + urllib.parse.quote(QUESTION_TEXT)
req = urllib.request.Request(tts_url, headers={'User-Agent': 'LaptopSpeakerTTS/1.0'})
try:
    with urllib.request.urlopen(req, timeout=10) as resp:
        tts_audio_bytes = resp.read()
    tts_data, tts_sr = sf.read(io.BytesIO(tts_audio_bytes))
    print(f"[1. ACOUSTIC PREP] Question audio ready: {len(tts_data)} samples @ {tts_sr} Hz (~{len(tts_data)/tts_sr:.1f}s)", flush=True)
except Exception as e:
    print(f"[1. ACOUSTIC PREP] Failed to fetch TTS from server: {e}. Falling back to synthesized speech tone.", flush=True)
    tts_sr = 16000
    t = np.linspace(0, 2.5, int(2.5 * tts_sr), endpoint=False)
    tts_data = 0.5 * np.sin(2 * np.pi * 440 * t)

# Save question reference artifact
question_ref_path = os.path.join(ARTIFACT_DIR, "step1_laptop_speech_question.wav")
sf.write(question_ref_path, tts_data, tts_sr)
print(f"[1. ACOUSTIC PREP] Reference audio saved: {question_ref_path}", flush=True)

# -------------------------------------------------------------
# STEP 2: OPEN SERIAL CONNECTION TO TUNEBAR (SAFE CDC PARAMS)
# -------------------------------------------------------------
print(f"\n[2. USB CONNECT] Connecting to TuneBar on {PORT} with dtr=False, rts=False...", flush=True)
ser = serial.Serial()
ser.port = PORT
ser.baudrate = BAUD
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
try:
    ser.open()
except Exception as e:
    print(f"Failed to open {PORT}: {e}", flush=True)
    sys.exit(1)

time.sleep(0.5)
while ser.in_waiting:
    ser.read()

def send_cmd(cmd_str, wait_sec=0.5):
    print(f"  [CLI TX] > {cmd_str}", flush=True)
    ser.write((cmd_str + "\n").encode('utf-8'))
    time.sleep(wait_sec)
    lines = []
    while ser.in_waiting:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if line:
            lines.append(line)
            print(f"    [CLI RX] {line}", flush=True)
    return lines

# Wake device & set maximum volume
print("\n[2. DEVICE SETUP] Waking display and ensuring volume 21 & WiFi online...", flush=True)
send_cmd("touch", 0.3)
send_cmd("vol 21", 0.3)
send_cmd("wifi on", 0.5)

# -------------------------------------------------------------
# STEP 3: NAVIGATE UI TO AI ASSISTANT SCREEN
# -------------------------------------------------------------
print("\n[3. UI NAVIGATION] Navigating to AI Assistant Card...", flush=True)
send_cmd("assistant", 1.5)

# -------------------------------------------------------------
# STEP 4: SETUP ROOM AUDIO LISTENER (LAPTOP MIC)
# -------------------------------------------------------------
RECORD_ROOM_SECS = 35
MIC_SR = 16000
rec_audio_buf = None
room_wav_path = os.path.join(ARTIFACT_DIR, "acoustic_device_response.wav")

def record_room_mic():
    global rec_audio_buf
    print(f"\n[MIC LISTENER] Laptop microphone listening to room for {RECORD_ROOM_SECS} seconds...", flush=True)
    rec = sd.rec(int(RECORD_ROOM_SECS * MIC_SR), samplerate=MIC_SR, channels=1, dtype='int16')
    sd.wait()
    rec_audio_buf = rec
    sf.write(room_wav_path, rec, MIC_SR)
    print(f"[MIC LISTENER] Saved physical room acoustic recording to: {room_wav_path}", flush=True)

mic_thread = threading.Thread(target=record_room_mic)
mic_thread.start()
time.sleep(0.4)

# -------------------------------------------------------------
# STEP 5: SIMULTANEOUS SPEAKER PLAYBACK & MIC TRIGGER
# -------------------------------------------------------------
print("\n[4. USER SPEAKS & TRIGGERS MIC] Starting acoustic speech and activating mic simultaneously...", flush=True)
print(f"   >>> \"{QUESTION_TEXT}\" <<<", flush=True)
# Start acoustic question audio over laptop speakers
sd.play(np.clip(tts_data * 2.0, -1.0, 1.0), tts_sr)
time.sleep(0.05)
# Immediately trigger record on TuneBar so the question is captured from start
ser.write(b"click mic\n")
sd.wait()
print("[4. USER SPEAKS ALOUD] Spoken question finished. TuneBar ES7210 capturing rest of 4.0s window...", flush=True)

# -------------------------------------------------------------
# STEP 6: MONITOR DEVICE LOGS & CAPTURE PHYSICAL SCREEN
# -------------------------------------------------------------
print("\n[5. DEVICE MONITOR] Listening for recording completion, upload, and answer speech...", flush=True)
start_monitor = time.time()
snapped_screen = False
captured_payload = ""

while time.time() - start_monitor < 32.0:
    if ser.in_waiting:
        line = ser.readline().decode('utf-8', errors='replace').strip()
        if line:
            print(f"  [DEVICE EVENT] {line}", flush=True)
            if "[AI UPLOAD] JSON payload" in line:
                captured_payload = line
            # When AI starts streaming answer audio or subtitle appears, take webcam photo of physical LCD!
            if ("Streaming answer" in line or "STATUS_UPDATE_AI_SPEAKING" in line or "audioPlayHOST" in line or "[AI INFO] Stream decoding" in line) and not snapped_screen:
                snapped_screen = True
                print("\n[CAMERA] Capturing physical screen photo while device displays and speaks response...", flush=True)
                cam_proc = subprocess.run([sys.executable, "snap_bright.py", "screen_ai_mumbai_weather.png"], capture_output=True, text=True)
                print(f"[CAMERA] {cam_proc.stdout.strip()}", flush=True)
    else:
        time.sleep(0.05)

# Wait for room mic listener to finish
mic_thread.join()

# If screen wasn't snapped during playback, snap it now
if not snapped_screen:
    print("\n[CAMERA] Capturing physical screen photo of current device state...", flush=True)
    cam_proc = subprocess.run([sys.executable, "snap_bright.py", "screen_ai_mumbai_weather.png"], capture_output=True, text=True)
    print(f"[CAMERA] {cam_proc.stdout.strip()}", flush=True)

# -------------------------------------------------------------
# STEP 7: RETRIEVE PHYSICAL RECORDING OVER HIGH-SPEED HTTP / USB
# -------------------------------------------------------------
print("\n[6. AUDIO RETRIEVE] Retrieving exact /rec.wav recorded by TuneBar...", flush=True)
retrieved_device_wav_path = os.path.join(ARTIFACT_DIR, "tunebar_device_recorded.wav")
retrieved_ok = False

try:
    http_url = "http://192.168.3.4/rec.wav"
    req = urllib.request.Request(http_url, headers={'User-Agent': 'TuneBarValidator/1.0'})
    with urllib.request.urlopen(req, timeout=5) as resp:
        wav_bytes = resp.read()
    if len(wav_bytes) >= 44:
        with open(retrieved_device_wav_path, "wb") as f:
            f.write(wav_bytes)
        print(f"  [HTTP RETRIEVE] Successfully downloaded {len(wav_bytes)} bytes from {http_url}", flush=True)
        retrieved_ok = True
except Exception as e:
    print(f"  [HTTP RETRIEVE] Note: {e}. Falling back to USB CDC Base64 stream...", flush=True)

if not retrieved_ok:
    while ser.in_waiting:
        ser.read()
    ser.write(b"b64rec\n")
    b64_lines = []
    in_b64 = False
    start_b64 = time.time()
    while time.time() - start_b64 < 15.0:
        if ser.in_waiting:
            line = ser.readline().decode('utf-8', errors='replace').strip()
            if "[B64_START]" in line:
                in_b64 = True
            elif "[B64_END]" in line:
                in_b64 = False
                break
            elif in_b64 and line.startswith("[B64]:"):
                b64_lines.append(line[6:])
        else:
            time.sleep(0.005)
    if b64_lines:
        try:
            raw_bytes = base64.b64decode("".join(b64_lines))
            with open(retrieved_device_wav_path, "wb") as f:
                f.write(raw_bytes)
            print(f"  [USB RETRIEVE] Decoded {len(raw_bytes)} bytes from USB Base64 stream", flush=True)
        except Exception as e:
            print(f"  [USB RETRIEVE] Error: {e}", flush=True)

ser.close()

# -------------------------------------------------------------
# STEP 8: RUN STT TRANSCRIPTION VERIFICATION
# -------------------------------------------------------------
print("\n[7. STT VERIFICATION] Running Speech-to-Text on recordings via auto...", flush=True)

def transcribe(wav_path, label):
    if not os.path.exists(wav_path):
        print(f"  [{label}] File not found: {wav_path}", flush=True)
        return ""
    proc = subprocess.run([sys.executable, "transcribe_auto.py", wav_path], capture_output=True, text=True)
    out = proc.stdout.strip()
    print(f"  --- {label} STT Result ---", flush=True)
    for l in out.splitlines():
        if "Transcription" in l or "Could not" in l or "Error" in l:
            print(f"  {l}", flush=True)
    return out

print("\nVerifying what TuneBar ES7210 microphone recorded:")
stt_device = transcribe(retrieved_device_wav_path, "TuneBar Recorded Query")

print("\nVerifying what Laptop Mic heard from TuneBar's physical speaker:")
stt_room = transcribe(room_wav_path, "Device Acoustic Response")

print("\n==================================================================", flush=True)
print("             AUTONOMOUS VALIDATION RUN COMPLETED                 ", flush=True)
print("==================================================================", flush=True)
