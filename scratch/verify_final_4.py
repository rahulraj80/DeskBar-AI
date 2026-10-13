import urllib.request
import ssl
import time

CANDIDATES = [
    # Replace Bombay Beats (HI, Station 3)
    {"id": "HI_3", "target_name": "Bombay Beats Radio", "url": "https://strmreg.1.fm/bombaybeats_mobile_mp3"},
    {"id": "HI_3_ALT", "target_name": "Radio Mirchi Hindi", "url": "https://eu8.fastcast4u.com/proxy/clyedupq/stream"},
    
    # Replace Classic Vinyl HD (EN, Station 3)
    {"id": "EN_3", "target_name": "Radio Paradise Rock", "url": "https://stream.radioparadise.com/rock-128"},
    
    # Replace Classic Hits 70s 80s (EN, Station 7)
    {"id": "EN_7", "target_name": "RdMix Classic Rock", "url": "https://cast1.torontocast.com:4610/stream"},
    {"id": "EN_7_ALT", "target_name": "Laut.fm 70s 80s 90s", "url": "https://stream.laut.fm/70s-80s-90s-riw-vintage-channel"},
    
    # Replace WALM 2 HD (EN, Station 8)
    {"id": "EN_8", "target_name": "Radio Paradise Main", "url": "https://stream.radioparadise.com/mp3-128"},
    {"id": "EN_8_ALT", "target_name": "Radio Caroline 259", "url": "https://happy.radiocaroline.nl/listen/classic_rock/radio128.mp3"}
]

def run_test():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    
    for c in CANDIDATES:
        print(f"\n==========================================")
        print(f"Testing Candidate: {c['target_name']} ({c['id']})")
        print(f"URL: {c['url']}")
        t0 = time.time()
        try:
            req = urllib.request.Request(c['url'], headers={"User-Agent": "TuneBar-ESP32S3/2.0.4", "Icy-MetaData": "1"})
            with urllib.request.urlopen(req, context=ctx, timeout=8) as resp:
                conn_time = time.time() - t0
                ct = resp.headers.get("Content-Type", "")
                icy_br = resp.headers.get("icy-br", "128")
                print(f"Connected in {conn_time:.2f}s | Content-Type: {ct} | icy-br: {icy_br}")
                
                bytes_read = 0
                t_start = time.time()
                while time.time() - t_start < 25:
                    chunk = resp.read(4096)
                    if not chunk:
                        print("Stream finished early!")
                        break
                    bytes_read += len(chunk)
                
                elapsed = time.time() - t_start
                rate_kbps = (bytes_read * 8) / (elapsed * 1000)
                nom_kbps = float(icy_br) if (icy_br and icy_br.isdigit()) else 128.0
                diff = rate_kbps - nom_kbps
                print(f"RESULT: {elapsed:.1f}s | {bytes_read/1024:.1f} KB | Rate: {rate_kbps:.1f} kbps (Nominal: {nom_kbps:.0f} kbps, Diff: {diff:+.1f} kbps)")
                print(f"VERDICT: {'>>> PASS (HEALTHY SURPLUS)' if diff >= 0 else '>>> FAIL (DEFICIT)'}")
        except Exception as e:
            print(f"ERROR: {e}")

if __name__ == "__main__":
    run_test()
