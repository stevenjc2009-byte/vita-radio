#!/usr/bin/env python3
"""Step 1-2: discover a radio-browser.info API server, pull station list + codecs."""
import json
import socket
import ssl
import urllib.request
import urllib.error
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
UA = "VitaRadioSurvey/0.1 (measurement script; contact: stevenjc2009@gmail.com)"

def discover_servers():
    """Resolve all.api.radio-browser.info via DNS to get candidate mirror IPs/hosts."""
    hosts = []
    try:
        infos = socket.getaddrinfo("all.api.radio-browser.info", 443, proto=socket.IPPROTO_TCP)
        ips = sorted(set(i[4][0] for i in infos))
        print(f"DNS resolved all.api.radio-browser.info -> {ips}", file=sys.stderr)
    except Exception as e:
        print(f"DNS resolution failed: {e}", file=sys.stderr)
        ips = []
    # Also try the documented JSON server list via a direct request to the round-robin name.
    try:
        req = urllib.request.Request(
            "https://all.api.radio-browser.info/json/servers",
            headers={"User-Agent": UA},
        )
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = json.loads(resp.read().decode("utf-8"))
        hosts = [d["name"] for d in data if "name" in d]
        print(f"/json/servers returned {len(hosts)} hosts: {hosts}", file=sys.stderr)
    except Exception as e:
        print(f"/json/servers fetch failed: {e}", file=sys.stderr)
    return hosts


def http_get_json(url, timeout=60):
    req = urllib.request.Request(url, headers={"User-Agent": UA, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main():
    hosts = discover_servers()
    if not hosts:
        hosts = ["de1.api.radio-browser.info", "nl1.api.radio-browser.info", "at1.api.radio-browser.info"]
        print(f"Falling back to known static hostnames: {hosts}", file=sys.stderr)

    base = None
    for h in hosts:
        try:
            test_url = f"https://{h}/json/stats"
            stats = http_get_json(test_url, timeout=10)
            print(f"Server {h} OK: {stats}", file=sys.stderr)
            base = h
            break
        except Exception as e:
            print(f"Server {h} failed: {e}", file=sys.stderr)
            continue

    if base is None:
        print("FATAL: no working radio-browser server found", file=sys.stderr)
        sys.exit(1)

    print(f"Using base server: {base}", file=sys.stderr)

    # Top 1000 by clickcount (hidebroken=true)
    top_url = f"https://{base}/json/stations?hidebroken=true&order=clickcount&reverse=true&limit=1000"
    print("Fetching top 1000 stations by clickcount...", file=sys.stderr)
    top1000 = http_get_json(top_url, timeout=60)
    print(f"Got {len(top1000)} top stations", file=sys.stderr)
    (HERE / "top1000.json").write_text(json.dumps(top1000), encoding="utf-8")

    # Full non-broken list (could be tens of thousands)
    full_url = f"https://{base}/json/stations?hidebroken=true"
    print("Fetching full non-broken station list (this may take a while)...", file=sys.stderr)
    try:
        full = http_get_json(full_url, timeout=180)
        print(f"Got {len(full)} total non-broken stations", file=sys.stderr)
        (HERE / "all_stations.json").write_text(json.dumps(full), encoding="utf-8")
    except Exception as e:
        print(f"Full list fetch failed ({e}); falling back to limit=10000", file=sys.stderr)
        full_url2 = f"https://{base}/json/stations?hidebroken=true&order=clickcount&reverse=true&limit=10000"
        full = http_get_json(full_url2, timeout=120)
        print(f"Got {len(full)} stations (limit=10000 fallback)", file=sys.stderr)
        (HERE / "all_stations.json").write_text(json.dumps(full), encoding="utf-8")

    # Codecs list
    codecs_url = f"https://{base}/json/codecs"
    codecs = http_get_json(codecs_url, timeout=30)
    (HERE / "codecs.json").write_text(json.dumps(codecs), encoding="utf-8")
    print(f"Codecs: {codecs}", file=sys.stderr)

    (HERE / "server_used.txt").write_text(base, encoding="utf-8")
    print("DONE", file=sys.stderr)


if __name__ == "__main__":
    main()
