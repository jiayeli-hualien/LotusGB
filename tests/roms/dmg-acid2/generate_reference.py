#!/usr/bin/env python3
"""Convert the upstream 2-bit grayscale reference into DMG shade indices."""

from pathlib import Path
import struct
import zlib


ASSET_DIR = Path(__file__).resolve().parent
SOURCE = ASSET_DIR / "reference-dmg-v1.0.png"
OUTPUT = ASSET_DIR / "reference-shades.bin"


def paeth(left: int, up: int, upper_left: int) -> int:
    estimate = left + up - upper_left
    distances = (abs(estimate - left), abs(estimate - up), abs(estimate - upper_left))
    return (left, up, upper_left)[distances.index(min(distances))]


def main() -> None:
    png = SOURCE.read_bytes()
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"not a PNG: {SOURCE}")

    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    offset = 8
    while offset < len(png):
        size = struct.unpack_from(">I", png, offset)[0]
        kind = png[offset + 4 : offset + 8]
        data = png[offset + 8 : offset + 8 + size]
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(
                ">IIBBBBB", data
            )
        elif kind == b"IDAT":
            compressed.extend(data)
        offset += size + 12

    if (width, height, bit_depth, color_type, interlace) != (160, 144, 2, 0, 0):
        raise ValueError("expected a 160x144, non-interlaced, 2-bit grayscale PNG")

    row_size = (width * bit_depth + 7) // 8
    raw = zlib.decompress(compressed)
    previous = bytearray(row_size)
    shades = bytearray()
    offset = 0
    for _ in range(height):
        filter_type = raw[offset]
        encoded = raw[offset + 1 : offset + 1 + row_size]
        offset += row_size + 1
        row = bytearray(row_size)
        for i, value in enumerate(encoded):
            left = row[i - 1] if i else 0
            up = previous[i]
            upper_left = previous[i - 1] if i else 0
            if filter_type == 0:
                predictor = 0
            elif filter_type == 1:
                predictor = left
            elif filter_type == 2:
                predictor = up
            elif filter_type == 3:
                predictor = (left + up) // 2
            elif filter_type == 4:
                predictor = paeth(left, up, upper_left)
            else:
                raise ValueError(f"unsupported PNG filter {filter_type}")
            row[i] = (value + predictor) & 0xFF

        for x in range(width):
            sample = (row[x // 4] >> (6 - 2 * (x % 4))) & 0x03
            shades.append(3 - sample)
        previous = row

    OUTPUT.write_bytes(shades)
    print(f"wrote {len(shades)} shade indices to {OUTPUT}")


if __name__ == "__main__":
    main()
