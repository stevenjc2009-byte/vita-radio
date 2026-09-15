#!/usr/bin/env python3
"""Paginate through the full non-broken station list in chunks."""
import json
import time
import urllib.request
from pathlib import Path

HERE = Path(__file__).resolve().parent
UA = "VitaRadioSurvey/0.1 (measurement script; contact: stevenjc2009@gmail.com)"
BASE = (HERE / "server_used.txt").read_text(encoding="utf-8").strip()

CHUNK = 5000

def fetch(offset):
    url = (
        f"https://{BASE}/json/stations?hidebroken=true&order=name&reverse=false"
        f"&limit={CHUNK}&offset={offset}"
    )
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=90) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main():
    all_stations = []
    offset = 0
    seen_ids = set()
    while True:
        print(f"Fetching offset={offset} limit={CHUNK}...", flush=True)
        try:
            chunk = fetch(offset)
        except Exception as e:
            print(f"  chunk failed: {e}; retrying once after backoff", flush=True)
            time.sleep(3)
            try:
                chunk = fetch(offset)
            except Exception as e2:
                print(f"  retry failed: {e2}; stopping pagination here", flush=True)
                break
        if not chunk:
            print("  empty chunk, done", flush=True)
            break
        new = 0
        for s in chunk:
            sid = s.get("stationuuid")
            if sid and sid not in seen_ids:
                seen_ids.add(sid)
                all_stations.append(s)
                new += 1
        print(f"  got {len(chunk)} rows, {new} new, total so far {len(all_stations)}", flush=True)
        if len(chunk) < CHUNK:
            print("  short chunk, reached end", flush=True)
            break
        offset += CHUNK

    (HERE / "all_stations.json").write_text(json.dumps(all_stations), encoding="utf-8")
    print(f"DONE: wrote {len(all_stations)} stations to all_stations.json", flush=True)


if __name__ == "__main__":
    main()
