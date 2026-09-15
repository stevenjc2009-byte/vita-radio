"""Independent reference for tests/test_pkg_meta.c.

Re-implements VitaShell's makeHeadBin() with Python's hashlib (no shared code
with src/pkg_meta.c or src/sha1.c) and writes the expected head.bin for
TITLE_ID VRAD00001 with no CONTENT_ID.

    python tests/data/make_headbin_expected.py
"""
import hashlib
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
TEMPLATE = HERE.parent.parent / "assets" / "head.bin"
OUT = HERE / "headbin_VRAD00001.bin"


def fpkg_hmac(data: bytes) -> bytes:
    sha = hashlib.sha1(data).digest()
    buf = bytearray(64)
    buf[0:8] = sha[4:12]
    buf[8:16] = sha[4:12]
    buf[16:20] = sha[12:16]
    buf[20] = sha[16]
    buf[21] = sha[1]
    buf[22] = sha[2]
    buf[23] = sha[3]
    buf[24:32] = buf[16:24]
    return hashlib.sha1(bytes(buf)).digest()[:16]


def be32(b: bytearray, off: int) -> int:
    return struct.unpack(">I", bytes(b[off:off + 4]))[0]


def main() -> None:
    head = bytearray(TEMPLATE.read_bytes())
    cid = b"EP9000-VRAD00001_00-0000000000000000"
    head[0x30:0x30 + 48] = cid + bytes(48 - len(cid))

    n = be32(head, 0xD0)
    head[n:n + 16] = fpkg_hmac(bytes(head[:n]))

    off, ilen, dst = be32(head, 0x8), be32(head, 0x10), be32(head, 0xD4)
    head[dst:dst + 16] = fpkg_hmac(bytes(head[off:off + ilen - 64]))

    n = be32(head, 0xE8)
    head[n:n + 16] = fpkg_hmac(bytes(head[:n]))

    OUT.write_bytes(bytes(head))
    print(f"wrote {OUT} ({len(head)} bytes), sha1 {hashlib.sha1(head).hexdigest()}")


if __name__ == "__main__":
    main()
