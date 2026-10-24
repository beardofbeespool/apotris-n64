#!/usr/bin/env python3
"""Convert a GBA (little-endian) asset to the N64's big-endian byte order.

The game reads tile rows as u32 values (pixel k in bits 4k..4k+3) and
palettes, tilemaps and gradients as u16 values. The .bin assets store
those values little-endian, so on the N64 they are swapped here at build
time to match what the code expects. Anything else is copied unchanged.
"""

import os
import sys


def swap(data, width):
    out = bytearray(data)
    for i in range(0, len(data) - len(data) % width, width):
        out[i:i + width] = data[i:i + width][::-1]
    return bytes(out)


def element_width(name):
    if name.endswith('tiles.bin'):
        return 4
    if name.endswith('map.bin') or name.endswith('_pal.bin') or name.endswith('Gradient.bin'):
        return 2
    return 1


def main():
    src, dst = sys.argv[1], sys.argv[2]
    with open(src, 'rb') as f:
        data = f.read()
    width = element_width(os.path.basename(src))
    with open(dst, 'wb') as f:
        f.write(swap(data, width) if width > 1 else data)


if __name__ == '__main__':
    main()
