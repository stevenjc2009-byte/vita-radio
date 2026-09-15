#!/usr/bin/env python3
"""Generate PLACEHOLDER LiveArea / bubble art for Vita Radio (stdlib only).

Outputs (relative to this script's folder), all 8-bit palettized:
  icon0.png                          128x128
  livearea/contents/bg.png           840x500
  livearea/contents/startup.png      280x158

Every image is PNG colour type 3 (palette), bit depth 8, non-interlaced, and
carries only IHDR/PLTE/IDAT/IEND.

Why that exact format: the Vita's package installer refuses non-indexed sce_sys
images with error 0x8010113D at the end of a VPK install. Measured, not assumed
- VitaShell 2.02, Vita Homebrew Browser and VitaDB Downloader all ship every
sce_sys PNG as palette/8, and all three install. 1.0.0 first shipped icon0.png
as colour type 2 (truecolour) and failed; re-encoding all three to RGBA
(type 6) failed with the identical code. Truecolour with or without alpha,
greyscale, 16-bit depth and interlacing are all refused. Replace the art at any
time, but keep the sizes and run the result through pngquant (or this script)
or the package stops installing again.
"""
import struct
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent

PALETTE = [
    (18, 20, 30),     # 0 background
    (40, 44, 64),     # 1 panel
    (255, 170, 40),   # 2 accent (letters)
    (90, 200, 255),   # 3 waves
]


def chunk(tag: bytes, data: bytes) -> bytes:
    return (struct.pack(">I", len(data)) + tag + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))


def write_png(path: Path, w: int, h: int, pix: list) -> None:
    """Write 8-bit palettized (colour type 3), no ancillary chunks."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # filter: none
        raw.extend(pix[y])
    plte = bytearray()
    for r, g, b in PALETTE:
        plte.extend((r, g, b))
    out = b"\x89PNG\r\n\x1a\n"
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
    out += chunk(b"PLTE", bytes(plte))
    out += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    out += chunk(b"IEND", b"")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(out)


def canvas(w: int, h: int) -> list:
    return [bytearray(w) for _ in range(h)]


def rect(pix, x, y, w, h, c):
    H, W = len(pix), len(pix[0])
    for yy in range(max(0, y), min(H, y + h)):
        for xx in range(max(0, x), min(W, x + w)):
            pix[yy][xx] = c


def letters_vr(pix, x, y, s, c):
    """Block 'VR' on a 5-unit-tall grid; s = unit size in pixels."""
    # V: two slanted-ish columns made of stepped blocks meeting at the bottom
    rect(pix, x, y, s, 3 * s, c)
    rect(pix, x + s, y + 3 * s, s, s, c)
    rect(pix, x + 2 * s, y + 4 * s, s, s, c)
    rect(pix, x + 3 * s, y + 3 * s, s, s, c)
    rect(pix, x + 4 * s, y, s, 3 * s, c)
    # R
    rx = x + 6 * s
    rect(pix, rx, y, s, 5 * s, c)
    rect(pix, rx + s, y, 2 * s, s, c)
    rect(pix, rx + 3 * s, y + s, s, s, c)
    rect(pix, rx + s, y + 2 * s, 2 * s, s, c)
    rect(pix, rx + 2 * s, y + 3 * s, s, s, c)
    rect(pix, rx + 3 * s, y + 4 * s, s, s, c)


def waves(pix, cx, cy, radii, thick, c):
    """Right-hand radio arcs (quarter circles facing right)."""
    H, W = len(pix), len(pix[0])
    for r in radii:
        for yy in range(max(0, cy - r), min(H, cy + r + 1)):
            for xx in range(max(0, cx), min(W, cx + r + 1)):
                d2 = (xx - cx) ** 2 + (yy - cy) ** 2
                if (r - thick) ** 2 <= d2 <= r * r and abs(yy - cy) <= (xx - cx):
                    pix[yy][xx] = c


def make(w, h, unit, path):
    pix = canvas(w, h)
    border = max(2, unit // 2)
    rect(pix, border, border, w - 2 * border, h - 2 * border, 1)
    text_w, text_h = 10 * unit, 5 * unit
    wave_r = [unit * 2, unit * 3, unit * 4]
    total_w = text_w + unit + wave_r[-1]
    x0 = (w - total_w) // 2
    y0 = (h - text_h) // 2
    letters_vr(pix, x0, y0, unit, 2)
    waves(pix, x0 + text_w + unit // 2, y0 + text_h // 2, wave_r, max(2, unit // 2), 3)
    write_png(path, w, h, pix)
    print(f"wrote {path} ({w}x{h}, palette/8)")


if __name__ == "__main__":
    make(128, 128, 7, HERE / "icon0.png")
    make(840, 500, 34, HERE / "livearea" / "contents" / "bg.png")
    make(280, 158, 11, HERE / "livearea" / "contents" / "startup.png")
