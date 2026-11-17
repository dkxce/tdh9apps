#!/usr/bin/env python3
"""Render TD-H9 tracksvr screens as PNGs, pixel-identical to tracksvr.c.

Reproduces gfx_text / gfx_fill exactly: the 5x7 font is parsed from
myapps/shared/common/font.h, a glyph is a 6x8 cell multiplied by `scale`, and
colors are the GFX_* RGB565 values from gfx.h converted to 24-bit. Coordinates
and strings match the app (main_menu, saved_points, point_view, name_edit).

Outputs <scripts>/render_tracksvr_screens/*.png:
  screen_menu.png           TRACKER SAVER main menu (Save Start selected)
  screen_points.png         SAVED POINTS list (HOME selected)
  screen_point.png          POINT view (NUM/NAME/LAT/LON + NAVTO / delete)
  screen_name.png           NAME T9 editor (cursor, "HOME-ish")
  screen_save_choice.png    save sub-menu (EDIT LAT / EDIT LON / SAVE)
  screen_clear.png          CLEAR ALL confirm (MENU=YES BACK=NO)
"""
import os
import re
import struct
import zlib

W, H = 160, 128

# GFX colors (RGB565) -> names
COLORS = {"black": 0x0000, "white": 0xffff, "yellow": 0xffe0, "blue": 0x001f,
          "grey": 0x8410, "green": 0x07e0, "red": 0xf800}
C_DGREY = 0x4208


def rgb24(c565):
    """Expand a 565 to (r, g, b), reproducing gfx.c's 8-bit conversion."""
    r, g, b = (c565 >> 11) & 0x1f, (c565 >> 5) & 0x3f, c565 & 0x1f
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


# --- parse the real 5x7 font from font.h --------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))
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
        if isinstance(color, str):
            c = rgb24(COLORS[color])
        else:
            c = rgb24(color) if isinstance(color, int) else color
        for yy in range(y, min(y + h, H)):
            for xx in range(x, min(x + w, W)):
                self.px[yy][xx] = c

    def glyph(self, ch):
        if "a" <= ch <= "z":
            ch = ch.upper()
        i = _FONT_CHARS.find(ch)
        return FONT[i] if i >= 0 else [0] * 7

    def text(self, x, y, s, maxlen, scale, fg24, bg24="black"):
        bg = rgb24(COLORS[bg24]) if isinstance(bg24, str) else bg24
        x = int(x)
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
                        self.px[yy][xx] = fg24 if on else bg
            x += 6 * scale


def center(s, y, text, scale):
    w = len(text) * 6 * scale
    x = (W - w) // 2 if w < W else 0
    s.text(x, y, text, 26, scale, rgb24(COLORS["yellow"]), "black")


def msg(s, text):
    """msg(): black fill, scale-2 text at y=50. """
    s.fill(0, 0, W, H, "black")
    s.text(4, 50, text, 26, 2, rgb24(COLORS["white"]), "black")


# --- main menu (main_menu, tracksvr.c:385) -------------------------------
ROW_Y, ROW_H = 16, 14
MAIN = ["Save Start Point", "Save Current Point", "Save NavTo Point",
        "Save GPS Point", "Save My Own", "Saved Points", "Clear All"]


def menu_row(s, i, sel):
    y = ROW_Y + i * ROW_H
    bg = rgb24(C_DGREY) if i == sel else rgb24(COLORS["black"])
    label = MAIN[i][:17]
    s.fill(0, y, W, ROW_H, bg)
    s.text(4, y + 4, "[%d] %s" % (i + 1, label), 26, 1, rgb24(COLORS["white"]), bg)


def screen_menu():
    s = Screen()
    s.fill(0, 0, W, H, "black")
    center(s, 2, "-- TRACKER SAVER --", 1)
    for i in range(7):
        menu_row(s, i, i == 0)
    s.text(4, 116, "UP/DN MENU BACK", 16, 1, rgb24(COLORS["grey"]), "black")
    return s


# --- saved points list (saved_points, tracksvr.c:410) --------------------
# Row drawn at 17 + r*12; header at y=4. Label: "[n] name".
def saved_row(s, top_r, r, sel):
    y = 17 + r * 12
    bg = rgb24(C_DGREY) if top_r == sel else rgb24(COLORS["black"])
    name = ["HOME", "WORK", "BASE", "FIELD"][top_r] if top_r < 4 else ""
    s.fill(0, y, W, 12, bg)
    s.text(4, y + 1, "[%d] %s" % (top_r + 1, name), 24, 1, rgb24(COLORS["white"]), bg)


