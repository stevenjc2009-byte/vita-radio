#!/usr/bin/env python3
import json
from collections import Counter
from pathlib import Path
from urllib.parse import urlparse

HERE = Path(__file__).resolve().parent
results = json.loads((HERE / "probe_results.json").read_text(encoding="utf-8"))

n = len(results)
cat_ctr = Counter(r.get("category", "unknown") for r in results)
status_ctr = Counter(r.get("final_status") for r in results if r.get("final_status") is not None)
ct_ctr = Counter((r.get("content_type") or "none").split(";")[0].strip().lower() for r in results if r.get("category") == "ok")
playlist_ctr = Counter(r.get("playlist_kind") for r in results if r.get("playlist_kind"))
icy_ctr = Counter(r.get("icy_metaint_present") for r in results if r.get("category") == "ok")
tls_ver_ctr = Counter(r.get("tls_version") for r in results if r.get("tls_version"))
redirect_ctr = Counter(r.get("redirects", 0) for r in results if r.get("category") not in ("exception",))

https_results = [r for r in results if urlparse(r.get("url") or "").scheme == "https"]
tls12_ok_ctr = Counter(r.get("tls12_capped_ok") for r in https_results if "tls12_capped_ok" in r)
tls13_only = [r for r in https_results if r.get("tls13_only_suspected")]

error_cats = Counter(r.get("category") for r in results if r.get("category") != "ok" and not str(r.get("category", "")).startswith("http_"))
http_err_cats = Counter(r.get("category") for r in results if str(r.get("category", "")).startswith("http_"))

adts_stations = [r for r in results if r.get("adts_header")]

def reclassify_segment(seg_type):
    if not seg_type:
        return seg_type
    if seg_type.startswith("unknown (first bytes: "):
        hexstr = seg_type[len("unknown (first bytes: "):-1]
        try:
            raw = bytes.fromhex(hexstr)
        except ValueError:
            return seg_type
        if raw.startswith(b"#EXTM3U"):
            return "nested-m3u8 (first entry was a variant playlist, not a media segment)"
        if raw.startswith(b"\x1f\x8b"):
            return "gzip-compressed body (likely playlist/text, not raw media)"
        return seg_type
    return seg_type

seg_type_ctr = Counter(reclassify_segment(r.get("segment_type")) for r in results if r.get("segment_type"))
m3u8_key_ctr = Counter(r.get("m3u8_has_ext_x_key") for r in results if "m3u8_has_ext_x_key" in r)

summary = {
    "n_probed": n,
    "category_counts": dict(cat_ctr),
    "final_status_counts": {str(k): v for k, v in status_ctr.items()},
    "content_type_counts_ok_only": dict(ct_ctr),
    "playlist_kind_counts": dict(playlist_ctr),
    "icy_metaint_present_counts_ok_only": {str(k): v for k, v in icy_ctr.items()},
    "tls_version_counts": dict(tls_ver_ctr),
    "redirect_count_distribution": {str(k): v for k, v in redirect_ctr.items()},
    "https_count": len(https_results),
    "tls12_capped_ok_counts": {str(k): v for k, v in tls12_ok_ctr.items()},
    "tls13_only_suspected_count": len(tls13_only),
    "tls13_only_suspected_examples": [{"name": r["name"], "url": r["url"], "tls_version": r.get("tls_version"), "tls12_error": r.get("tls12_capped_error")} for r in tls13_only[:10]],
    "non_ok_non_http_error_categories": dict(error_cats),
    "http_error_status_categories": dict(http_err_cats),
    "adts_direct_stream_count": len(adts_stations),
    "adts_profile_counts": dict(Counter(r["adts_header"]["profile_name"] for r in adts_stations)),
    "hls_segment_type_counts": dict(seg_type_ctr),
    "m3u8_ext_x_key_counts": {str(k): v for k, v in m3u8_key_ctr.items()},
}

(HERE / "probe_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
print(json.dumps(summary, indent=2))
