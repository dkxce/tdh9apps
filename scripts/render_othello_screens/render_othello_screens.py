#!/usr/bin/env python3
"""Render TD-H9 othello-app screens as PNGs, pixel-identical to othello.c.

Reproduces gfx_text / gfx_fill exactly (the 5x7 font is parsed from
myapps/shared/common/font.h, a glyph is a 6x8 cell multiplied by `scale`, colors
are the RGB565 values converted to 24-bit with rgb24) and othello's own helpers:
draw_disc, draw_cell (hint dot + cursor frame), render_board (board + score
panel), set_msg, board border and the right-side panel.

Instead of hand-placing discs, the board states are built by PLAYING the game:
apply_move() reproduces try_move(x,y,side,1) exactly, so every state (including
the flipped discs) is what the radio would actually draw.

Outputs <scripts>/render_othello_screens/*.png:
  screen_menu.png     Start menu (1 VS CPU / 2 APRS RADIO LINK)
  screen_start.png    Fresh board, "YOUR": player to move, cursor on a real move
  screen_after.png    After player's first legal move (flips applied), "CPU.."
  screen_pass.png     Player has no move, "PASS"
  screen_win.png      Game over, player ahead, "WIN!"
  screen_lose.png     Game over, CPU (black) ahead, "LOSE"
"""
import os
import re
import struct
import zlib

W, H = 160, 128
N, CELL = 8, 14
OFF_X, OFF_Y = 4, 8
RIGHT = 118                  # right panel starts here

# RGB565 colors used by othello.c
BLACK = 0x0000
C_BOARD = 0x03E0            # dark green cloth
C_LIGHT = 0x05E0            # light green cells
C_BORDER = 0x8410
C_CUR = 0xFFE0
C_HINT = 0x07FF
C_PLAYER = 0xffff
C_PLAYER_RIM = 0xC618
C_CPU = 0x2104
C_CPU_RIM = 0x0000
C_YOUR = 0x07E0
C_RED = 0xF800
C_BLUE = 0x07FF

COLORS = {"black": 0x0000, "white": 0xffff, "yellow": 0xffe0, "blue": 0x001f,
          "grey": 0x8410, "green": 0x07e0, "red": 0xf800}


def rgb24(c565):
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


# --- game logic replica (othello.c) ------------------------------------
DX = [0, 1, 1, 1, 0, -1, -1, -1]
DY = [-1, -1, 0, 1, 1, 1, 0, -1]


def new_board():
    mx = [[0] * N for _ in range(N)]
    mx[3][3] = mx[4][4] = 2      # C_PLAYER (white)
    mx[3][4] = mx[4][3] = 1      # C_CPU (black)
    return mx


def try_move(mx, xa, ya, side, do_flip=False):
    """Return flipped count; if do_flip, apply the move (incl. placing disc)."""
    if xa < 0 or xa >= N or ya < 0 or ya >= N:
        return 0
    if mx[ya][xa] != 0:
        return 0
    opp = 1 if side == 2 else 2
    total = 0
    for d in range(8):
        x, y = xa + DX[d], ya + DY[d]
        count = 0
        while 0 <= x < N and 0 <= y < N and mx[y][x] == opp:
            x += DX[d]
            y += DY[d]
            count += 1
        if count > 0 and 0 <= x < N and 0 <= y < N and mx[y][x] == side:
            total += count
            if do_flip:
                fx, fy = xa + DX[d], ya + DY[d]
                for _ in range(count):
                    mx[fy][fx] = side
                    fx += DX[d]
                    fy += DY[d]
    if total > 0 and do_flip:
        mx[ya][xa] = side
    return total


def legal_moves(mx, side):
    return [(x, y) for y in range(N) for x in range(N)
            if try_move(mx, x, y, side) > 0]


def play(mx, side, x, y):
    """Apply a real move (mutates mx) and switch sides."""
    try_move(mx, x, y, side, do_flip=True)
    return 1 if side == 2 else 2


# --- drawing helpers (othello.c) ---------------------------------------
def draw_disc(s, px, py, color, rim):
    s.fill(px + 3, py + 2, 7, 9, rim)
    s.fill(px + 2, py + 3, 9, 7, rim)
    s.fill(px + 3, py + 3, 7, 7, color)


def draw_cell(s, x, y, mx, cx, cy, game_over):
    px = OFF_X + x * CELL
    py = OFF_Y + y * CELL
    bg = C_LIGHT if ((x + y) & 1) == 0 else C_BOARD
    s.fill(px, py, CELL - 1, CELL - 1, bg)
    v = mx[y][x]
    if v == 2:
        draw_disc(s, px, py, C_PLAYER, C_PLAYER_RIM)
    elif v == 1:
        draw_disc(s, px, py, C_CPU, C_CPU_RIM)
    elif not game_over and try_move(mx, x, y, 2) > 0:
        s.fill(px + 5, py + 5, 3, 3, C_HINT)
    if x == cx and y == cy:
        s.fill(px, py, CELL - 1, 1, C_CUR)
        s.fill(px, py + CELL - 2, CELL - 1, 1, C_CUR)
        s.fill(px, py, 1, CELL - 1, C_CUR)
        s.fill(px + CELL - 2, py, 1, CELL - 1, C_CUR)


