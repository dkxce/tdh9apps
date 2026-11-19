#!/usr/bin/env python3
"""Render TD-H9 game screens as PNGs, pixel-identical to their .c sources.

Reproduces gfx_text / gfx_fill exactly (5x7 font parsed from font.h) plus each
game's own drawing helpers. Outputs <scripts>/render_games/*.png.

For each of the 8 games (8ballpool, battlecity, bomberman, bomberman2,
loadrunner, mineswiper, snake, tetris) 3-4 screens are produced: gameplay,
pause, stage-clear/win and game-over.
"""
import os
import re
import struct
import zlib

W, H = 160, 128

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


def box_msg(s, x, y, txt, col=0x07E0, sc=1):
    """Draw a centered overlay message on a small black rectangle."""
    s.fill(x - 4, y - 4, len(txt) * 6 * sc + 8, 8 * sc + 8, 0x0000)
    s.text(x, y, txt, len(txt), sc, rgb24(col), rgb24(0x0000))


def _border(s, x, y, w2, h2):
    s.fill(x - 2, y - 2, w2 + 4, 2, 0xFFFF)
    s.fill(x - 2, y + h2, w2 + 4, 2, 0xFFFF)
    s.fill(x - 2, y, 2, h2, 0xFFFF)
    s.fill(x + w2, y, 2, h2, 0xFFFF)


# =====================================================================
# 8ballpool
# =====================================================================
TBL_X, TBL_Y, TBL_W, TBL_H = 8, 18, 144, 104
BALL_COLORS = [0xFFFF, 0xFFE0, 0x001F, 0xF800, 0xF81F, 0xFD20,
               0x07E0, 0x07FF, 0xC618, 0x2104]


def pb_ball(s, x, y, idx):
    b = BALL_COLORS[idx]
    s.fill(x - 2, y - 3, 5, 7, b)
    s.fill(x - 3, y - 2, 7, 5, b)
    s.fill(x - 1, y - 1, 2, 2, 0xFFFF)


