import time
import urllib.request
import ssl
import json
import socket

# Candidates to evaluate
CANDIDATES = [
    # Hindi replacements for Bombay Beats India
    {"name": "Radio City Hindi Hits", "genre": "Hindi", "url": "https://prclive1.listenon.in/Hindi"},
    {"name": "Meethi Mirchi", "genre": "Hindi", "url": "https://drive.uber.radio/uber/meethimirchi/icecast.audio"},
    {"name": "Mirchi 90s Radio", "genre": "Hindi", "url": "https://drive.uber.radio/uber/bollywood90s/icecast.audio"},
    {"name": "Radio City Freedom", "genre": "Hindi", "url": "https://prclive1.listenon.in/Freedom"},
    {"name": "Big FM 92.7", "genre": "Hindi", "url": "https://drive.uber.radio/uber/bigfm/icecast.audio"},
    
    # English replacements for Classic Vinyl HD / WALM 2 HD
    {"name": "Radio Paradise Rock Mix", "genre": "English Rock", "url": "https://stream.radioparadise.com/rock-128"},
    {"name": "Radio Paradise Main", "genre": "English Eclectic", "url": "https://stream.radioparadise.com/mp3-128"},
    {"name": "Classic Rock Florida", "genre": "English Rock", "url": "http://streams.crucialp.com:8000/classicrock"},
    {"name": "Linn Classic", "genre": "English Classical", "url": "http://radio.linn.co.uk:8000/autodj"},
    {"name": "Virgin Radio UK", "genre": "English Pop/Rock", "url": "https://radio.virginradio.co.uk/stream"},

    # English replacements for Classic Hits 70s 80s
    {"name": "Exclusive Radio 70s Hits", "genre": "English 70s", "url": "https://streaming.exclusive.radio/er/classichits/icecast.audio"},
    {"name": "I Love Radio 80s", "genre": "English 80s", "url": "https://streams.ilovemusic.de/iloveradio16.mp3"},
    {"name": "Hits 70s 80s Exclusive", "genre": "English 80s", "url": "https://streaming.exclusive.radio/er/80shits/icecast.audio"},
]

def test_stream(info, duration=30):
    url = info["url"]
    name = info["name"]
    print(f"\n--- Testing '{name}' ({info['genre']}) ---")
    print(f"URL: {url}")
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    
    headers = {
        "User-Agent": "TuneBar-ESP32S3/2.0.4 (ESP32-S3 Arduino Core)",
        "Icy-MetaData": "1"
    }
    req = urllib.request.Request(url, headers=headers)
    t0 = time.time()
    try:
        with urllib.request.urlopen(req, context=ctx, timeout=8) as response:
            connect_time = time.time() - t0
            content_type = response.headers.get("Content-Type", "unknown")
            icy_br = response.headers.get("icy-br", None)
            icy_name = response.headers.get("icy-name", "unknown")
            print(f"Connected in {connect_time:.2f}s! Content-Type: {content_type}, icy-br: {icy_br}, icy-name: {icy_name}")
            
            bytes_read = 0
            start_streaming = time.time()
            last_report = start_streaming
            
            while time.time() - start_streaming < duration:
                chunk = response.read(4096)
                if not chunk:
                    print("Stream EOF encountered!")
                    break
                bytes_read += len(chunk)
                now = time.time()
                if now - last_report >= 5.0:
                    elapsed = now - start_streaming
                    kbps = (bytes_read * 8) / (elapsed * 1000)
                    print(f"  [{elapsed:4.1f}s] Read: {bytes_read/1024:6.1f} KB | Rate: {kbps:5.1f} kbps")
                    last_report = now
                    
            total_elapsed = time.time() - start_streaming
            overall_kbps = (bytes_read * 8) / (total_elapsed * 1000)
            nominal_kbps = float(icy_br) if (icy_br and icy_br.isdigit()) else 128.0
            deficit_surplus = overall_kbps - nominal_kbps
            
            print(f"SUMMARY for '{name}':")
            print(f"  Duration: {total_elapsed:.1f}s | Bytes: {bytes_read:,} ({bytes_read/1024:.1f} KB)")
            print(f"  Sustained Rate: {overall_kbps:.1f} kbps | Nominal: {nominal_kbps:.0f} kbps")
            print(f"  Surplus/Deficit: {deficit_surplus:+.1f} kbps ({'+PASS' if deficit_surplus >= 0 else '-FAIL'})")
            
            return {
                "name": name,
                "url": url,
                "genre": info["genre"],
                "content_type": content_type,
                "nominal_kbps": nominal_kbps,
                "sustained_kbps": round(overall_kbps, 1),
                "surplus_kbps": round(deficit_surplus, 1),
                "pass": deficit_surplus >= 0,
                "connect_sec": round(connect_time, 2)
            }
    except Exception as e:
        print(f"FAILED: {e}")
        return {
            "name": name,
            "url": url,
            "genre": info["genre"],
            "error": str(e),
            "pass": False
        }

if __name__ == "__main__":
    results = []
    for cand in CANDIDATES:
        res = test_stream(cand, duration=25)
        results.append(res)
    
    with open("c:/Users/rahul/OpenCode/TuneBar/scratch/candidate_results.json", "w") as f:
        json.dump(results, f, indent=2)
    print("\n\nAll tests completed. Results saved to scratch/candidate_results.json")
