#!/usr/bin/env python3
"""Render TD-H9 lock-app screens as PNGs, pixel-identical to passlock.c / menu.c.

Reproduces gfx_text / gfx_fill exactly: the 5x7 font is parsed from
myapps/shared/common/font.h (bit 4 of each row byte = left column), a glyph is
drawn as a 6x8 cell multiplied by `scale`, and colors are the GFX_* RGB565
values from gfx.h converted to 24-bit. Coordinates and strings match the app.

Outputs <project>/scripts/*.png:
  screen_radio_lock_unlock.png        unlock prompt (boot, skip_check==2)
  screen_radio_lock_menu.png          lock menu, LOCK selected
  screen_radio_lock_menu_selected.png lock menu, LOCK NOW selected
  screen_new_passw.png                NEW PASSW entry
  screen_radio_widget.png             toolbox menu list, first item selected
  screen_radio_widget_selected.png    toolbox menu list, second item selected
"""
import os
import re
import struct
import zlib

W, H = 160, 128

# GFX colors (RGB565) -> names
COLORS = {"black": 0x0000, "white": 0xffff, "yellow": 0xffe0, "blue": 0x001f,
          "grey": 0x8410, "green": 0x07e0, "red": 0xf800}


def rgb24(c565):
    """Expand a 565 to (r, g, b), reproducing gfx.c's 8-bit conversion."""
    r, g, b = (c565 >> 11) & 0x1f, (c565 >> 5) & 0x3f, c565 & 0x1f
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


# --- parse the real 5x7 font from font.h --------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))
# script sits at <project>/scripts/render_lock_screens/ -> two levels up is the project root
_PROJ = os.path.dirname(os.path.dirname(_HERE))
_FONT_H = os.path.join(_PROJ, "myapps", "shared", "common", "font.h")
_FONT_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-.:>/?,()*#!=+_@&%<[];$|~"


