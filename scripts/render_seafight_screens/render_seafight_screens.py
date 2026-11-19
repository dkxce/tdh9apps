#!/usr/bin/env python3
"""Render TD-H9 seafight-app screens as PNGs, pixel-identical to seafight.c.

Reproduces gfx_text / gfx_fill exactly (5x7 font parsed from font.h) and the
seafight drawing helpers from sf_core.c / sf_aprs.c: sf_cell, sf_board (two
10x10 grids + coords + ROOM header + footer), sf_status (bottom status line),
sf_select_room (room entry) and the win/lose screens.

Outputs <scripts>/render_seafight_screens/*.png:
  screen_menu.png      APRS BATTLESHIP menu (1 PLAY VS CPU / 2 APRS RADIO LINK)
  screen_room.png      ROOM (0-99) entry screen
  screen_ready.png     APRS board, header ROOM 01, "YOUR TURN! FIRE"
  screen_sent.png      After firing, "SENT! WAITING RESPONSE..."
  screen_hit.png       Our shot hit enemy, "DIRECT HIT! AGAIN"
  screen_sunk.png      We sank an enemy ship, "ENEMY SHIP SUNK! YOUR TURN"
  screen_enemy_hit.png Enemy hit our ship, "HIT! ENEMY SHOTS"
  screen_win.png       Final "YOU WIN"
  screen_lose.png      Final "YOU LOOSE"
"""
import os
import re
import random
import struct
import zlib

W, H = 160, 128
N, CELL = 10, 7
MY_X, EN_X, GRID_Y = 6, 84, 24

# RGB565
BLACK = 0x0000
COL_SEA = 0x0015
COL_SHIP = 0x07E0
COL_MISS = 0xC618
COL_HIT = 0xF800
COL_CUR = 0xFFE0
C_CYAN = 0x07FF
C_FUCHSIA = 0xF81F
C_GREY = 0x8410
C_RED = 0xF800
C_GREEN = 0x07E0
C_WHITE = 0xffff
C_YELLOW = 0xFFE0

COLORS = {"black": 0x0000, "white": 0xffff, "yellow": 0xffe0, "blue": 0x001f,
          "grey": 0x8410, "green": 0x07e0, "red": 0xf800}


def rgb24(c565):
    r, g, b = (c565 >> 11) & 0x1f, (c565 >> 5) & 0x3f, c565 & 0x1f
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


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
        c = rgb24(color) if isinstance(color, int) else color
        for yy in range(y, min(y + h, H)):
            for xx in range(x, min(x + w, W)):
                self.px[yy][xx] = c

    def glyph(self, ch):
        if "a" <= ch <= "z":
            ch = ch.upper()
        i = _FONT_CHARS.find(ch)
        return FONT[i] if i >= 0 else [0] * 7

    def text(self, x, y, s, maxlen, scale, fg24, bg24):
        x = int(x)
        for i in range(maxlen):
            if i >= len(s):
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
        return x


# --- seafight drawing helpers (sf_core.c) -------------------------------
def sf_cell(s, bx, x, y, st, cur):
    sx = bx + x * CELL
    sy = GRID_Y + y * CELL
    s.fill(sx, sy, CELL - 1, CELL - 1, COL_SHIP if st == 1 else COL_SEA)
    if st == 2:                       # C_MISS
        s.fill(sx + 2, sy + 2, 2, 2, COL_MISS)
    elif st == 3:                     # C_HIT
        s.fill(sx + 1, sy + 1, CELL - 3, CELL - 3, COL_HIT)
        s.fill(sx + 2, sy + 2, 2, 2, C_YELLOW)
    if cur:
        oc = COL_HIT if cur == 2 else COL_CUR
        s.fill(sx, sy, CELL - 1, 1, oc)
        s.fill(sx, sy + CELL - 2, CELL - 1, 1, oc)
        s.fill(sx, sy, 1, CELL - 1, oc)
        s.fill(sx + CELL - 2, sy, 1, CELL - 1, oc)