def screen_points():
    s = Screen()
    s.fill(0, 0, W, H, "black")
    center(s, 4, "-- SAVED POINTS --", 1)
    for r in range(4):
        saved_row(s, r, r, 0)
    return s


# --- point view (point_view, tracksvr.c:447) -----------------------------
def coord_str(micro):
    neg = micro < 0
    a = -micro if neg else micro
    t = "%06d" % a
    out = ("-" if neg else "") + t[:-5] + "." + t[-5:]
    return out


def screen_point():
    s = Screen()
    s.fill(0, 0, W, H, "black")
    s.text(4, 8, "NUM 1", 24, 1, rgb24(COLORS["yellow"]), "black")
    s.text(4, 22, "NAME HOME", 24, 1, rgb24(COLORS["white"]), "black")
    s.text(4, 36, "LAT " + coord_str(5051230), 24, 1, rgb24(COLORS["white"]), "black")
    s.text(4, 50, "LON " + coord_str(4018420), 24, 1, rgb24(COLORS["white"]), "black")
    s.text(4, 84, "5 NAVTO", 12, 1, rgb24(COLORS["yellow"]), "black")
    s.text(0, 96, "MENU = EDIT", 14, 1, rgb24(COLORS["grey"]), "black")
    s.text(0, 108, "*/0/# = DELETE  BACK = LIST", 28, 1, rgb24(COLORS["grey"]), "black")
    return s


# --- name T9 editor (name_edit, tracksvr.c:221) --------------------------
def edit_line(buf, pos, out):
    """buf up to pos, then '_' cursor, padded to 9, trailing spaces."""
    line = (buf[:pos] + "_" + " " * (8 - pos))[:9]
    return line


def screen_name():
    s = Screen()
    s.fill(0, 0, W, H, "black")
    center(s, 4, "-- NAME --", 1)
    s.text(4, 100, "0-9 TAP  * / # CURSOR", 21, 1, rgb24(COLORS["grey"]), "black")
    s.text(4, 112, "MENU SAVE  BACK CANCEL", 21, 1, rgb24(COLORS["grey"]), "black")
    # editor field: black 140x24 at (6,44), green scale-2 text at (6,46)
    s.fill(6, 44, 140, 24, "black")
    s.text(6, 46, edit_line("HOME", 4, None), 10, 2, rgb24(COLORS["green"]), "black")
    return s


# --- save sub-menu (do_save, tracksvr.c:322): EDIT LAT / EDIT LON / SAVE
SAVE_M = ["EDIT LAT", "EDIT LON", "SAVE"]


def screen_save_choice():
    s = Screen()
    s.fill(0, 0, W, H, "black")
    s.text(4, 20, "LAT " + coord_str(5051230), 24, 1, rgb24(COLORS["white"]), "black")
    s.text(4, 34, "LON " + coord_str(4018420), 24, 1, rgb24(COLORS["white"]), "black")
    center(s, 2, "SAVE CURRENT", 1)
    s.fill(0, 60, W, 48, "black")
    for i in range(3):
        y = 60 + i * 16
        bg = rgb24(C_DGREY) if i == 1 else rgb24(COLORS["black"])
        s.fill(0, y, W, 16, bg)
        s.text(4, y + 2, SAVE_M[i], 12, 1, rgb24(COLORS["white"]), bg)
    return s


# --- CLEAR ALL confirm (tracksvr.c:519) ---------------------------------
def screen_clear():
    s = Screen()
    s.fill(0, 0, W, H, "black")
    center(s, 50, "<CLEAR ALL>", 2)
    s.text(0, 112, "MENU=YES BACK=NO", 16, 1, rgb24(COLORS["grey"]), "black")
    return s


# --- PNG writer (same as render_tracker_screens.py) ----------------------
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


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(out, exist_ok=True)
    save(os.path.join(out, "screen_menu.png"), screen_menu())
    save(os.path.join(out, "screen_points.png"), screen_points())
    save(os.path.join(out, "screen_point.png"), screen_point())
    save(os.path.join(out, "screen_name.png"), screen_name())
    save(os.path.join(out, "screen_save_choice.png"), screen_save_choice())
    save(os.path.join(out, "screen_clear.png"), screen_clear())


if __name__ == "__main__":
    main()