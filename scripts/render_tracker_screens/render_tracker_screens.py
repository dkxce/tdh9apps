#!/usr/bin/env python3
"""Render TD-H9 tracker-app screens as PNGs, pixel-identical to tracker.c and
the .TRK modules.

Reproduces gfx_text / gfx_fill exactly (the 5x7 font is parsed from
myapps/shared/common/font.h, a glyph is a 6x8 cell multiplied by `scale`, colors
are the GFX_* RGB565 values converted to 24-bit) and the tracker's own drawing
helpers: view_line (Bresenham), view_circle, view_spoke / view_arrow (Q10
sin/cos via sin_deg), the value panel, the recording status, the scale bar and
the compass dial. Coordinates and strings match the app.

Outputs <scripts>/render_tracker_screens/*.png:
  screen_menu.png        TRACKER menu (NAVTO selected, IDLE)
  screen_navto.png       NAV TO sub-menu (FIXED/CURRENT/START/PAUSE/STOP)
  screen_fixed_lat.png   FIXED LAT coordinate editor (digit cursor)
  screen_radar.png       RADAR, split with values, target NE
  screen_map.png         MAP, split, start/track/target/own position
  screen_points.png      POINTS list (distance + time from start)
  screen_point.png       POINT detail (number, dist, elapsed, time, SHOW ...)
  screen_settings.png    SETTINGS (store pts, step dist, filter, step time/mode)
  screen_step_dist.png   STEP DIST picker (10M..5000KM, current marked)
"""
import os
import re
import struct
import zlib

W, H = 160, 128
VIEW_W = 106          # radar / map area in split mode
MY = H // 2

# GFX colors (RGB565) -> names
COLORS = {"black": 0x0000, "white": 0xffff, "yellow": 0xffe0, "blue": 0x001f,
          "grey": 0x8410, "green": 0x07e0, "red": 0xf800}
C_DGREY = 0x4208
C_LGREY = 0xC618
C_CYAN = 0x07ff
C_FUCHSIA = 0xF81F


def rgb24(c565):
    """Expand a 565 to (r, g, b), reproducing gfx.c's 8-bit conversion."""
    r, g, b = (c565 >> 11) & 0x1f, (c565 >> 5) & 0x3f, c565 & 0x1f
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


