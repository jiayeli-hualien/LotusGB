#!/usr/bin/env python3
"""Convert pinned upstream indexed PNG to little-endian RGB555 (stdlib only)."""
from pathlib import Path
import struct
import zlib
ROOT = Path(__file__).resolve().parent

def decode_reference():
    data = (ROOT / 'reference-cgb-v1.1.png').read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError('invalid PNG signature')
    offset = 8
    compressed = bytearray()
    palette = None
    shape = None
    while offset < len(data):
        size, = struct.unpack_from('>I', data, offset)
        kind = data[offset+4:offset+8]
        chunk = data[offset+8:offset+8+size]
        crc, = struct.unpack_from('>I', data, offset+8+size)
        if zlib.crc32(kind+chunk) & 0xFFFFFFFF != crc:
            raise ValueError('invalid PNG CRC')
        if kind == b'IHDR': shape = struct.unpack('>IIBBBBB', chunk)
        elif kind == b'PLTE': palette = chunk
        elif kind == b'IDAT': compressed.extend(chunk)
        offset += size+12
    if shape != (160,144,4,3,0,0,0) or palette is None:
        raise ValueError('expected 160x144 noninterlaced 4-bit palette PNG')
    raw = zlib.decompress(compressed)
    if len(raw) != 144*81:
        raise ValueError('invalid PNG scanline length')
    previous = bytearray(80)
    colors = bytearray()
    rgb = bytearray()
    for y in range(144):
        mode = raw[y*81]
        row = bytearray(80)
        for x, value in enumerate(raw[y*81+1:(y+1)*81]):
            left = row[x-1] if x else 0
            up = previous[x]
            upper_left = previous[x-1] if x else 0
            estimate = left+up-upper_left
            distances = [abs(estimate-left),abs(estimate-up),abs(estimate-upper_left)]
            paeth = (left,up,upper_left)[distances.index(min(distances))]
            if mode > 4: raise ValueError('unsupported PNG filter')
            row[x] = (value+(0,left,up,(left+up)//2,paeth)[mode]) & 255
        for x in range(160):
            index = (row[x//2] >> (0 if x&1 else 4)) & 15
            r,g,b = palette[index*3:index*3+3]
            colors.extend(struct.pack('<H',(r>>3)|((g>>3)<<5)|((b>>3)<<10)))
            rgb.extend((r,g,b))
        previous = row
    return colors,rgb

if __name__ == '__main__':
    colors,_ = decode_reference()
    (ROOT/'reference-rgb555.bin').write_bytes(colors)
    print(f'wrote {len(colors)//2} RGB555 pixels')