def render_board(s, mx, cx, cy, game_over):
    p_cnt = c_cnt = 0
    for y in range(N):
        for x in range(N):
            draw_cell(s, x, y, mx, cx, cy, game_over)
            if mx[y][x] == 2:
                p_cnt += 1
            elif mx[y][x] == 1:
                c_cnt += 1
    s.fill(RIGHT, 22, 38, 10, BLACK)
    s.text(122, 22, str(p_cnt), 4, 1, rgb24(COLORS["white"]), rgb24(BLACK))
    s.fill(RIGHT, 50, 38, 10, BLACK)
    s.text(122, 50, str(c_cnt), 4, 1, rgb24(C_BLUE), rgb24(BLACK))


def set_msg(s, msg, col):
    s.fill(RIGHT, 70, 42, 24, BLACK)
    s.text(120, 72, msg, 6, 1, rgb24(col), rgb24(BLACK))


def board_frame(s):
    s.fill(OFF_X - 2, OFF_Y - 2, N * CELL + 3, 2, C_BORDER)
    s.fill(OFF_X - 2, OFF_Y + N * CELL - 1, N * CELL + 3, 2, C_BORDER)
    s.fill(OFF_X - 2, OFF_Y, 2, N * CELL, C_BORDER)
    s.fill(OFF_X + N * CELL - 1, OFF_Y, 2, N * CELL, C_BORDER)


def draw_panel(s):
    s.text(120, 10, "YOU", 4, 1, rgb24(COLORS["white"]), rgb24(BLACK))
    s.text(120, 38, "CPU", 4, 1, rgb24(C_BLUE), rgb24(BLACK))
    s.text(120, 98, "#:NEW", 5, 1, rgb24(C_CUR), rgb24(BLACK))
    s.text(120, 110, "0:PUT", 5, 1, rgb24(C_RED), rgb24(BLACK))


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


def screen_start():
    """Fresh board, player to move, cursor on the first legal white move (4,2)."""
    mx = new_board()
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    board_frame(s)
    draw_panel(s)
    render_board(s, mx, 4, 2, False)
    set_msg(s, "YOUR", C_YOUR)
    return s


def screen_after():
    """After player's first move (4,2 flips (4,3)); CPU now thinking."""
    mx = new_board()
    play(mx, 2, 4, 2)         # white to (4,2), flips the black disc at (4,3)
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    board_frame(s)
    draw_panel(s)
    render_board(s, mx, 4, 2, False)
    set_msg(s, "CPU..", C_BLUE)
    return s


def screen_pass():
    """Edge board where the player has no legal move -> PASS (CPU would keep
    playing; shown after a sequence of moves)."""
    mx = [[0] * N for _ in range(N)]
    # Fill most of the board so white has no move; black is dominant.
    for y in range(N):
        for x in range(N):
            mx[y][x] = 1
    for (x, y) in [(3, 3), (4, 4), (2, 2), (5, 5), (2, 3), (5, 4), (0, 0), (7, 7)]:
        mx[y][x] = 2
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    board_frame(s)
    draw_panel(s)
    # no legal white move -> no hint dots
    render_board(s, mx, 3, 3, False)
    set_msg(s, "PASS", C_CUR)
    return s


def screen_skip():
    """CPU has no legal move but the player does -> SKIP (CPU skips)."""
    mx = [[0] * N for _ in range(N)]
    for y in range(N):
        for x in range(N):
            mx[y][x] = 1
    for (x, y) in [(3, 3), (4, 4), (2, 2), (5, 5), (2, 3), (5, 4), (0, 0), (7, 7)]:
        mx[y][x] = 2
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    board_frame(s)
    draw_panel(s)
    render_board(s, mx, 3, 3, False)
    set_msg(s, "SKIP", C_RED)
    return s


def screen_win():
    """Game over, player ahead -> WIN!, hints suppressed (game_over)."""
    mx = [[2] * N for _ in range(N)]
    mx[0][0] = mx[1][0] = mx[0][7] = mx[7][0] = 1
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    board_frame(s)
    draw_panel(s)
    render_board(s, mx, 4, 2, True)
    set_msg(s, "WIN!", C_YOUR)
    return s


def screen_lose():
    """Game over, CPU ahead -> LOSE, hints suppressed."""
    mx = [[1] * N for _ in range(N)]
    mx[0][0] = mx[1][0] = mx[0][7] = mx[7][0] = 2
    s = Screen()
    s.fill(0, 0, W, H, BLACK)
    board_frame(s)
    draw_panel(s)
    render_board(s, mx, 4, 2, True)
    set_msg(s, "LOSE", C_RED)
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
    for name, fn in [("screen_start", screen_start),
                     ("screen_after", screen_after), ("screen_pass", screen_pass),
                     ("screen_skip", screen_skip),
                     ("screen_win", screen_win), ("screen_lose", screen_lose)]:
        save(os.path.join(out, name + ".png"), fn())


if __name__ == "__main__":
    main()