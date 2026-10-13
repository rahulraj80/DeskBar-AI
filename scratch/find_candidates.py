import urllib.request
import json
import ssl
import time

def find_and_test_candidates():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE

    def fetch_stations(params):
        url = f"https://de1.api.radio-browser.info/json/stations/search?{params}"
        req = urllib.request.Request(url, headers={"User-Agent": "TuneBar-StreamAuditor/2.0"})
        with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
            return json.loads(resp.read().decode('utf-8'))

    print("Fetching top Hindi stations...")
    hindi_stations = fetch_stations("language=hindi&order=clickcount&reverse=true&hidebroken=true&limit=15")
    
    print("Fetching top English Rock / Classic Hits stations...")
    english_stations = fetch_stations("language=english&tag=classic,rock&order=clickcount&reverse=true&hidebroken=true&limit=15")
    
    candidates_to_test = []
    for s in hindi_stations:
        candidates_to_test.append({
            "target": "Hindi Replacement",
            "name": s["name"],
            "url": s["url_resolved"],
            "codec": s["codec"],
            "bitrate": s["bitrate"]
        })
        
    for s in english_stations:
        candidates_to_test.append({
            "target": "English Replacement",
            "name": s["name"],
            "url": s["url_resolved"],
            "codec": s["codec"],
            "bitrate": s["bitrate"]
        })
        
    print(f"Total candidates gathered: {len(candidates_to_test)}")
    
    verified_results = []
    
    for cand in candidates_to_test:
        url = cand["url"]
        name = cand["name"].encode('ascii', 'ignore').decode('ascii')
        print(f"\nEvaluating: {name} ({cand['target']}, declared {cand['bitrate']}k {cand['codec']})")
        print(f"URL: {url}")
        
        req = urllib.request.Request(url, headers={
            "User-Agent": "TuneBar-ESP32S3/2.0.4",
            "Icy-MetaData": "1"
        })
        t0 = time.time()
        try:
            with urllib.request.urlopen(req, context=ctx, timeout=6) as resp:
                connect_time = time.time() - t0
                ct = resp.headers.get("Content-Type", "")
                icy_br = resp.headers.get("icy-br", None)
                print(f"  Connected in {connect_time:.2f}s! Content-Type: {ct}, icy-br: {icy_br}")
                
                # Must be direct stream, not m3u8 playlist or html
                if "html" in ct.lower() or "vnd.apple.mpegurl" in ct.lower():
                    print("  Skipping: not direct mp3/aac audio stream.")
                    continue
                    
                bytes_read = 0
                t_start = time.time()
                while time.time() - t_start < 20: # 20 seconds test
                    chunk = resp.read(4096)
                    if not chunk:
                        break
                    bytes_read += len(chunk)
                
                elapsed = time.time() - t_start
                if elapsed < 15:
                    print("  Stream ended early, skipping.")
                    continue
                    
                kbps = (bytes_read * 8) / (elapsed * 1000)
                nom_kbps = float(icy_br) if (icy_br and icy_br.isdigit()) else (float(cand['bitrate']) if cand['bitrate'] > 0 else 128.0)
                diff = kbps - nom_kbps
                print(f"  Result: {elapsed:.1f}s | {bytes_read/1024:.1f} KB | Rate: {kbps:.1f} kbps vs Nominal: {nom_kbps:.0f} kbps (Diff: {diff:+.1f} kbps)")
                
                if diff >= 0:
                    verified_results.append({
                        "target": cand["target"],
                        "name": name,
                        "url": url,
                        "nominal_kbps": nom_kbps,
                        "sustained_kbps": round(kbps, 1),
                        "surplus_kbps": round(diff, 1),
                        "connect_sec": round(connect_time, 2),
                        "content_type": ct
                    })
                    print("  --> SUITABLE REPLACEMENT FOUND!")
        except Exception as e:
            print(f"  Error: {e}")

    with open("c:/Users/rahul/OpenCode/TuneBar/scratch/found_verified.json", "w") as f:
        json.dump(verified_results, f, indent=2)
    print("\nSaved verified candidates to scratch/found_verified.json")

if __name__ == "__main__":
    find_and_test_candidates()
