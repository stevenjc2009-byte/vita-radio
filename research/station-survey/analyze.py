#!/usr/bin/env python3
"""Step 3: distributions over (a) all non-broken stations and (b) top 1000 by clickcount."""
import json
from collections import Counter
from pathlib import Path
from urllib.parse import urlparse

HERE = Path(__file__).resolve().parent


def bitrate_bucket(b):
    try:
        b = int(b)
    except (TypeError, ValueError):
        return "unknown"
    if b <= 0:
        return "0/unknown"
    if b < 64:
        return "<64"
    if b < 96:
        return "64-95"
    if b < 128:
        return "96-127"
    if b < 192:
        return "128-191"
    if b < 256:
        return "192-255"
    return ">=256"


def analyze(stations, label):
    n = len(stations)
    codec_ctr = Counter()
    hls_ctr = Counter()
    scheme_ctr = Counter()
    bitrate_ctr = Counter()
    sslerr_ctr = Counter()

    for s in stations:
        codec = (s.get("codec") or "UNKNOWN").strip().upper()
        if codec == "":
            codec = "UNKNOWN"
        codec_ctr[codec] += 1

        hls_ctr[int(s.get("hls") or 0)] += 1

        url = s.get("url_resolved") or s.get("url") or ""
        scheme = urlparse(url).scheme.lower() if url else "none"
        scheme_ctr[scheme] += 1

        bitrate_ctr[bitrate_bucket(s.get("bitrate"))] += 1

        sslerr_ctr[int(s.get("ssl_error") or 0)] += 1

    def pct(count):
        return round(100.0 * count / n, 2) if n else 0.0

    result = {
        "label": label,
        "n": n,
        "codec": {k: {"count": v, "pct": pct(v)} for k, v in codec_ctr.most_common()},
        "hls": {str(k): {"count": v, "pct": pct(v)} for k, v in sorted(hls_ctr.items())},
        "scheme": {k: {"count": v, "pct": pct(v)} for k, v in scheme_ctr.most_common()},
        "bitrate_bucket": {k: {"count": v, "pct": pct(v)} for k, v in bitrate_ctr.most_common()},
        "ssl_error": {str(k): {"count": v, "pct": pct(v)} for k, v in sorted(sslerr_ctr.items())},
    }
    return result


def main():
    top1000 = json.loads((HERE / "top1000.json").read_text(encoding="utf-8"))
    all_path = HERE / "all_stations.json"
    all_stations = json.loads(all_path.read_text(encoding="utf-8")) if all_path.exists() else []

    out = {}
    if all_stations:
        out["all_nonbroken"] = analyze(all_stations, f"all non-broken (n={len(all_stations)})")
    out["top1000"] = analyze(top1000, "top 1000 by clickcount")

    (HERE / "distributions.json").write_text(json.dumps(out, indent=2), encoding="utf-8")
    print(json.dumps(out, indent=2))


if __name__ == "__main__":
    main()