def pb_table(s):
    s.fill(0, 0, W, H, 0x0000)
    s.text(2, 3, "SHT:", 4, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(28, 3, "0", 4, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(58, 3, "PWR:", 4, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.fill(84, 4, 34, 6, 0x4208)
    s.fill(84, 4, 21, 6, 0x07E0)
    s.text(120, 3, "*:PAU", 5, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(140, 3, "B:X", 3, 1, rgb24(0xF800), rgb24(0x0000))
    s.fill(TBL_X - 4, TBL_Y - 4, TBL_W + 8, TBL_H + 8, 0x8200)
    s.fill(TBL_X, TBL_Y, TBL_W, TBL_H, 0x03E0)
    for px6 in (TBL_X, TBL_X + TBL_W // 2, TBL_X + TBL_W):
        for py6 in (TBL_Y, TBL_Y + TBL_H):
            s.fill(px6 - 4, py6 - 4, 9, 9, 0x0000)


def pb_rack(s):
    start_x, mid_y = TBL_X + 96, TBL_Y + TBL_H // 2
    order = [1, 2, 3, 4, 9, 5, 6, 7, 8]
    k = 0
    for col in range(4):
        count = (col + 1) if col < 3 else 3
        cx = start_x + int(col * 6.40)
        sy = mid_y - int(((count - 1) * 7.22) / 2)
        for row in range(count):
            if k >= 9:
                break
            pb_ball(s, cx, int(sy + row * 7.22), order[k])
            k += 1
    pb_ball(s, TBL_X + 36, mid_y, 0)


def pb_aim(s, angle_slope):
    for d in range(8, 45, 6):
        ax = TBL_X + 36 + d
        ay = TBL_Y + TBL_H // 2 + d // angle_slope
        if TBL_X + 2 < ax < TBL_X + TBL_W - 2 and TBL_Y + 2 < ay < TBL_Y + TBL_H - 2:
            s.fill(ax, ay, 2, 2, 0xFFE0)


def screen_8ballpool():
    s = Screen()
    pb_table(s)
    pb_rack(s)
    pb_aim(s, 6)
    return s


def screen_8ballpool_pause():
    s = Screen()
    pb_table(s)
    pb_rack(s)
    box_msg(s, 58, 60, "PAUSED", 0xFFE0)
    return s


def screen_8ballpool_win():
    s = Screen()
    pb_table(s)
    # only cue ball left on the table (all color balls + 8 potted)
    pb_ball(s, TBL_X + 36, TBL_Y + TBL_H // 2, 0)
    box_msg(s, 44, 62, "YOU WIN!", 0x07E0)
    return s


def screen_8ballpool_foul():
    s = Screen()
    pb_table(s)
    pb_rack(s)
    box_msg(s, 38, 62, "8-BALL FOUL", 0xF800)
    return s


# =====================================================================
# battlecity
# =====================================================================
MAP_W, MAP_H, TILE = 13, 13, 9
OFF_X, OFF_Y = 4, 5
COL_BRICK, COL_STEEL, COL_BASE = 0xC200, 0xC618, 0xF81F
COL_TANK, COL_ENEMY, COL_WATER, COL_FOREST, COL_LEAF = 0xFFE0, 0x07FF, 0x001F, 0x05E0, 0x07E0

LV1 = [
    [0,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,1,0,1,0,1,0,1,0,1,0,1,0],
    [0,1,0,1,0,1,2,1,0,1,0,1,0],
    [0,1,0,1,5,5,0,5,5,1,0,1,0],
    [0,0,0,0,5,1,0,1,5,0,0,0,0],
    [1,1,0,2,0,1,0,1,0,2,0,1,1],
    [5,4,4,0,0,0,0,0,0,0,4,4,5],
    [0,1,0,1,0,1,1,1,0,1,0,1,0],
    [0,1,0,1,0,0,0,0,0,1,0,1,0],
    [0,1,0,1,0,1,0,1,0,1,0,1,0],
    [0,0,0,0,0,1,0,1,0,0,0,0,0],
    [0,0,0,0,0,1,1,1,0,0,0,0,0],
    [0,0,0,0,0,1,3,1,0,0,0,0,0],
]


def bc_tile(s, tx, ty, t):
    px, py = OFF_X + tx * TILE, OFF_Y + ty * TILE
    if t == 0:
        s.fill(px, py, TILE, TILE, 0x0000)
    elif t == 1:
        s.fill(px, py, TILE, TILE, COL_BRICK)
        s.fill(px, py + 4, TILE, 1, 0x0000)
        s.fill(px + 4, py, 1, TILE, 0x0000)
    elif t == 2:
        s.fill(px, py, TILE, TILE, COL_STEEL)
        s.fill(px + 2, py + 2, TILE - 4, TILE - 4, 0xFFFF)
    elif t == 3:
        s.fill(px + 1, py + 1, TILE - 2, TILE - 2, COL_BASE)
    elif t == 4:
        s.fill(px, py, TILE, TILE, COL_WATER)
        s.fill(px + 1, py + 2, 3, 1, COL_ENEMY)
        s.fill(px + 5, py + 6, 3, 1, COL_ENEMY)
    elif t == 5:
        s.fill(px, py, TILE, TILE, 0x0000)
        s.fill(px + 1, py + 1, 3, 3, COL_FOREST)
        s.fill(px + 5, py + 1, 3, 3, COL_LEAF)
        s.fill(px + 1, py + 5, 3, 3, COL_LEAF)
        s.fill(px + 5, py + 5, 3, 3, COL_FOREST)


def bc_tank(s, x, y, color, d):
    px, py = OFF_X + x * TILE, OFF_Y + y * TILE
    s.fill(px + 1, py + 1, TILE - 2, TILE - 2, color)
    if d == 0:
        s.fill(px + 4, py, 1, 4, 0xFFFF)
    elif d == 2:
        s.fill(px + 4, py + 5, 1, 4, 0xFFFF)
    elif d == 3:
        s.fill(px, py + 4, 4, 1, 0xFFFF)
    else:
        s.fill(px + 5, py + 4, 4, 1, 0xFFFF)


def bc_hud(s, score, lives, enemies):
    s.fill(126, 6, 40, 114, 0x0000)
    s.text(126, 6, "PTS", 5, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(126, 28, "HP", 5, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(126, 50, "ENM", 5, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(126, 70, "2/4/6", 5, 1, rgb24(0x07FF), rgb24(0x0000))
    s.text(126, 78, "5/8:D", 5, 1, rgb24(0x07FF), rgb24(0x0000))
    s.text(126, 90, "0:FR", 5, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(126, 102, "*:PAUS", 5, 1, rgb24(0x07E0), rgb24(0x0000))
    s.text(126, 114, "Back:EX", 4, 1, rgb24(0xF800), rgb24(0x0000))
    s.text(126, 16, str(score), 6, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(126, 38, str(lives), 4, 1, rgb24(0x07E0), rgb24(0x0000))
    s.text(126, 60, str(enemies), 4, 1, rgb24(0x07FF), rgb24(0x0000))


def bc_scene(s, score, lives, enemies, tank_pos=(4, 12, 0),
             en1=(0, 0, 2), en2=(12, 0, 2)):
    s.fill(0, 0, W, H, 0x0000)
    _border(s, OFF_X, OFF_Y, MAP_W * TILE, MAP_H * TILE)
    for y in range(MAP_H):
        for x in range(MAP_W):
            bc_tile(s, x, y, LV1[y][x])
    bc_tank(s, en1[0], en1[1], COL_ENEMY, en1[2])
    bc_tank(s, en2[0], en2[1], COL_ENEMY, en2[2])
    bc_tank(s, tank_pos[0], tank_pos[1], COL_TANK, tank_pos[2])
    bc_hud(s, score, lives, enemies)


def screen_battlecity():
    s = Screen()
    bc_scene(s, 300, 3, 8, tank_pos=(4, 12, 0), en1=(0, 0, 2), en2=(12, 0, 2))
    return s


def screen_battlecity_pause():
    s = Screen()
    bc_scene(s, 500, 3, 7)
    box_msg(s, 32, 56, "PAUSED", 0x07E0)
    return s


def screen_battlecity_clear():
    s = Screen()
    bc_scene(s, 900, 3, 0)
    box_msg(s, 30, 56, "STAGE CLEAR", 0x07E0)
    return s


def screen_battlecity_over():
    s = Screen()
    bc_scene(s, 900, 0, 5)
    bc_tank(s, 4, 12, COL_TANK, 0)  # player dead (crushed)
    s.fill(OFF_X + 4 * TILE, OFF_Y + 12 * TILE, TILE, TILE, 0xF800)
    box_msg(s, 30, 56, "GAME OVER", 0xF800)
    return s


# =====================================================================
# bomberman / bomberman2 (shared board helpers)
# =====================================================================
BM_MW, BM_MH = 13, 11
BM2_WALL = [0x8410, 0x03E0, 0x041F, 0x8000]
BM2_BRICK = [0xC200, 0x8200, 0x5DFF, 0xFD20]


class RNG:
    def __init__(self, seed):
        self.s = seed

    def __call__(self):
        self.s ^= (self.s << 13) & 0xFFFFFFFF
        self.s ^= (self.s >> 17)
        self.s ^= (self.s << 5) & 0xFFFFFFFF
        self.s &= 0xFFFFFFFF
        return self.s


def bm_field(rng, pct):
    m = [[0] * BM_MW for _ in range(BM_MH)]
    for y in range(BM_MH):
        for x in range(BM_MW):
            if x == 0 or y == 0 or x == BM_MW - 1 or y == BM_MH - 1 or (x % 2 == 0 and y % 2 == 0):
                m[y][x] = 1
            elif x <= 2 and y <= 2:
                m[y][x] = 0
            elif rng() % 100 < pct:
                m[y][x] = 2
            else:
                m[y][x] = 0
    return m


def bm_tile(s, x, y, t, wall=0x8410, brick=0xC200):
    sx, sy = 2 + x * 10, 14 + y * 10
    s.fill(sx, sy, 10, 10, 0x0000)
    if t == 1:
        s.fill(sx, sy, 10, 10, wall)
        s.fill(sx + 1, sy + 1, 8, 8, 0xC618)
    elif t == 2:
        s.fill(sx, sy, 10, 10, brick)
        s.fill(sx, sy + 4, 10, 1, 0x0000)
        s.fill(sx + 5, sy, 1, 4, 0x0000)
        s.fill(sx + 2, sy + 5, 1, 5, 0x0000)
    elif t == 3:
        s.fill(sx + 1, sy + 1, 8, 8, 0x07E0)
        s.fill(sx + 3, sy + 3, 4, 4, 0x0000)
    elif t == 4:
        s.fill(sx + 2, sy + 2, 6, 6, 0xF800)
        s.fill(sx + 4, sy + 3, 2, 4, 0xFFE0)


def bm_player(s, px, py, suit=0x001F):
    sx, sy = 2 + px * 10, 14 + py * 10
    s.fill(sx + 2, sy + 1, 6, 4, 0xFFFF)
    s.fill(sx + 3, sy + 2, 4, 2, 0xFFE0)
    s.fill(sx + 2, sy + 5, 6, 4, suit)


def bm_enemy(s, x, y, col):
    sx, sy = 2 + x * 10, 14 + y * 10
    s.fill(sx + 1, sy + 2, 8, 6, col)
    s.fill(sx + 2, sy + 1, 6, 8, col)
    s.fill(sx + 3, sy + 3, 1, 2, 0xFFFF)
    s.fill(sx + 6, sy + 3, 1, 2, 0xFFFF)


def bm_bomb(s, x, y, col=0x07FF):
    sx, sy = 2 + x * 10, 14 + y * 10
    s.fill(sx + 2, sy + 2, 6, 6, col)
    s.fill(sx + 4, sy + 1, 2, 2, 0xFFE0)


def bm_fire(s, x, y):
    sx, sy = 2 + x * 10, 14 + y * 10
    s.fill(sx + 1, sy + 1, 8, 8, 0xFD20)


def bm_banner(s, p_text, st_text):
    s.fill(0, 0, W, H, 0x0000)
    s.text(2, 2, "PTS:", 5, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(38, 2, p_text, 5, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(88, 2, "ST:", 4, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(112, 2, st_text, 3, 1, rgb24(0x07E0), rgb24(0x0000))
    s.text(128, 2, "B:EX", 5, 1, rgb24(0xF800), rgb24(0x0000))


def bm_side_help(s, lines):
    y = 20
    for ln, col in lines:
        s.text(136, y, ln, 6, 1, rgb24(col), rgb24(0x0000))
        y += 10


def bm_v1(m, door=None, bonus=None):
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    s.text(2, 2, "PTS:", 5, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(88, 2, "ST:", 4, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(112, 2, "1", 3, 1, rgb24(0x07E0), rgb24(0x0000))
    s.text(128, 2, "B:EX", 5, 1, rgb24(0xF800), rgb24(0x0000))
    for (ln, col) in [("2/4/6", 0x07FF), ("5/8:D", 0x07FF), ("0:BMB", 0xFFE0), ("*:PAU", 0x07E0)]:
        pass
    bm_side_help(s, [("2/4/6", 0x07FF), ("5/8:D", 0x07FF), ("0:BMB", 0xFFE0), ("*:PAU", 0x07E0)])
    for y in range(BM_MH):
        for x in range(BM_MW):
            bm_tile(s, x, y, m[y][x])
    if door:
        dxx, dyy = door
        bm_tile(s, dxx, dyy, 3)
    if bonus:
        for (bx2, by2) in bonus:
            bm_tile(s, bx2, by2, 4)
    return s


# --- bomberman v1 screens ---
def screen_bomberman():
    m = bm_field(RNG(0x11111111), 42)
    s = bm_v1(m)
    # force a few bricks to open: door + bonus
    bm_tile(s, 5, 2, 3)
    bm_tile(s, 8, 5, 4)
    bm_player(s, 1, 1)
    bm_enemy(s, 9, 6, 0xF81F)
    bm_enemy(s, 11, 8, 0xF81F)
    bm_bomb(s, 6, 4)
    return s


def screen_bomberman_explode():
    m = bm_field(RNG(0x22222222), 42)
    s = bm_v1(m)
    # some bricks destroyed around a blast at (6,5)
    for (x2, y2) in [(6, 5), (7, 5), (5, 5), (6, 6), (6, 4)]:
        bm_fire(s, x2, y2)
    bm_player(s, 1, 1)
    bm_enemy(s, 9, 6, 0xF81F)
    bm_enemy(s, 11, 8, 0xF81F)
    return s


def screen_bomberman_clear():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    s.text(2, 2, "PTS:", 5, 1, rgb24(0xFFFF), rgb24(0x0000))
    box_msg(s, 34, 60, "STAGE CLEAR", 0x07E0)
    return s


def screen_bomberman_over():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    s.text(2, 2, "PTS:", 5, 1, rgb24(0xFFFF), rgb24(0x0000))
    box_msg(s, 36, 60, "GAME OVER", 0xF800)
    return s


# --- bomberman2 screens ---
def bm2_banner(s, score, stage):
    s.fill(0, 0, W, H, 0x0000)
    s.text(2, 2, "PTS:", 4, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(34, 2, str(score), 5, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(84, 2, "ST:", 3, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(106, 2, str(stage), 3, 1, rgb24(0x07E0), rgb24(0x0000))
    s.text(128, 2, "B:EX", 4, 1, rgb24(0xF800), rgb24(0x0000))
    bm_side_help(s, [("2/4/6", 0x07FF), ("5/8:D", 0x07FF), ("0:BMB", 0xFFE0),
                     ("#:DET", 0xF81F), ("*:PAU", 0x07E0)])


def screen_bomberman2():
    m = bm_field(RNG(0xB0B2B0B2), 44)
    s = Screen()
    bm2_banner(s, 250, 1)
    for y in range(BM_MH):
        for x in range(BM_MW):
            bm_tile(s, x, y, m[y][x], BM2_WALL[0], BM2_BRICK[0])
    bm_tile(s, 5, 2, 3)
    bm_tile(s, 8, 5, 4)
    bm_player(s, 1, 1, 0x07FF)
    bm_enemy(s, 9, 6, 0xF81F)
    bm_enemy(s, 11, 3, 0x07FF)
    bm_enemy(s, 11, 9, 0x07E0)
    bm_bomb(s, 3, 7)
    bm_bomb(s, 6, 4)
    return s


def screen_bomberman2_explode():
    m = bm_field(RNG(0x31313131), 44)
    s = Screen()
    bm2_banner(s, 400, 1)
    for y in range(BM_MH):
        for x in range(BM_MW):
            bm_tile(s, x, y, m[y][x], BM2_WALL[1], BM2_BRICK[1])
    for (x2, y2) in [(6, 5), (7, 5), (5, 5), (6, 6), (6, 4)]:
        bm_fire(s, x2, y2)
    bm_player(s, 1, 1, 0x07FF)
    bm_enemy(s, 9, 6, 0xF81F)
    return s


def screen_bomberman2_clear():
    s = Screen()
    bm2_banner(s, 700, 2)
    box_msg(s, 34, 60, "STAGE CLEAR", 0x07E0)
    return s


def screen_bomberman2_over():
    s = Screen()
    bm2_banner(s, 700, 2)
    box_msg(s, 36, 60, "GAME OVER", 0xF800)
    return s


# =====================================================================
# loadrunner
# =====================================================================
LR_LEVEL1 = [
    [0,0,0,0,0,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0],
    [0,0,0,0,5,0,0,0,0,6,0,0,0,0,0,0,0,0,0,0],
    [1,1,1,1,1,1,1,3,1,1,1,1,1,1,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,3,4,4,4,4,4,0,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,3,0,0,8,0,0,3,0,0,0,5,0,0],
    [0,0,0,0,1,1,1,1,1,1,1,3,1,1,1,3,1,1,1,1],
    [0,0,0,0,3,0,0,0,0,0,0,3,0,0,0,3,0,0,0,0],
    [0,0,0,0,3,0,0,5,0,0,0,3,0,0,0,3,0,0,0,0],
    [1,1,1,1,1,1,1,3,1,1,1,1,1,1,1,1,1,1,1,1],
    [0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,0,5,0,0,0,0,3,4,4,4,4,4,0,0,5,0,0,0,0],
    [1,1,1,1,3,1,1,1,1,1,0,0,1,1,1,1,3,1,1,1],
    [0,0,0,0,3,0,0,7,0,0,0,0,0,0,0,0,3,0,8,0],
    [2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2],
]


def lr_tile(s, x, y, t, exit_open=False):
    sx, sy = 10 + x * 7, 2 + y * 8
    s.fill(sx, sy, 7, 8, 0x0000)
    if t == 1:
        s.fill(sx, sy, 7, 8, 0xC200)
        s.fill(sx, sy + 3, 7, 1, 0x0000)
        s.fill(sx + 3, sy, 1, 4, 0x0000)
    elif t == 2:
        s.fill(sx, sy, 7, 8, 0x8410)
        s.fill(sx + 1, sy + 1, 5, 6, 0xC618)
    elif t == 3 or (t == 6 and exit_open):
        c = 0xFFE0 if t == 6 else 0x07FF
        s.fill(sx + 1, sy, 1, 8, c)
        s.fill(sx + 5, sy, 1, 8, c)
        s.fill(sx + 1, sy + 2, 5, 1, c)
        s.fill(sx + 1, sy + 6, 5, 1, c)
    elif t == 4:
        s.fill(sx, sy + 1, 7, 1, 0xC618)
    elif t == 5:
        s.fill(sx + 1, sy + 4, 5, 3, 0xFFE0)
        s.fill(sx + 2, sy + 3, 3, 1, 0xFFFF)


def lr_actor(s, x, y, color):
    sx, sy = 10 + x * 7, 2 + y * 8
    s.fill(sx + 2, sy + 1, 3, 2, 0xFFFF)
    s.fill(sx + 1, sy + 3, 5, 3, color)
    s.fill(sx + 1, sy + 6, 2, 2, color)
    s.fill(sx + 4, sy + 6, 2, 2, color)


def lr_hud(s, pts, gold):
    s.fill(0, 116, W, 12, 0x0000)
    s.text(4, 118, "PTS:", 4, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(30, 118, str(pts), 7, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(82, 118, "GLD:", 4, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(108, 118, str(gold), 3, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(124, 118, "*:P", 3, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(140, 118, "B:X", 3, 1, rgb24(0xF800), rgb24(0x0000))


def lr_level(s, exit_open=False):
    for y in range(14):
        for x in range(20):
            lr_tile(s, x, y, LR_LEVEL1[y][x], exit_open)


def screen_loadrunner():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    lr_level(s)
    lr_actor(s, 5, 12, 0x07E0)
    lr_actor(s, 10, 4, 0xF800)
    lr_actor(s, 18, 12, 0xF800)
    lr_actor(s, 16, 7, 0xF800)
    lr_hud(s, 0, 5)
    return s


def screen_loadrunner_exit():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    lr_level(s, exit_open=True)
    lr_actor(s, 0, 6, 0x07E0)  # climbing up the open exit ladder
    lr_actor(s, 10, 4, 0xF800)
    lr_actor(s, 16, 7, 0xF800)
    lr_hud(s, 500, 0)
    return s


def screen_loadrunner_clear():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    box_msg(s, 40, 56, "STAGE CLEAR", 0xFFE0)
    return s


def screen_loadrunner_over():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    box_msg(s, 44, 56, "GAME OVER", 0xF800)
    return s


# =====================================================================
# mineswiper
# =====================================================================
NUM_COLORS = [0x0000, 0x07FF, 0x07E0, 0xF800, 0x001F, 0x8000,
              0x07E0, 0xF81F, 0xFFFF]


def ms_cell(s, gx, gy, val, cursor):
    sx, sy = 5 + gx * 10, 15 + gy * 10
    if val == 0:
        s.fill(sx, sy, 9, 9, 0x8410)
        s.fill(sx, sy, 9, 1, 0xC618)
        s.fill(sx, sy, 1, 9, 0xC618)
    else:
        s.fill(sx, sy, 9, 9, 0x2104)
        if val == 9:
            s.fill(sx + 2, sy + 2, 5, 5, 0xF800)
            s.fill(sx + 4, sy + 1, 1, 7, 0xFFFF)
            s.fill(sx + 1, sy + 4, 7, 1, 0xFFFF)
        elif val > 0:
            c = NUM_COLORS[val]
            s.text(sx + 2, sy + 1, str(val), 1, 1, rgb24(c), rgb24(0x2104))
    if cursor:
        s.fill(sx, sy, 9, 1, 0xFFE0)
        s.fill(sx, sy + 8, 9, 1, 0xFFE0)
        s.fill(sx, sy, 1, 9, 0xFFE0)
        s.fill(sx + 8, sy, 1, 9, 0xFFE0)


def ms_open_field():
    """field where cells (gx>=2,gy>=2..<=8) opened with numbers; rest closed."""
    grid = [[0] * 15 for _ in range(11)]
    for gy in range(11):
        for gx in range(15):
            if 2 <= gx <= 7 and 2 <= gy <= 8:
                grid[gy][gx] = ((gx + gy) % 8) + 1
    grid[2][8] = 9
    return grid


def ms_banner(s, flags, opened, extra=None):
    s.text(2, 3, "MIN:", 4, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(34, 3, str(flags), 4, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(60, 3, "0:OPN", 5, 1, rgb24(0x07FF), rgb24(0x0000))
    s.text(94, 3, "*:FLG", 5, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(122, 3, "#:NEW", 5, 1, rgb24(0x07E0), rgb24(0x0000))
    s.text(140, 3, "B:X", 3, 1, rgb24(0xF800), rgb24(0x0000))


def screen_mineswiper():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    ms_banner(s, 18, 0)
    g = ms_open_field()
    for gy in range(11):
        for gx in range(15):
            ms_cell(s, gx, gy, g[gy][gx], (gx == 4 and gy == 4))
    return s


def screen_mineswiper_flag():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    ms_banner(s, 17, 0)
    g = ms_open_field()
    for gy in range(11):
        for gx in range(15):
            ms_cell(s, gx, gy, g[gy][gx], (gx == 9 and gy == 4))
    # draw a flag at (9,4) closed cell
    sx, sy = 5 + 9 * 10, 15 + 4 * 10
    s.fill(sx, sy, 9, 9, 0x8410)
    s.fill(sx, sy, 9, 1, 0xC618)
    s.fill(sx, sy, 1, 9, 0xC618)
    s.fill(sx + 3, sy + 2, 1, 6, 0xFFFF)
    s.fill(sx + 4, sy + 2, 3, 3, 0xF800)
    return s


def screen_mineswiper_boom():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    ms_banner(s, 18, 0)
    g = ms_open_field()
    for gy in range(11):
        for gx in range(15):
            if g[gy][gx] == 9:
                ms_cell(s, gx, gy, 9, False)
            elif gx in (3, 5, 9, 12) and gy in (2, 5, 9):
                ms_cell(s, gx, gy, 9, False)  # more mines revealed
            else:
                ms_cell(s, gx, gy, g[gy][gx], False)
    box_msg(s, 44, 60, "BOOM! #:NEW", 0xF800)
    return s


def screen_mineswiper_win():
    s = Screen()
    s.fill(0, 0, W, H, 0x0000)
    ms_banner(s, 0, 143)
    # all safe cells open
    grid = [[0] * 15 for _ in range(11)]
    for gy in range(11):
        for gx in range(15):
            if gy in (2, 5, 8) and gx in (3, 5, 9, 12):
                grid[gy][gx] = 9  # mines hidden
            else:
                grid[gy][gx] = ((gx + gy) % 8) + 1
    for gy in range(11):
        for gx in range(15):
            ms_cell(s, gx, gy, grid[gy][gx], False)
    box_msg(s, 46, 60, "WIN! #:NEW", 0x07E0)
    return s


# =====================================================================
# snake
# =====================================================================
GRID_W, GRID_H, CELLS = 26, 19, 6
OFF2_X, OFF2_Y = 2, 12
COL_HEAD, COL_BODY, COL_FOOD, COL_WALL = 0xFFE0, 0x07E0, 0xF800, 0x5C1F


def snake_cell(s, gx, gy, col):
    s.fill(OFF2_X + gx * CELLS, OFF2_Y + gy * CELLS, CELLS - 1, CELLS - 1, col)


def snake_frame(s):
    s.fill(0, 0, W, H, 0x0000)
    s.fill(OFF2_X - 2, OFF2_Y - 2, GRID_W * CELLS + 3, 2, COL_WALL)
    s.fill(OFF2_X - 2, OFF2_Y + GRID_H * CELLS, GRID_W * CELLS + 3, 2, COL_WALL)
    s.fill(OFF2_X - 2, OFF2_Y, 2, GRID_H * CELLS, COL_WALL)
    s.fill(OFF2_X + GRID_W * CELLS - 1, OFF2_Y, 2, GRID_H * CELLS, COL_WALL)
    s.text(2, 2, "SCORE:", 6, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(92, 2, "*:PAUSE", 6, 1, rgb24(0x07E0), rgb24(0x0000))
    s.text(140, 2, "B:EX", 3, 1, rgb24(0xF800), rgb24(0x0000))


def snake_body(s, head, segs, food):
    s.fill(44, 2, 48, 8, 0x0000)
    s.text(44, 2, str(segs * 10), 8, 1, rgb24(0xFFE0), rgb24(0x0000))
    pts = [(head - i, 9) for i in range(segs)] if False else None
    # draw body
    hsx = head
    for i in range(segs):
        snake_cell(s, hsx - i, 9, COL_HEAD if i == 0 else COL_BODY)
    snake_cell(s, food, 5, COL_FOOD)


def screen_snake():
    s = Screen()
    snake_frame(s)
    snake_body(s, 12, 8, 17)
    return s


def screen_snake_pause():
    s = Screen()
    snake_frame(s)
    snake_body(s, 14, 11, 6)
    box_msg(s, 52, 52, "PAUSED", 0x07E0)
    return s


def screen_snake_over():
    s = Screen()
    snake_frame(s)
    snake_body(s, 13, 18, 19)
    box_msg(s, 52, 60, "GAME OVER", 0xF800)
    return s


# =====================================================================
# tetris
# =====================================================================
TING = [
    [0,0,0,0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,3,4,3,4,3,4,3,4,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,4,3,3,4,4,3,3,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,3,3,4,4,3,3,4,4,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,4,3,4,4,3,4,4,3,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,3,3,4,4,3,3,4,4,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,4,3,3,4,4,3,3,4,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,3,3,4,4,3,3,4,4,3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,4,3,4,4,3,4,4,3,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,5,5,4,4,5,5,4,4,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [4,5,4,4,4,5,4,4,4,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [5,5,4,5,5,4,5,5,4,5,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [5,4,5,5,5,4,5,5,5,4,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
    [0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0],
]
TETRO_OFF_X, TETRO_OFF_Y = 12, 4


def tetris_cell(s, bx, by, c):
    s.fill(TETRO_OFF_X + bx * 6, TETRO_OFF_Y + by * 6, 5, 5,
           0x0000 if c == 0 else [0x0000, 0x07FF, 0xFFE0, 0xF81F, 0x07E0,
                                  0xF800, 0x001F, 0xFD20][c])


def tetris_banner(s, score):
    s.fill(0, 0, W, H, 0x0000)
    s.fill(TETRO_OFF_X - 2, TETRO_OFF_Y - 2, 10 * 6 + 4, 2, 0xFFFF)
    s.fill(TETRO_OFF_X - 2, TETRO_OFF_Y + 20 * 6, 10 * 6 + 4, 2, 0xFFFF)
    s.fill(TETRO_OFF_X - 2, TETRO_OFF_Y, 2, 20 * 6, 0xFFFF)
    s.fill(TETRO_OFF_X + 10 * 6, TETRO_OFF_Y, 2, 20 * 6, 0xFFFF)
    s.text(82, 8, "SCORE:", 10, 1, rgb24(0xFFFF), rgb24(0x0000))
    s.text(82, 24, str(score), 10, 1, rgb24(0xFFE0), rgb24(0x0000))
    s.text(82, 46, "4:LEFT", 10, 1, rgb24(0x07FF), rgb24(0x0000))
    s.text(82, 58, "6:RIGHT", 10, 1, rgb24(0x07FF), rgb24(0x0000))
    s.text(82, 70, "5:ROTATE", 10, 1, rgb24(0x07FF), rgb24(0x0000))
    s.text(82, 82, "8:DOWN", 10, 1, rgb24(0x07FF), rgb24(0x0000))
    s.text(82, 92, "7/9:DROP", 10, 1, rgb24(0x07FF), rgb24(0x0000))
    s.text(82, 104, "*:PAUSE", 10, 1, rgb24(0x07E0), rgb24(0x0000))
    s.text(82, 114, "Back:EXIT", 10, 1, rgb24(0xF800), rgb24(0x0000))


def tetris_board(s, filled):
    for by in range(20):
        for bx in range(10):
            tetris_cell(s, bx, by, filled[by][bx])


def tetris_filled_from_tail(tail_rows):
    f = [[0] * 10 for _ in range(20)]
    for by in range(tail_rows, 20):
        for bx in range(10):
            f[by][bx] = TING[by][bx] or 0
    return f


def screen_tetris():
    s = Screen()
    tetris_banner(s, 300)
    tetris_board(s, tetris_filled_from_tail(10))
    # falling T piece
    for (px, py) in [(4, 0), (5, 0), (6, 0), (5, 1)]:
        tetris_cell(s, px, py, 3)
    return s


def screen_tetris_cleared():
    s = Screen()
    tetris_banner(s, 600)
    # a cleared row (empty in the middle) then new pieces
    f = tetris_filled_from_tail(10)
    for bx in range(10):
        f[19][bx] = 0  # cleared bottom row
    tetris_board(s, f)
    for (px, py) in [(3, 0), (4, 0), (5, 0), (4, 1)]:
        tetris_cell(s, px, py, 4)  # S piece landing
    return s


def screen_tetris_pause():
    s = Screen()
    tetris_banner(s, 600)
    tetris_board(s, tetris_filled_from_tail(10))
    box_msg(s, 62, 56, "PAUSED", 0x07E0)
    return s


def screen_tetris_over():
    s = Screen()
    tetris_banner(s, 300)
    tetris_board(s, tetris_filled_from_tail(4))
    box_msg(s, 40, 56, "GAME OVER", 0xF800)
    return s


def main():
    out = os.path.dirname(os.path.abspath(__file__))
    os.makedirs(out, exist_ok=True)
    jobs = [
        ("screen_8ballpool", screen_8ballpool),
        ("screen_8ballpool_pause", screen_8ballpool_pause),
        ("screen_8ballpool_win", screen_8ballpool_win),
        ("screen_battlecity", screen_battlecity),
        ("screen_battlecity_pause", screen_battlecity_pause),
        ("screen_battlecity_clear", screen_battlecity_clear),
        ("screen_battlecity_over", screen_battlecity_over),
        ("screen_bomberman", screen_bomberman),
        ("screen_bomberman_explode", screen_bomberman_explode),
        ("screen_bomberman_clear", screen_bomberman_clear),
        ("screen_bomberman_over", screen_bomberman_over),
        ("screen_bomberman2", screen_bomberman2),
        ("screen_bomberman2_explode", screen_bomberman2_explode),
        ("screen_bomberman2_clear", screen_bomberman2_clear),
        ("screen_bomberman2_over", screen_bomberman2_over),
        ("screen_loadrunner", screen_loadrunner),
        ("screen_loadrunner_exit", screen_loadrunner_exit),
        ("screen_loadrunner_clear", screen_loadrunner_clear),
        ("screen_loadrunner_over", screen_loadrunner_over),
        ("screen_mineswiper", screen_mineswiper),
        ("screen_mineswiper_flag", screen_mineswiper_flag),
        ("screen_mineswiper_boom", screen_mineswiper_boom),
        ("screen_mineswiper_win", screen_mineswiper_win),
        ("screen_snake", screen_snake),
        ("screen_snake_pause", screen_snake_pause),
        ("screen_snake_over", screen_snake_over),
        ("screen_tetris", screen_tetris),
        ("screen_tetris_cleared", screen_tetris_cleared),
        ("screen_tetris_pause", screen_tetris_pause),
        ("screen_tetris_over", screen_tetris_over),
    ]
    for name, fn in jobs:
        save(os.path.join(out, name + ".png"), fn())
        print("wrote", name + ".png")


if __name__ == "__main__":
    main()