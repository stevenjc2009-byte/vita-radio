#!/usr/bin/env python3
"""Step 4: live probe of a random sample of ~300 stations from top1000.

Records: final HTTP status, redirect count, final content-type, playlist
detection (m3u8/pls/m3u), icy-metaint presence, negotiated TLS version, and
whether the https host still works when capped at TLS 1.2 max. Also inspects
ADTS AAC headers and HLS segment types where applicable.
"""
import concurrent.futures
import http.client
import json
import random
import socket
import ssl
import sys
import time
from pathlib import Path
from urllib.parse import urlparse, urljoin

HERE = Path(__file__).resolve().parent
UA = "VitaRadioSurvey/0.1 (measurement script; contact: stevenjc2009@gmail.com)"
TIMEOUT = 8
MAX_BYTES = 32 * 1024
MAX_REDIRECTS = 5
SAMPLE_N = 300
CONCURRENCY = 20
SEED = 42


def make_conn(scheme, host, port, timeout, tls_max=None):
    if scheme == "https":
        ctx = ssl.create_default_context()
        if tls_max is not None:
            ctx.maximum_version = tls_max
        conn = http.client.HTTPSConnection(host, port, timeout=timeout, context=ctx)
    else:
        conn = http.client.HTTPConnection(host, port, timeout=timeout)
    return conn


def read_limited(resp, max_bytes):
    data = b""
    try:
        while len(data) < max_bytes:
            chunk = resp.read(min(4096, max_bytes - len(data)))
            if not chunk:
                break
            data += chunk
    except Exception:
        pass
    return data


def single_request(url, timeout=TIMEOUT, max_bytes=MAX_BYTES, extra_headers=None, tls_max=None):
    """Do one GET (no redirect following), return dict with response details."""
    parsed = urlparse(url)
    scheme = parsed.scheme.lower()
    host = parsed.hostname
    port = parsed.port or (443 if scheme == "https" else 80)
    path = parsed.path or "/"
    if parsed.query:
        path += "?" + parsed.query

    headers = {"User-Agent": UA, "Icy-MetaData": "1", "Connection": "close"}
    if extra_headers:
        headers.update(extra_headers)

    conn = make_conn(scheme, host, port, timeout, tls_max=tls_max)
    tls_version = None
    try:
        if scheme == "https":
            # getresponse() clears conn.sock to None once the response is read,
            # so the TLS version must be captured right after the handshake
            # (connect() is called implicitly by request() if not already connected).
            conn.connect()
            sock = getattr(conn, "sock", None)
            if sock is not None and hasattr(sock, "version"):
                tls_version = sock.version()
        conn.request("GET", path, headers=headers)
        resp = conn.getresponse()
        status = resp.status
        resp_headers = {k.lower(): v for k, v in resp.getheaders()}
        body = read_limited(resp, max_bytes)
        return {
            "ok": True,
            "status": status,
            "headers": resp_headers,
            "body": body,
            "tls_version": tls_version,
            "location": resp_headers.get("location"),
        }
    except Exception as e:
        return {"ok": False, "error": f"{type(e).__name__}: {e}"}
    finally:
        try:
            conn.close()
        except Exception:
            pass


def follow_and_probe(url):
    """Follow redirects manually, capping at MAX_REDIRECTS, returning final result."""
    current = url
    redirects = 0
    last_err = None
    history = []
    while redirects <= MAX_REDIRECTS:
        result = single_request(current)
        if not result["ok"]:
            last_err = result["error"]
            return {"ok": False, "error": last_err, "redirects": redirects, "history": history, "final_url": current}
        history.append({"url": current, "status": result["status"]})
        if result["status"] in (301, 302, 303, 307, 308) and result.get("location"):
            current = urljoin(current, result["location"])
            redirects += 1
            continue
        result["redirects"] = redirects
        result["final_url"] = current
        result["history"] = history
        return result
    return {"ok": False, "error": "too many redirects", "redirects": redirects, "history": history, "final_url": current}


def classify_playlist(content_type, body_text, url):
    ct = (content_type or "").lower()
    lower_url = url.lower()
    is_m3u8 = ("mpegurl" in ct) or lower_url.endswith(".m3u8") or body_text.strip().startswith("#EXTM3U")
    is_pls = ("x-scpls" in ct) or lower_url.endswith(".pls") or body_text.strip().upper().startswith("[PLAYLIST]")
    is_m3u = (not is_m3u8) and (("audio/x-mpegurl" in ct) or lower_url.endswith(".m3u"))
    kind = None
    if is_m3u8:
        kind = "m3u8"
    elif is_pls:
        kind = "pls"
    elif is_m3u:
        kind = "m3u"
    return kind


def adts_profile(byte0, byte1, byte2, byte3):
    if not (byte0 == 0xFF and (byte1 & 0xF0) == 0xF0):
        return None
    profile_bits = (byte2 >> 6) & 0x3
    profile_map = {0: "Main", 1: "LC", 2: "SSR", 3: "LTP"}
    sf_index = (byte2 >> 2) & 0xF
    return {"profile_bits": profile_bits, "profile_name": profile_map.get(profile_bits, "unknown"), "sf_index": sf_index}


def classify_segment_bytes(data):
    if len(data) >= 4 and data[0] == 0x47:
        return "MPEG-TS"
    if len(data) >= 8 and data[4:8] in (b"ftyp", b"styp"):
        return "fMP4"
    if len(data) >= 2 and data[0] == 0xFF and (data[1] & 0xF0) == 0xF0:
        return "raw-ADTS-AAC"
    return f"unknown (first bytes: {data[:8].hex() if data else ''})"


