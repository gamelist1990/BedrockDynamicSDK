"""Generate the demo's embedded PNG without an imaging dependency."""
import struct
import sys
import zlib
from pathlib import Path


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def pixel(x, y):
    # Draw a lit wireframe crystal over a dark checkerboard.
    d = abs(x - 120) * 1.4 + abs(y - 65)
    if 48 < d < 54:
        return (168, 255, 223)
    if d <= 48:
        return (44, 160, 127) if x > 120 else (78, 209, 158)
    return (22, 38, 51) if (x // 12 + y // 12) % 2 else (18, 28, 39)


width, height = 240, 135
rows = b"".join(b"\0" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))
png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(rows, 9))
    + chunk(b"IEND", b"")
)
lines = [", ".join(hex(b) for b in png[i : i + 16]) for i in range(0, len(png), 16)]
Path(sys.argv[1]).write_text(
    "#pragma once\ninline constexpr unsigned char demo_preview_png[] = {\n    "
    + ",\n    ".join(lines) + "\n};\n", encoding="ascii"
)
