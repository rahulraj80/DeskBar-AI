import urllib.request
import time
import os

os.makedirs("scripts/temp_captures", exist_ok=True)
print("1. Navigating to utility...")
urllib.request.urlopen('http://192.168.3.4/api/screen?action=utility', timeout=5)
time.sleep(0.6)

print("2. Tapping Spirit Level button...")
urllib.request.urlopen('http://192.168.3.4/api/touch?x=320&y=76', timeout=5)
time.sleep(0.6)

print("3. Tapping Raw Data tab [01]...")
urllib.request.urlopen('http://192.168.3.4/api/touch?x=300&y=10', timeout=5)
time.sleep(0.6)

print("4. Capturing screenshot...")
data = urllib.request.urlopen('http://192.168.3.4/api/screenshot', timeout=5).read()
with open('scripts/temp_captures/raw_data_current.bmp', 'wb') as f:
    f.write(data)
print("Successfully captured scripts/temp_captures/raw_data_current.bmp (bytes:", len(data), ")")