def probe_station(station):
    url = station.get("url_resolved") or station.get("url")
    name = station.get("name")
    uuid = station.get("stationuuid")
    parsed = urlparse(url)
    scheme = parsed.scheme.lower()
    out = {"name": name, "uuid": uuid, "url": url, "codec_field": station.get("codec"), "hls_field": station.get("hls")}

    result = follow_and_probe(url)
    out["redirects"] = result.get("redirects", 0)
    if not result.get("ok"):
        out["error"] = result.get("error")
        out["category"] = classify_error(result.get("error"))
        return out

    out["final_status"] = result["status"]
    out["final_url"] = result["final_url"]
    headers = result.get("headers", {})
    out["content_type"] = headers.get("content-type")
    out["icy_metaint_present"] = "icy-metaint" in headers
    out["icy_metaint_value"] = headers.get("icy-metaint")
    out["tls_version"] = result.get("tls_version")
    out["category"] = "ok" if 200 <= result["status"] < 300 else f"http_{result['status']}"

    body = result.get("body", b"")
    body_text_sample = body[:4096].decode("utf-8", errors="replace")

    playlist_kind = classify_playlist(out["content_type"], body_text_sample, out["final_url"])
    out["playlist_kind"] = playlist_kind

    if playlist_kind == "m3u8":
        lines = body_text_sample.splitlines()
        has_key = any(l.startswith("#EXT-X-KEY") for l in lines)
        out["m3u8_has_ext_x_key"] = has_key
        seg_uri = None
        for l in lines:
            l = l.strip()
            if l and not l.startswith("#"):
                seg_uri = l
                break
        out["m3u8_first_entry"] = seg_uri
        if seg_uri:
            seg_url = urljoin(out["final_url"], seg_uri)
            seg_result = follow_and_probe(seg_url)
            if seg_result.get("ok"):
                seg_body = seg_result.get("body", b"")
                out["segment_type"] = classify_segment_bytes(seg_body)
                out["segment_content_type"] = seg_result.get("headers", {}).get("content-type")
            else:
                out["segment_type"] = f"fetch_failed: {seg_result.get('error')}"
    elif out["content_type"] and ("audio/aac" in out["content_type"].lower() or "audio/aacp" in out["content_type"].lower()):
        if len(body) >= 4:
            adts = adts_profile(body[0], body[1], body[2], body[3])
            out["adts_header"] = adts
            if adts and adts["profile_name"] == "LC":
                out["aac_classification_caveat"] = (
                    "ADTS profile bit says LC, but HE-AAC (SBR) is often signaled "
                    "implicitly out-of-band and is indistinguishable from plain AAC-LC "
                    "by inspecting the ADTS header alone; true AAC-LC vs HE-AAC-SBR "
                    "requires decoding or an out-of-band signal."
                )

    # TLS 1.2 cap test (only meaningful for https)
    if scheme == "https":
        cap_result = single_request(out["final_url"], tls_max=ssl.TLSVersion.TLSv1_2)
        out["tls12_capped_ok"] = bool(cap_result.get("ok")) and 200 <= cap_result.get("status", 0) < 400
        out["tls12_capped_error"] = cap_result.get("error")
        out["tls12_capped_status"] = cap_result.get("status")
        # "fails only at TLS1.2" means: unrestricted attempt succeeded, but TLS1.2-capped attempt failed
        out["tls13_only_suspected"] = (out["category"] == "ok") and (not out["tls12_capped_ok"])

    return out


def classify_error(err):
    if not err:
        return "unknown_error"
    e = err.lower()
    if "timed out" in e or "timeout" in e:
        return "timeout"
    if "name or service not known" in e or "getaddrinfo failed" in e or "nodename nor servname" in e:
        return "dns"
    if "ssl" in e or "certificate" in e or "handshake" in e:
        return "tls_error"
    if "connection refused" in e:
        return "connection_refused"
    if "reset" in e:
        return "connection_reset"
    return "other_error"


def main():
    top1000 = json.loads((HERE / "top1000.json").read_text(encoding="utf-8"))
    random.seed(SEED)
    sample = random.sample(top1000, min(SAMPLE_N, len(top1000)))
    print(f"Probing {len(sample)} stations with concurrency={CONCURRENCY}...", file=sys.stderr)

    results = []
    t0 = time.time()
    with concurrent.futures.ThreadPoolExecutor(max_workers=CONCURRENCY) as ex:
        futs = {ex.submit(probe_station, s): s for s in sample}
        done = 0
        for fut in concurrent.futures.as_completed(futs):
            try:
                r = fut.result()
            except Exception as e:
                s = futs[fut]
                r = {"name": s.get("name"), "uuid": s.get("stationuuid"), "url": s.get("url_resolved"), "error": str(e), "category": "exception"}
            results.append(r)
            done += 1
            if done % 25 == 0:
                print(f"  {done}/{len(sample)} done ({time.time()-t0:.1f}s elapsed)", file=sys.stderr)

    print(f"Probe complete in {time.time()-t0:.1f}s", file=sys.stderr)
    (HERE / "probe_results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"Wrote {len(results)} results to probe_results.json", file=sys.stderr)


if __name__ == "__main__":
    main()