def sf_status(s, msg, c):
    s.fill(0, 100, W, 13, BLACK)
    s.text(2, 102, msg, 26, 1, rgb24(c), rgb24(BLACK))


def sf_board(s, room=None, lx=-1, ly=-1, cx=4, cy=4, ox=-1, oy=-1):
    """Reproduce sf_board() with fixed or generated grids + cursor/coords."""
    s.fill(0, 0, W, H, BLACK)
    x = s.text(7, 12, "MY", 2, 1, rgb24(COL_SHIP), rgb24(BLACK))
    # " x:y" coordinate after MY (lx,ly)
    if lx >= 0:
        x = s.text(x, 12, " %d:%d" % (lx, ly), 5, 1, rgb24(COL_HIT), rgb24(BLACK))
    if room:
        x = s.text(EN_X + 3, 12, "ROOM " + room, 7, 1, rgb24(C_FUCHSIA), rgb24(BLACK))
    else:
        x = s.text(EN_X + 3, 12, "EN", 2, 1, rgb24(C_CYAN), rgb24(BLACK))
    if ox >= 0:
        x = s.text(x, 12, " %d:%d" % (ox, oy), 5, 1, rgb24(COL_CUR), rgb24(BLACK))
    s.text(4, 117, "24568:AIM 0:FIRE #:NEW", 26, 1, rgb24(C_GREY), rgb24(BLACK))
    return x


# --- game-state grids ---------------------------------------------------
def my_fleet():
    """A plausible player fleet on the left grid (C_SHIP=1)."""
    m = [[0] * N for _ in range(N)]
    ships = [(0, 0, 4, 1), (0, 5, 0, 2), (3, 0, 3, 1), (5, 2, 0, 2),
             (2, 7, 2, 1), (6, 0, 0, 2), (8, 8, 0, 1), (9, 2, 1, 1),
             (7, 5, 0, 1), (4, 9, 0, 1)]
    for (x, y, ln, h) in ships:
        for i in range(ln):
            m[y + (0 if h else i)][x + (i if h else 0)] = 1
    return m


def enemy_grid():
    """Enemy right grid: all C_WATER (never shows ships)."""
    return [[0] * N for _ in range(N)]


def draw_grids(s, my, en, mycur=None, encur=None):
    for y in range(N):
        for x in range(N):
            sf_cell(s, MY_X, x, y, my[y][x], 2 if mycur == (x, y) else 0)
            sf_cell(s, EN_X, x, y, en[y][x], 1 if encur == (x, y) else 0)


# --- screens ------------------------------------------------------------
def screen_menu():
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    s.text(18, 8, "-- APRS BATTLESHIP -- ", 22, 1, rgb24(0x07FF), rgb24(BLACK))
    s.text(14, 28, "1: PLAY VS CPU", 18, 1, rgb24(0x07E0), rgb24(BLACK))
    s.text(14, 42, "2: APRS RADIO LINK", 18, 1, rgb24(0xF81F), rgb24(BLACK))
    s.text(8, 62, "(c) dkxce 2026", 18, 1, rgb24(0x8410), rgb24(BLACK))
    s.text(8, 74, "https://github.com/dkxce", 25, 1, rgb24(0x8410), rgb24(BLACK))
    s.text(8, 94, "IF APRS FAILS RE-POWER", 25, 1, rgb24(0x8410), rgb24(BLACK))
    s.text(90, 112, "BACK: EXIT", 12, 1, rgb24(C_RED), rgb24(BLACK))
    return s


def screen_room():
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    s.text(30, 30, "ROOM (0-99):", 12, 1, rgb24(C_WHITE), rgb24(BLACK))
    s.text(4, 102, "0-9 DIGIT *:DEL MENU OK", 24, 1, rgb24(C_GREY), rgb24(BLACK))
    s.text(64, 62, "01", 4, 2, rgb24(COL_CUR), rgb24(BLACK))
    s.fill(64, 78, 10, 2, C_RED)                 # cursor under '0' (pos 0)
    return s


