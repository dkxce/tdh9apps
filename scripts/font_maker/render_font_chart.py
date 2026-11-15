#!/usr/bin/env python3
"""Render a PNG chart of every glyph in myapps/shared/common/font.h.

Each 5x7 glyph (bit 4 = left column) is drawn at `SCALE`, laid out in a
`COLS`-wide grid with a scale-sized gap between cells. Same glyph
rasterization as render_lock_screens.py.
"""
import argparse
import os
import re
import struct
import zlib

SCALE = 4
COLS = 16
FG = (0, 0, 0)
BG = (255, 255, 255)

_DEFAULT_FONT = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), "myapps", "shared", "common", "font.h")


def load_font(font_h):
    src = open(font_h, encoding="utf-8").read()
    chars = re.search(r'FONT_CHARS\[\]\s*=\s*"([^"]*)"', src).group(1)
    block = src[src.index("FONT[62][7]") : src.index("};")]
    vals = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", block)]
    font = [vals[i:i + 7] for i in range(0, len(vals), 7)]
    if len(chars) != 62 or len(font) != 62:
        raise ValueError("expected 62 glyphs, got chars=%d font=%d"
                         % (len(chars), len(font)))
    return list(chars), font


def paste_glyph(px, cx, cy, ch, chars, font, scale=SCALE):
    """Draw glyph `ch` at cell origin (cx, cy)."""
    if "a" <= ch <= "z":
        ch = ch.upper()
    try:
        i = chars.index(ch)
    except ValueError:
        i = chars.index("?")
    rows = font[i]
    for py in range(7 * scale):
        row = rows[py // scale]
        for pxlet in range(5 * scale):
            col = pxlet // scale
            on = (row >> (4 - col)) & 1
            xx, yy = cx + pxlet, cy + py
            px[yy][xx] = FG if on else BG


def render_chart_png(font_h, out_png, scale=SCALE, cols=COLS):
    """Render all glyphs of font.h to a PNG chart at out_png. Returns path."""
    chars, font = load_font(font_h)
    rows = (len(chars) + cols - 1) // cols
    sheet_w = (cols - 1) * (6 * scale + scale) + 6 * scale
    sheet_h = rows * 7 * scale + (rows - 1) * scale
    px = [[BG] * sheet_w for _ in range(sheet_h)]
    for i, ch in enumerate(chars):
        cx = (i % cols) * (6 * scale + scale)
        cy = (i // cols) * (7 * scale + scale)
        paste_glyph(px, cx, cy, ch, chars, font, scale)
    raw = bytearray()
    for yy in range(sheet_h):
        raw.append(0)
        for xx in range(sheet_w):
            raw += bytes(px[yy][xx])
    def chunk(tag, data):
        c = tag + data
        return (struct.pack(">I", len(data)) + c
                + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", sheet_w, sheet_h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(out_png, "wb").write(png)
    return out_png


def main():
    ap = argparse.ArgumentParser(description="Render a PNG chart of font.h glyphs.")
    ap.add_argument("--font", default=_DEFAULT_FONT, help="path to font.h")
    ap.add_argument("--out", default=None, help="output png path")
    args = ap.parse_args()
    chars, font = load_font(args.font)

    rows = (62 + COLS - 1) // COLS
    sheet_w = (COLS - 1) * (6 * SCALE + SCALE) + 6 * SCALE
    sheet_h = rows * 7 * SCALE + (rows - 1) * SCALE
    px = [[BG] * sheet_w for _ in range(sheet_h)]
    for i, ch in enumerate(chars):
        cx = (i % COLS) * (6 * SCALE + SCALE)
        cy = (i // COLS) * (7 * SCALE + SCALE)
        paste_glyph(px, cx, cy, ch, chars, font, SCALE)

    raw = bytearray()
    for yy in range(sheet_h):
        raw.append(0)
        for xx in range(sheet_w):
            raw += bytes(px[yy][xx])

    def chunk(tag, data):
        c = tag + data
        return (struct.pack(">I", len(data)) + c
                + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", sheet_w, sheet_h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                   "font_chart.png")
    open(out, "wb").write(png)
    print("wrote %s (%dx%d, %d glyphs in %dx%d grid, scale=%d)"
          % (out, sheet_w, sheet_h, len(chars), COLS, rows, SCALE))


if __name__ == "__main__":
    main()