# --- parse the real 5x7 font from font.h --------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))
# script sits at <project>/scripts/render_tracker_screens/ -> two levels up is the project root
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

    # --- tracker primitives (trk_view.c) --------------------------------
    def pixel(self, x, y, c, clip):
        if 0 <= x < clip and 0 <= y < H:
            self.fill(x, y, 1, 1, c)

    def line(self, x0, y0, x1, y1, c, clip):
        dx = x1 - x0 if x1 >= x0 else x0 - x1
        dy = y1 - y0 if y1 >= y0 else y0 - y1
        sx = 1 if x0 < x1 else -1
        sy = 1 if y0 < y1 else -1
        err = dx - dy
        while True:
            self.pixel(x0, y0, c, clip)
            if x0 == x1 and y0 == y1:
                break
            e2 = err * 2
            if e2 > -dy:
                err -= dy
                x0 += sx
            if e2 < dx:
                err += dx
                y0 += sy

    def circle(self, cx, cy, r, c, clip):
        x, y, err = r, 0, 1 - r
        while x >= y:
            for dx, dy in ((x, y), (-x, y), (x, -y), (-x, -y),
                           (y, x), (-y, x), (y, -x), (-y, -x)):
                self.pixel(cx + dx, cy + dy, c, clip)
            y += 1
            if err < 0:
                err += 2 * y + 1
            else:
                x -= 1
                err += 2 * (y - x) + 1

    def spoke(self, cx, cy, a, r):
        return cx + sin_deg(a) * r // 1024, cy - cos_deg(a) * r // 1024

    def arrow(self, cx, cy, a, r, c, clip):
        tx, ty = self.spoke(cx, cy, a, r)
        self.line(cx, cy, tx, ty, c, clip)
        for da in (a + 12, a + 348):
            x, y = self.spoke(cx, cy, da, r * 3 // 4)
            self.line(tx, ty, x, y, c, clip)


# --- tracker navigation math (trk_core.c) --------------------------------
def sin_deg(a):
    T = [0, 178, 350, 512, 658, 784, 887, 962, 1008, 1024]
    a %= 360
    neg = a >= 180
    a %= 180
    if a > 90:
        a = 180 - a
    i, f = a // 10, a % 10
    v = T[i] + ((T[i + 1] - T[i]) * f // 10 if i < 9 else 0)
    return -v if neg else v


def cos_deg(a):
    return sin_deg(a + 90)


def tan_q10(d):
    return sin_deg(d) * 1024 // cos_deg(d)


def atan_oct(num, den):
    r = num * 1024 // den
    d = 0
    while d < 45 and tan_q10(d + 1) <= r:
        d += 1
    if d < 45 and tan_q10(d + 1) - r < r - tan_q10(d):
        d += 1
    return d


def nav_delta(lat1, lon1, lat2, lon2):
    alat = abs(lat1) // 1000000
    east = 116726 * cos_deg(alat)
    dy = ((lat2 - lat1) * 116726) >> 20
    dx = ((lon2 - lon1) * east) >> 30
    return dx, dy


def isqrt(n):
    if not n:
        return 0
    x = n
    y = x // 2 + 1
    while y < x:
        x = y
        y = (x + n // x) // 2
    return x


def nav_distance(dx, dy):
    ax, ay = abs(dx), abs(dy)
    if ax < 32768 and ay < 32768:
        return isqrt(ax * ax + ay * ay)
    return isqrt((ax // 1000) ** 2 + (ay // 1000) ** 2) * 1000


def nav_bearing(dx, dy):
    ax, ay = abs(dx), abs(dy)
    if not ax and not ay:
        return 0
    a = atan_oct(ax, ay) if ax <= ay else 90 - atan_oct(ay, ax)
    if dy >= 0:
        return a if dx >= 0 else (360 - a) % 360
    return 180 - a if dx >= 0 else 180 + a


# --- text helpers matching trk_core.c / trk_track.c ----------------------
def fixed(v, frac):
    m = abs(v)
    neg = "-" if v < 0 else ""
    if not frac:
        return neg + str(m)
    return neg + str(m // 10 ** frac) + "." + str(m % 10 ** frac).zfill(frac)


def km_text(m):
    if m < 10000:
        return fixed(m, 3)
    if m < 100000:
        return fixed(m // 10, 2)
    if m < 1000000:
        return fixed(m // 100, 1)
    return str(m // 1000)


def deg_text(micro):
    if micro >= 100000000 or micro <= -100000000:
        return fixed(micro // 100, 4)
    return fixed(micro // 10, 5)


def time_text(sec):
    return "%d:%02d:%02d" % (sec // 3600, sec // 60 % 60, sec % 60)


def dist_label(m):
    if m < 1000:
        return "%dM" % m
    if m % 1000:
        return "%d.%dKM" % (m // 1000, m % 1000 // 100)
    return "%dKM" % (m // 1000)


# --- shared menu drawing (trk_menu.c) ------------------------------------
ROW_Y, ROW_H, ROWS, STATUS_Y = 14, 10, 10, 118
STATUS = ["STATUS: IDLE", "STATUS: RECORDING...", "STATUS: PAUSED"]


def menu_title(s, title):
    b = "-- " + title + " --"
    w = len(b) * 6
    s.text((W - w) // 2, 2, b, 26, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))


def menu_row(s, y, n, label, selected):
    bg = rgb24(C_DGREY) if selected else rgb24(COLORS["black"])
    s.fill(0, y, W, ROW_H, bg)
    s.text(4, y + 1, "[%d] %s" % (n, label), 26, 1, rgb24(COLORS["white"]), bg)


def status_line(s, rec):
    s.fill(0, STATUS_Y - 3, W, 1, C_DGREY)
    s.text(4, STATUS_Y, STATUS[rec], 20, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))


def draw_menu(s, title, items, selected, rec=0):
    s.fill(0, 0, W, H, rgb24(COLORS["black"]))
    menu_title(s, title)
    status_line(s, rec)
    for i, label in enumerate(items):
        menu_row(s, ROW_Y + i * ROW_H, i + 1, label, i == selected)


def screen_menu():
    """tracker.c: TRACKER menu, NAVTO selected, IDLE."""
    s = Screen()
    draw_menu(s, "TRACKER", ["NAVTO", "RADAR", "MAP", "POINTS", "SETTINGS",
                             "HELP", "ABOUT"], selected=0, rec=0)
    return s


def screen_navto():
    """tracker.c: NAV TO sub-menu, FIXED selected."""
    s = Screen()
    draw_menu(s, "NAV TO", ["FIXED", "CURRENT", "START", "PAUSE", "STOP"],
              selected=0, rec=1)
    return s


def screen_fixed_lat():
    """fixed.c edit("LAT") with a digit cursor."""
    s = Screen()
    s.fill(0, 0, W, H, rgb24(COLORS["black"]))
    menu_title(s, "LAT")
    # "N 55.751230": cells N|sp|5|5|.|7|5|1|2|3|0 -> cell 8 (1-based) = digit "2"
    s.text(8, 44, "N 55.751230", 11, 2, rgb24(COLORS["white"]), rgb24(COLORS["black"]))
    s.fill(8, 61, 132, 2, rgb24(COLORS["black"]))     # clear underline
    s.fill(8 + (8 - 1) * 12, 61, 12, 2, rgb24(COLORS["white"]))  # cursor under digit 2
    s.text(4, 96, "0-9 DIGIT  UP/DN MOVE", 21, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))
    s.text(4, 110, "MENU OK  BACK CANCEL", 20, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))
    return s


# --- radar (radar.c) ------------------------------------------------------
def radar_dial(s, cx, cy, target_brg, target_col, course):
    s.fill(0, 0, VIEW_W, H, rgb24(COLORS["black"]))
    s.fill(VIEW_W, 0, 1, H, C_DGREY)
    s.fill(VIEW_W + 1, 0, W - VIEW_W - 1, H, rgb24(COLORS["black"]))
    s.circle(cx, cy, 50, rgb24(COLORS["grey"]), VIEW_W)
    s.fill(cx - 50, cy, 101, 1, C_DGREY)
    s.fill(cx, cy - 50, 1, 101, C_DGREY)
    for i in range(6):          # red north triangle, apex up
        s.fill(cx - i, cy - 61 + i, 2 * i + 1, 1, rgb24(COLORS["red"]))
    for a, t in ((0, "N"), (90, "E"), (180, "S"), (270, "W")):
        x, y = s.spoke(cx, cy, a, 42)
        s.text(x - 3, y - 4, t, 1, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))
    if course is not None:
        s.arrow(cx, cy, course, 36, C_LGREY, VIEW_W)
    if target_brg is not None:
        s.arrow(cx, cy, target_brg, 38, target_col, VIEW_W)


def view_value(s, row, label, value, color):
    s.text(VIEW_W + 2, row * 18, label, 9, 1, rgb24(COLORS["grey"]), rgb24(COLORS["black"]))
    s.text(VIEW_W + 2, row * 18 + 8, (value or "---").ljust(9)[:9], 9, 1,
           rgb24(color), rgb24(COLORS["black"]))


def view_status(s, x, y, rec):
    HALF = [1, 2, 3, 4, 4, 4, 3, 2, 1]
    s.fill(x, y, 9, 9, rgb24(COLORS["black"]))
    if rec == 2:
        s.fill(x + 1, y, 3, 9, rgb24(COLORS["yellow"]))
        s.fill(x + 5, y, 3, 9, rgb24(COLORS["yellow"]))
        return
    col = rgb24((COLORS["red"], COLORS["green"], COLORS["yellow"])[rec])
    for i in range(9):
        s.fill(x + 4 - HALF[i], y + i, 2 * HALF[i] + 1, 1, col)


def screen_radar():
    """radar.c: split, recording, target to the north-east."""
    s = Screen()
    cx = 52
    own = (55751230, 37618420)
    tgt = (55754230, 37621020)
    dx, dy = nav_delta(own[0], own[1], tgt[0], tgt[1])
    brg = nav_bearing(dx, dy)
    course = 30
    radar_dial(s, cx, 64, brg, rgb24(COLORS["green"]), course)
    view_status(s, 2, 2, 1)                        # green dot: RECORDING
    view_value(s, 0, "DST KM", km_text(nav_distance(dx, dy)), COLORS["white"])
    view_value(s, 1, "HDG", "%03d" % course, COLORS["white"])
    view_value(s, 2, "BRG", "%03d" % brg, COLORS["yellow"])
    view_value(s, 3, "SPD KMH", fixed(55, 1), COLORS["white"])
    view_value(s, 4, "LAT", "55.75123", COLORS["white"])
    view_value(s, 5, "LON", "37.61842", COLORS["white"])
    view_value(s, 6, "ALT M", "142", COLORS["white"])
    s.text(2, 119, "SATS:5 PTS:5", 17, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))
    return s


# --- map (map.c) ---------------------------------------------------------
def screen_map():
    """map.c: split, north up; start green, track cyan, target red, own white."""
    s = Screen()
    mx = VIEW_W // 2
    scale = 1000                      # default zoom=4 -> 1KM
    start = (55749030, 37617420)
    track = [(55749030, 37617420), (55749530, 37617700), (55750130, 37618030),
             (55750830, 37618500), (55751530, 37619100), (55752230, 37619900)]
    target = (55754230, 37621020)
    own = (55751230, 37618420)

    def pos(lat, lon):
        dx, dy = nav_delta(own[0], own[1], lat, lon)
        return mx + dx * 40 // scale, MY - dy * 40 // scale

    s.fill(0, 0, VIEW_W, H, rgb24(COLORS["black"]))
    s.fill(VIEW_W, 0, 1, H, C_DGREY)
    s.fill(VIEW_W + 1, 0, W - VIEW_W - 1, H, rgb24(COLORS["black"]))

    pts = [pos(*t) for t in track]
    prev = None
    for p in pts:
        if prev:
            s.line(prev[0], prev[1], p[0], p[1], C_CYAN, VIEW_W)
        else:
            s.pixel(p[0], p[1], C_CYAN, VIEW_W)
        prev = p
    sx, sy = pos(*start)
    s.circle(sx, sy, 3, rgb24(COLORS["green"]), VIEW_W)
    tx, ty = pos(*target)
    s.circle(tx, ty, 3, rgb24(COLORS["red"]), VIEW_W)
    ox, oy = mx, MY                          # own position at the centre
    s.line(ox - 3, oy, ox + 3, oy, rgb24(COLORS["white"]), VIEW_W)
    s.line(ox, oy - 3, ox, oy + 3, rgb24(COLORS["white"]), VIEW_W)

    # north pointer + NAV arrow
    s.arrow(4, 14, 0, 12, rgb24(COLORS["white"]), VIEW_W)
    s.text(9, 2, "N", 1, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))
    s.text(2, 18, "NAV", 3, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))
    bd = nav_bearing(*nav_delta(own[0], own[1], target[0], target[1]))
    s.arrow(28, 21, bd, 7, rgb24(COLORS["green"]), VIEW_W)

    # scale bar: 1KM per 40 px
    s.fill(4, 122, 41, 1, rgb24(COLORS["white"]))
    s.fill(4, 118, 1, 4, rgb24(COLORS["white"]))
    s.fill(4 + 40, 118, 1, 4, rgb24(COLORS["white"]))
    s.text(4, 108, "1KM", 6, 1, rgb24(COLORS["white"]), rgb24(COLORS["black"]))

    view_status(s, W - 11, 1, 1)
    view_value(s, 0, "FIX", "OK:5", COLORS["green"])
    view_value(s, 1, "DST KM", km_text(nav_distance(*nav_delta(own[0], own[1], target[0], target[1]))), COLORS["white"])
    view_value(s, 2, "HDG", "030", COLORS["white"])
    view_value(s, 3, "BRG", "%03d" % nav_bearing(*nav_delta(own[0], own[1], target[0], target[1])), COLORS["yellow"])
    view_value(s, 4, "ALT M", "142", COLORS["white"])
    view_value(s, 5, "SPD KMH", fixed(55, 1), COLORS["white"])
    view_value(s, 6, "PTS", "5", C_CYAN)
    return s


# --- points (points.c) ---------------------------------------------------
def screen_points():
    """points.c: POINTS list, first point selected, distance + elapsed."""
    s = Screen()
    pts = ["0.000KM 0:00:00", "0.085KM 0:02:02", "0.160KM 0:04:05",
           "0.240KM 0:06:10", "0.310KM 0:08:15"]
    draw_menu(s, "POINTS", pts, selected=0, rec=1)
    return s


def screen_point():
    """points.c: POINT detail, point 3 (1-based), SHOW selected."""
    s = Screen()
    items = ["NUMBER 3", "DIST 0.160KM", "ELAPSED 0:02:03",
             "TIME 12:34:05", "EST DIST 0.212KM", "SHOW", "NAVIGATE"]
    draw_menu(s, "POINT", items, selected=5, rec=1)
    return s


# --- settings (setup.c) ---------------------------------------------------
STEP_M = [10, 25, 50, 75, 100, 150, 200, 250, 300, 350, 400, 450,
          500, 1000, 2500, 5000]


def screen_settings():
    """setup.c: SETTINGS menu with current values (cap 160, step 10M, filter ON)."""
    s = Screen()
    items = ["STORE PTS 160", "STEP DIST 10M", "FILTER 2KMH ON",
             "STEP TIME 30S", "STEP MODE DIST", "CLEAR LOG"]
    draw_menu(s, "SETTINGS", items, selected=0, rec=1)
    return s


def screen_step_dist():
    """setup.c: STEP DIST picker; the current step (10M) marked with '<'."""
    s = Screen()
    items = ["%s <" % dist_label(m) if m == STEP_M[0] else dist_label(m)
             for m in STEP_M]
    draw_menu(s, "STEP DIST", items, selected=0, rec=1)
    return s


# --- PNG writer (same as render_lock_screens.py) -------------------------
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
    save(os.path.join(out, "screen_navto.png"), screen_navto())
    save(os.path.join(out, "screen_fixed_lat.png"), screen_fixed_lat())
    save(os.path.join(out, "screen_radar.png"), screen_radar())
    save(os.path.join(out, "screen_map.png"), screen_map())
    save(os.path.join(out, "screen_points.png"), screen_points())
    save(os.path.join(out, "screen_point.png"), screen_point())
    save(os.path.join(out, "screen_settings.png"), screen_settings())
    save(os.path.join(out, "screen_step_dist.png"), screen_step_dist())


if __name__ == "__main__":
    main()