import urllib.request
import json
import ssl

def fetch_top_english():
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    
    url = "https://de1.api.radio-browser.info/json/stations/search?language=english&tagList=classic%20rock,70s,80s&order=clickcount&reverse=true&hidebroken=true&limit=20"
    req = urllib.request.Request(url, headers={"User-Agent": "TuneBar/2.0"})
    with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
        stations = json.loads(resp.read().decode('utf-8'))
        
    for s in stations:
        print(f"Name: {s['name']} | Codec: {s['codec']} | Bitrate: {s['bitrate']} | URL: {s['url_resolved']}")

if __name__ == "__main__":
    fetch_top_english()