def screen_ready():
    """APRS board, room 01, player's turn."""
    s = Screen()
    my = my_fleet()
    en = enemy_grid()
    sf_board(s, room="01", cx=4, cy=4)
    draw_grids(s, my, en, encur=(4, 4))
    sf_status(s, "YOUR TURN! FIRE", COL_CUR)
    return s


def screen_sent():
    """Player fired at (4,4); waiting for the reply."""
    s = Screen()
    my = my_fleet()
    en = enemy_grid()
    sf_board(s, room="01", cx=4, cy=4, ox=4, oy=4)
    draw_grids(s, my, en, encur=(4, 4))
    sf_status(s, "SENT! WAITING RESPONSE...", COL_CUR)
    return s


def screen_hit():
    """Our shot hit: enemy (4,4) red; player may shoot again."""
    s = Screen()
    my = my_fleet()
    en = enemy_grid()
    en[4][4] = 3                                     # C_HIT
    sf_board(s, room="01", cx=4, cy=4, ox=4, oy=4)
    draw_grids(s, my, en, encur=(4, 4))
    sf_status(s, "DIRECT HIT! AGAIN", COL_CUR)
    return s


def screen_sunk():
    """We sank an enemy ship (a vertical 3-ship at x=2,y=3..5)."""
    s = Screen()
    my = my_fleet()
    en = enemy_grid()
    en[3][2] = en[4][2] = en[5][2] = 3               # C_HIT on the whole ship
    for (gx, gy) in [(1, 2), (1, 3), (1, 4), (1, 5), (1, 6), (2, 2), (2, 6),
                     (3, 2), (3, 3), (3, 4), (3, 5), (3, 6)]:
        if 0 <= gx < N and 0 <= gy < N and en[gy][gx] == 0:
            en[gy][gx] = 2                            # C_MISS perimeter
    sf_board(s, room="01", cx=2, cy=4, ox=2, oy=4)
    draw_grids(s, my, en, encur=(2, 4))
    sf_status(s, "ENEMY SHIP SUNK! YOUR TURN", COL_SHIP)
    return s


def screen_enemy_hit():
    """Enemy hit OUR ship (5,2 on left grid is red), enemy's turn."""
    s = Screen()
    my = my_fleet()
    my[2][5] = 3                                      # C_HIT on our ship
    en = enemy_grid()
    sf_board(s, room="01", lx=5, ly=2, cx=4, cy=4)
    draw_grids(s, my, en, encur=(4, 4))
    sf_status(s, "HIT! ENEMY SHOTS", C_RED)
    return s


def screen_win():
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    s.text(42, 52, "YOU WIN", 8, 2, rgb24(COL_SHIP), rgb24(BLACK))
    s.text(48, 92, "BACK: EXIT", 10, 1, rgb24(C_GREY), rgb24(BLACK))
    return s


def screen_lose():
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    s.text(28, 52, "YOU LOOSE", 10, 2, rgb24(C_RED), rgb24(BLACK))
    s.text(48, 92, "BACK: EXIT", 10, 1, rgb24(C_GREY), rgb24(BLACK))
    return s


# --- PNG writer (same as tracker/lock scripts) --------------------------
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
    for name, fn in [("screen_menu", screen_menu), ("screen_room", screen_room),
                     ("screen_ready", screen_ready), ("screen_sent", screen_sent),
                     ("screen_hit", screen_hit), ("screen_sunk", screen_sunk),
                     ("screen_enemy_hit", screen_enemy_hit),
                     ("screen_win", screen_win), ("screen_lose", screen_lose)]:
        save(os.path.join(out, name + ".png"), fn())


if __name__ == "__main__":
    main()