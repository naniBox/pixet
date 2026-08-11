#!/usr/bin/env python3
"""Generates src/app/pixet.icns - the .app bundle icon.

Kept as a generator rather than a checked-in binary blob nobody can edit: the icon is a
handful of parameters (below), so regenerating it after a tweak is one command, and the
"source" stays reviewable in a diff.

Pure standard library on purpose - no Pillow, no ImageMagick, no extra brew installs to
build the icon. PNG encoding is simple enough with zlib, and macOS ships iconutil to do the
.icns packing.

Usage:  python3 scripts/make-icon.py
Then:   rebuild - CMake copies the .icns into pixet.app/Contents/Resources.

Design notes: a lens/aperture ring, because it has to survive being 16px tall in the Dock
and a menu bar. Anything with fine detail or text turns to mush at that size; a thick ring
on a solid rounded square stays legible. Rendered with 4x supersampling for antialiasing,
which is cheap at these sizes and the only way to avoid visible stair-stepping on the curves.
"""

import math
import os
import struct
import subprocess
import sys
import zlib

# --- design parameters -------------------------------------------------------

BG_TOP = (0x3E, 0x53, 0x82)      # indigo, top of the gradient
BG_BOTTOM = (0x1B, 0x22, 0x38)   # near-black navy, bottom
RING = (0xF2, 0xF5, 0xFA)        # off-white; pure white looks harsh next to the dark bg
ACCENT = (0x5A, 0xC8, 0xE0)      # cyan highlight dot

CORNER_RADIUS = 0.2234           # fraction of the icon's width - matches Apple's squircle-ish look
RING_OUTER = 0.315               # fraction of width
RING_INNER = 0.205
DOT_CENTER = (0.365, 0.355)      # upper-left highlight, in unit coords
DOT_RADIUS = 0.052
MARGIN = 0.055                   # inset so the square doesn't touch the icon edge

SUPERSAMPLE = 4

# The set macOS expects in an .iconset directory: (filename, pixel size).
ICONSET = [
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
]


def write_png(path, width, height, pixels):
    """pixels: flat bytearray of RGBA, row-major, len == width*height*4."""
    rows = bytearray()
    stride = width * 4
    for y in range(height):
        rows.append(0)  # PNG filter type 0 (None) per scanline
        rows.extend(pixels[y * stride:(y + 1) * stride])

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)  # 8-bit RGBA, no interlace
    blob = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
            + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(blob)


def rounded_square_contains(x, y, lo, hi, radius):
    """Point-in-rounded-square, in unit coordinates."""
    if x < lo or x > hi or y < lo or y > hi:
        return False
    # Only the four corner regions need the circular test.
    cx = lo + radius if x < lo + radius else (hi - radius if x > hi - radius else x)
    cy = lo + radius if y < lo + radius else (hi - radius if y > hi - radius else y)
    if cx == x and cy == y:
        return True
    return (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2


def sample(x, y):
    """Colour at unit-coordinate (x, y). Returns (r, g, b, a) with a in 0..255."""
    lo, hi = MARGIN, 1.0 - MARGIN
    if not rounded_square_contains(x, y, lo, hi, CORNER_RADIUS):
        return (0, 0, 0, 0)

    # Vertical gradient background.
    t = (y - lo) / (hi - lo)
    r = int(BG_TOP[0] + (BG_BOTTOM[0] - BG_TOP[0]) * t)
    g = int(BG_TOP[1] + (BG_BOTTOM[1] - BG_TOP[1]) * t)
    b = int(BG_TOP[2] + (BG_BOTTOM[2] - BG_TOP[2]) * t)

    dist = math.hypot(x - 0.5, y - 0.5)
    if RING_INNER <= dist <= RING_OUTER:
        r, g, b = RING

    if math.hypot(x - DOT_CENTER[0], y - DOT_CENTER[1]) <= DOT_RADIUS:
        r, g, b = ACCENT

    return (r, g, b, 255)


def render(size):
    pixels = bytearray(size * size * 4)
    step = 1.0 / (size * SUPERSAMPLE)
    inv = 1.0 / (SUPERSAMPLE * SUPERSAMPLE)
    for py in range(size):
        for px in range(size):
            acc_r = acc_g = acc_b = acc_a = 0
            for sy in range(SUPERSAMPLE):
                y = (py * SUPERSAMPLE + sy + 0.5) * step
                for sx in range(SUPERSAMPLE):
                    x = (px * SUPERSAMPLE + sx + 0.5) * step
                    r, g, b, a = sample(x, y)
                    # Premultiply so partially-covered edge pixels don't pick up colour
                    # from the fully-transparent samples around them.
                    acc_r += r * a
                    acc_g += g * a
                    acc_b += b * a
                    acc_a += a
            i = (py * size + px) * 4
            if acc_a:
                pixels[i + 0] = min(255, int(acc_r / acc_a))
                pixels[i + 1] = min(255, int(acc_g / acc_a))
                pixels[i + 2] = min(255, int(acc_b / acc_a))
            pixels[i + 3] = min(255, int(acc_a * inv))
    return pixels


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    iconset = os.path.join(repo_root, "build", "pixet.iconset")
    out = os.path.join(repo_root, "src", "app", "pixet.icns")
    os.makedirs(iconset, exist_ok=True)

    for name, size in ICONSET:
        print(f"  rendering {name} ({size}x{size})")
        write_png(os.path.join(iconset, name), size, size, render(size))

    subprocess.run(["iconutil", "-c", "icns", iconset, "-o", out], check=True)
    print(f"wrote {out} ({os.path.getsize(out)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