def _load_font():
    src = open(_FONT_H, encoding="utf-8").read()
    block = src[src.index("FONT[62][7]") : src.index("};")]
    vals = [int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", block)]
    font = [vals[i:i + 7] for i in range(0, len(vals), 7)]
    if len(font) != 62:
        raise ValueError("expected 62 glyphs, got %d" % len(font))
    return font


FONT = _load_font()


class Screen:
    def __init__(self):
        self.px = [[rgb24(COLORS["black"])] * W for _ in range(H)]

    def fill(self, x, y, w, h, color):
        c = rgb24(COLORS[color])
        for yy in range(y, min(y + h, H)):
            for xx in range(x, min(x + w, W)):
                self.px[yy][xx] = c

    def glyph(self, ch):
        if "a" <= ch <= "z":
            ch = ch.upper()
        i = _FONT_CHARS.find(ch)
        return FONT[i] if i >= 0 else [0] * 7

    def text(self, x, y, s, maxlen, scale, fg, bg):
        fg24, bg24 = rgb24(COLORS[fg]), rgb24(COLORS[bg])
        for i in range(maxlen):
            if i >= len(s) or s[i] == "\0":
                break
            rows = self.glyph(s[i])
            for py in range(8 * scale):
                row = py // scale
                bits = rows[row] if row < 7 else 0
                for px in range(6 * scale):
                    col = px // scale
                    on = col < 5 and (bits >> (4 - col)) & 1
                    xx, yy = x + px, y + py
                    if 0 <= xx < W and 0 <= yy < H:
                        self.px[yy][xx] = fg24 if on else bg24
            x += 6 * scale


def save(path, screen):
    raw = bytearray()
    for yy in range(H):
        raw.append(0)
        for xx in range(W):
            raw += bytes(screen.px[yy][xx])

    def chunk(tag, data):
        c = tag + data
        return (struct.pack(">I", len(data)) + c
                + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)
    print("wrote", path)


# --- screen builders follow the app's drawing order ---------------------
def screen_unlock():
    """passlock.c: title_line / okb / enter_digits("PASSWORD:", 56, mask=1)."""
    s = Screen()
    #s.fill(0, 0, W, 22, "blue")
    #s.text(18, 3, "RADIO LOCK", 12, 2, "white", "blue")
    s.fill(0, 22, W, H - 22, "black")
    s.text(4, 26, "RADIO IS LOCK", 14, 2, "red", "black")
    s.text(4, 118, "ENTER PASSWORD TO UNLOCK", 30, 1, "grey", "black")
    s.fill(0, 56, W, 16, "black")
    s.text(50, 56, "PASSWORD:", 20, 1, "yellow", "black")
    s.fill(4, 67, W - 8, 22, "black")
    s.text(40, 68, "******", 8, 2, "white", "black")
    return s


def _lock_menu(selected):
    """passlock.c: menu_loop with STATUS: LOCKED, selected highlighted."""
    s = Screen()
    s.fill(0, 0, W, 22, "blue")
    s.text(18, 3, "RADIO LOCK", 12, 2, "white", "blue")
    s.fill(0, 22, W, H - 22, "black")
    s.text(34, 28, "STATUS: LOCKED", 16, 1, "red", "black")
    rows = ["[LOCK]      Set pass", " UNLOCK     Clear pass",
            " LOCK NOW   Lock radio", " EXIT       Menu"]
    for i, y in zip(range(4), (44, 60, 76, 92)):
        s.text(4, y, rows[i], 24, 1, "yellow" if i == selected else "white", "black")
    s.text(2, 118, "UP/DN: MOVE      BACK: OK", 30, 1, "grey", "black")
    return s


def screen_new_passw():
    """passlock.c: LOCK selected -> clear_body + enter_digits("NEW PASSW:",56,0)."""
    s = Screen()
    s.fill(0, 0, W, 22, "blue")
    s.text(18, 3, "RADIO LOCK", 12, 2, "white", "blue")
    s.fill(0, 22, W, H - 22, "black")
    s.fill(0, 56, W, 16, "black")
    s.text(50, 56, "NEW PASSW:", 20, 1, "yellow", "black")
    s.fill(4, 67, W - 8, 22, "black")
    s.text(40, 68, "123", 8, 2, "white", "black")
    return s


def _radio_widget(modules, selected):
    """menu.c: title bar + module list rows + footer."""
    ROW_Y, ROW_H, ROWS, FOOT_Y = 26, 18, 5, 117
    s = Screen()
    s.fill(0, 0, W, 22, "blue")
    s.text(8, 3, "RADIO WIDGET", 12, 2, "white", "blue")
    s.fill(0, 22, W, 4, "black")
    s.text(4, FOOT_Y, "MENU: RUN   BACK: RADIO", 26, 1, "grey", "black")
    s.fill(0, FOOT_Y + 8, W, H - FOOT_Y - 8, "black")
    for row in range(ROWS):
        y = ROW_Y + row * ROW_H
        s.fill(0, y, W, 1, "black")
        s.fill(0, y + 17, W, 1, "black")
        s.fill(0, y + 1, 4, 16, "black")
        if row < len(modules):
            sel = selected == row
            fg, bg = ("black", "yellow") if sel else ("white", "black")
            s.text(4, y + 1, modules[row], 12, 2, fg, bg)
            s.fill(0, y, 1, 16, bg)
            s.fill(0, y, 1, 1, bg)
        else:
            s.fill(0, y + 1, W, 16, "black")
    return s


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    save(os.path.join(out, "screen_radio_lock_locked.png"), screen_unlock())
    save(os.path.join(out, "screen_radio_lock_menu.png"), _lock_menu(0))
    save(os.path.join(out, "screen_radio_lock_menu_selected.png"), _lock_menu(2))
    save(os.path.join(out, "screen_new_passw.png"), screen_new_passw())
    mods = ["PASSLOCK", "GPS", "APRS MONITOR", "USB UPDATE", "DKXCEPIC"]
    save(os.path.join(out, "screen_radio_widget.png"), _radio_widget(mods, 0))
    save(os.path.join(out, "screen_radio_widget_selected.png"), _radio_widget(mods, 1))


if __name__ == "__main__":
    main()