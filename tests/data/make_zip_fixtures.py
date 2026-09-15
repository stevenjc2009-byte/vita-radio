"""Writes the zip fixtures for tests/test_zip_extract.c with Python's zipfile.

    python tests/data/make_zip_fixtures.py
"""
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
A_TXT = b"hello vita radio\n"
B_BIN = bytes((i * 31 + 7) & 0xFF for i in range(200000))


def write_zip(path: Path, entries) -> bytes:
    with zipfile.ZipFile(path, "w") as z:
        for name, data, method in entries:
            z.writestr(zipfile.ZipInfo(name) if name.endswith("/") else name, data, compress_type=method)
    return path.read_bytes()


def flip(raw: bytes, offset: int) -> bytes:
    b = bytearray(raw)
    b[offset] ^= 0x55
    return bytes(b)


def main() -> None:
    good = write_zip(HERE / "zip_good.zip", [
        ("emptydir/", b"", zipfile.ZIP_STORED),
        ("a.txt", A_TXT, zipfile.ZIP_STORED),
        ("sub/dir/b.bin", B_BIN, zipfile.ZIP_DEFLATED),
    ])
    write_zip(HERE / "zip_evil.zip", [
        ("ok.txt", b"ok", zipfile.ZIP_STORED),
        ("../evil.txt", b"evil", zipfile.ZIP_STORED),
    ])

    stored = write_zip(HERE / "zip_crc.zip", [("a.txt", A_TXT, zipfile.ZIP_STORED)])
    (HERE / "zip_crc.zip").write_bytes(flip(stored, stored.find(A_TXT) + 3))

    deflated = write_zip(HERE / "zip_deflate_bad.zip", [("b.bin", B_BIN, zipfile.ZIP_DEFLATED)])
    (HERE / "zip_deflate_bad.zip").write_bytes(flip(deflated, 30 + len("b.bin") + 40))

    (HERE / "zip_truncated.zip").write_bytes(good[:100])
    print("fixtures written to", HERE)


if __name__ == "__main__":
    main()
