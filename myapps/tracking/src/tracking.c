/* tracking.c - GPS tracking / navigation app for the TD-H9.
 *
 * Pick a target (the current fix or typed coordinates), then follow it on a
 * compass (distance, bearing, arrow) or on a map (track, vector to target,
 * scale bar).
 *
 * GPS: while a module runs, stock's main loop is stopped, so its parsed fix
 * (0x12550 valid, 0x12558 lat, 0x12560 lon) is frozen at the value it had on
 * entry. It is used only as the starting fix. Live positions come from the
 * NMEA the APRS modem forwards into the main-side serial ring (frames
 * C0 <cmd> <payload> C0, cmd 0x10), drained here in stock's place, as
 * modules/gps/gps.c in h9toolbox does.
 *
 * Memory: no .data/.bss in a module and the task stack is small, so all
 * state lives in the 2 KiB below the request mailbox at the top of the RAM
 * slot (as aprs_monitor.c). The module body must end below STATE_ADDR,
 * i.e. stay under 6,112 bytes.
 *
 * Keys: digit d = d + 2, 12 Menu, 13 Up, 14 Down, 15 Back.
 *   select:  1 = target here, 2 = type coordinates, Back = leave
 *   editor:  0-9 digit (on N/S, E/W any digit flips it), Up/Down move,
 *            Menu = go, Back = cancel
 *   nav:     Menu = compass / map, Back = change target
 *
 * Integer only: no floats, no libm, no libc beyond the shared gfx lib. */
#include "h9mod.h"
#include "gfx.h"

#define C_CYAN 0x07ff

#define GPS_VALID (*(const volatile uint8_t *)0x12550)
#define GPS_LAT (*(const volatile int32_t *)0x12558)
#define GPS_LON (*(const volatile int32_t *)0x12560)

#define RING ((volatile uint8_t *)0x16634)
#define RING_WRITE (*(volatile uint16_t *)0x16E34)
#define RING_READ (*(volatile uint16_t *)0x16E36)
#define RING_MASK 2047u
#define TICK (*(const volatile uint32_t *)0x1D8B0)   /* 100 Hz */

enum { NMEA = 84, MAX_TRACK = 200, MIN_DIST_M = 4, FIX_TIMEOUT = 500, STATE_BYTES = 0x800 };
enum { MODE_SELECT, MODE_EDIT, MODE_COMPASS, MODE_MAP };

#define STATE_ADDR (H9_SHARED_TOP - STATE_BYTES)

typedef struct {
    int sign_lat, sign_lon;
    uint8_t lat[7];   /* dd.ddddd */
    uint8_t lon[8];   /* ddd.ddddd */
    unsigned cur;     /* 0 N/S, 1-7 lat digits, 8 E/W, 9-16 lon digits */
} editor_t;

struct state {
    unsigned cmd, frame_len, escape, nmea_len;
    char nmea[NMEA];
    int32_t lat, lon;
    unsigned course, spd10;   /* RMC course (degrees), speed (knots x 10) */
    uint32_t fix_tick;
    unsigned have_fix, fresh;
    unsigned key, key_seen, mode, dirty, shown_live;
    uint32_t last_draw;
    int32_t tlat, tlon;
    editor_t ed;
    unsigned count;
    int32_t t_lat[MAX_TRACK], t_lon[MAX_TRACK];
};
_Static_assert(sizeof(struct state) <= STATE_BYTES, "state must fit below the request mailbox");

/* ---- text ------------------------------------------------------------------ */

static char *put(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }
static char *num(char *d, uint32_t v, unsigned width)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n < width) t[n++] = '0';
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}
static char *dist_text(char *d, uint32_t m)
{
    if (m < 10000) { d = num(d, m, 1); return put(d, "m"); }
    d = num(d, m / 1000, 1);
    if (m < 100000) { *d++ = '.'; d = num(d, m / 100 % 10, 1); }
    return put(d, "km");
}

/* ---- NMEA (gps.c-derived: RMC and GGA position only) ------------------------ */

static unsigned dig(char c) { return c >= '0' && c <= '9' ? (unsigned)(c - '0') : 0; }

/* "-12.34" -> value x 10^frac */
static int32_t val(const char *s, unsigned frac)
{
    int neg = *s == '-';
    uint32_t v = 0;
    if (neg) s++;
    while (*s >= '0' && *s <= '9') v = v * 10 + dig(*s++);
    if (*s == '.') s++;
    for (unsigned k = 0; k < frac; k++) v = v * 10 + (*s >= '0' && *s <= '9' ? dig(*s++) : 0);
    return neg ? -(int32_t)v : (int32_t)v;
}

/* ddmm.mmmm / dddmm.mmmm + hemisphere -> signed microdegrees; 0 = unusable */
static int coord(const char *f, unsigned deg_digits, char hemi, int32_t *out)
{
    uint32_t deg = 0;
    unsigned i;
    for (i = 0; i < deg_digits; i++) deg = deg * 10 + dig(f[i]);
    if (!f[i] || f[i + 2] != '.') return 0;
    uint32_t micro = deg * 1000000u + (uint32_t)val(f + i, 4) * 5u / 3u;
    if (hemi == 'S' || hemi == 'W') *out = -(int32_t)micro;
    else if (hemi == 'N' || hemi == 'E') *out = (int32_t)micro;
    else return 0;
    return 1;
}

static void got_fix(struct state *s, int32_t lat, int32_t lon)
{
    s->lat = lat;
    s->lon = lon;
    s->fix_tick = TICK;
    s->have_fix = 1;
    s->fresh = 1;
}

static void sentence(struct state *s)
{
    char *p = s->nmea, *f[16];
    unsigned n = 0;
    int32_t la, lo;
    if (s->nmea_len < 6 || p[0] != '$') return;
    for (unsigned i = 0; i < s->nmea_len && n < 16; i++) {
        if (p[i] == '*') { p[i] = 0; break; }
        if (p[i] == ',') { p[i] = 0; f[n++] = p + i + 1; }
    }
    if (p[3] == 'R' && p[4] == 'M' && p[5] == 'C' && n >= 8) {
        if (f[1][0] != 'A') return;
        if (!coord(f[2], 2, f[3][0], &la) || !coord(f[4], 3, f[5][0], &lo)) return;
        s->spd10 = (unsigned)val(f[6], 1);
        s->course = (unsigned)val(f[7], 0) % 360;
        got_fix(s, la, lo);
    } else if (p[3] == 'G' && p[4] == 'G' && p[5] == 'A' && n >= 6) {
        if (!val(f[5], 0)) return;
        if (coord(f[1], 2, f[2][0], &la) && coord(f[3], 3, f[4][0], &lo)) got_fix(s, la, lo);
    }
}

static void nmea_byte(struct state *s, char c)
{
    if (c == '$') s->nmea_len = 0;
    if (c == '\r' || c == '\n') {
        if (s->nmea_len) { s->nmea[s->nmea_len] = 0; sentence(s); }
        s->nmea_len = 0;
        return;
    }
    if (s->nmea_len < NMEA - 1) s->nmea[s->nmea_len++] = c;
}

/* Drain the modem ring in stock's place: C0 <cmd> <payload> C0, DB DC = C0, DB DD = DB. */
static void poll_ring(struct state *s)
{
    unsigned write = RING_WRITE & RING_MASK, read = RING_READ & RING_MASK;
    while (read != write) {
        uint8_t b = RING[read];
        read = (read + 1) & RING_MASK;
        if (b == 0xC0) { s->frame_len = 0; s->escape = 0; continue; }
        if (s->escape) { b = b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b; s->escape = 0; }
        else if (b == 0xDB) { s->escape = 1; continue; }
        if (!s->frame_len) { s->cmd = b; s->frame_len = 1; }
        else if (s->cmd == 0x10) nmea_byte(s, (char)b);
    }
    RING_READ = (uint16_t)read;
}

/* ---- trigonometry (Q10, gps.c table) ----------------------------------------- */

static int sin_deg(unsigned a)
{
    static const uint16_t T[10] = { 0, 178, 350, 512, 658, 784, 887, 962, 1008, 1024 };
    a %= 360;
    int neg = a >= 180;
    a %= 180;
    if (a > 90) a = 180 - a;
    unsigned i = a / 10, f = a % 10;
    int v = (int)T[i] + (i < 9 ? ((int)T[i + 1] - (int)T[i]) * (int)f / 10 : 0);
    return neg ? -v : v;
}
static int cos_deg(unsigned a) { return sin_deg(a + 90); }
static int32_t tan_q10(unsigned d) { return (int32_t)sin_deg(d) * 1024 / cos_deg(d); }

/* atan(num / den) in degrees, 0 <= num <= den, den > 0: 0..45 */
static unsigned atan_oct(uint32_t num, uint32_t den)
{
    while (den >= (1u << 20)) { num >>= 1; den >>= 1; }
    int32_t r = (int32_t)(num * 1024u / den);
    unsigned d = 0;
    while (d < 45 && tan_q10(d + 1) <= r) d++;
    if (d < 45 && tan_q10(d + 1) - r < r - tan_q10(d)) d++;
    return d;
}

/* ---- navigation (microdegrees in, metres out, tangent plane) ------------------ */

/* East / north offset in metres from point 1 to point 2. 0.11132 m per
 * microdegree is 116726 in Q20; 32 x 32 -> 64 multiplies as in fix_store.h. */
static void nav_delta(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2, int32_t *dx, int32_t *dy)
{
    unsigned alat = (unsigned)(lat1 < 0 ? -lat1 : lat1) / 1000000u;
    int32_t east = 116726 * cos_deg(alat);
    *dy = (int32_t)(((int64_t)(lat2 - lat1) * 116726) >> 20);
    *dx = (int32_t)(((int64_t)(lon2 - lon1) * east) >> 30);
}

static uint32_t isqrt(uint32_t n)
{
    if (!n) return 0;
    uint32_t x = n, y = x / 2 + 1;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

static uint32_t nav_distance(int32_t dx, int32_t dy)
{
    uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx), ay = (uint32_t)(dy < 0 ? -dy : dy);
    if (ax < 32768 && ay < 32768) return isqrt(ax * ax + ay * ay);
    ax /= 1000;
    ay /= 1000;
    return isqrt(ax * ax + ay * ay) * 1000;
}

/* Compass bearing (0 = north, clockwise) of the offset dx east, dy north. */
static unsigned nav_bearing(int32_t dx, int32_t dy)
{
    uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx), ay = (uint32_t)(dy < 0 ? -dy : dy);
    if (!ax && !ay) return 0;
    unsigned a = ax <= ay ? atan_oct(ax, ay) : 90 - atan_oct(ay, ax);
    if (dy >= 0) return dx >= 0 ? a : (360 - a) % 360;
    return dx >= 0 ? 180 - a : 180 + a;
}

/* ---- track ------------------------------------------------------------------ */

static void track_add(struct state *s, int32_t lat, int32_t lon)
{
    if (s->count) {
        int32_t dx, dy;
        nav_delta(s->t_lat[s->count - 1], s->t_lon[s->count - 1], lat, lon, &dx, &dy);
        if (nav_distance(dx, dy) < MIN_DIST_M) return;
    }
    if (s->count >= MAX_TRACK) {
        for (unsigned i = 1; i < MAX_TRACK; i++) { s->t_lat[i - 1] = s->t_lat[i]; s->t_lon[i - 1] = s->t_lon[i]; }
        s->count = MAX_TRACK - 1;
    }
    s->t_lat[s->count] = lat;
    s->t_lon[s->count] = lon;
    s->count++;
}

static void track_restart(struct state *s, int live)
{
    s->count = 0;
    if (live) track_add(s, s->lat, s->lon);
}

/* ---- drawing primitives ----------------------------------------------------- */

static void px(int x, int y, uint16_t c)
{
    if (x < 0 || x >= H9_WIDTH || y < 0 || y >= H9_HEIGHT) return;
    gfx_fill((unsigned)x, (unsigned)y, 1, 1, c);
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 < x0 ? x0 - x1 : x1 - x0;
    int dy = y1 < y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

static void circle(int cx, int cy, int r, uint16_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        px(cx + x, cy + y, c); px(cx - x, cy + y, c);
        px(cx + x, cy - y, c); px(cx - x, cy - y, c);
        px(cx + y, cy + x, c); px(cx - y, cy + x, c);
        px(cx + y, cy - x, c); px(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* Point at compass angle a, radius r from (cx, cy). */
static void spoke(int cx, int cy, unsigned a, int r, int *x, int *y)
{
    *x = cx + sin_deg(a) * r / 1024;
    *y = cy - cos_deg(a) * r / 1024;
}

/* ---- map view --------------------------------------------------------------- */

static void draw_map(const struct state *s, uint32_t dist_m)
{
    enum { MX = 4, MY = 6, MW = 116, MH = 116 };
    static const uint32_t STEP[] = { 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000,
                                     100000, 200000, 500000, 1000000 };
    gfx_fill(MX, MY, MW + 1, 1, GFX_GREY);
    gfx_fill(MX, MY + MH, MW + 1, 1, GFX_GREY);
    gfx_fill(MX, MY, 1, MH, GFX_GREY);
    gfx_fill(MX + MW, MY, 1, MH, GFX_GREY);

    /* extent in metres around the current fix (origin 0, 0) */
    int32_t dx, dy, tx, ty;
    int32_t minx = 0, maxx = 0, miny = 0, maxy = 0;
    nav_delta(s->lat, s->lon, s->tlat, s->tlon, &tx, &ty);
    if (tx < minx) minx = tx;
    if (tx > maxx) maxx = tx;
    if (ty < miny) miny = ty;
    if (ty > maxy) maxy = ty;
    for (unsigned i = 0; i < s->count; i++) {
        nav_delta(s->lat, s->lon, s->t_lat[i], s->t_lon[i], &dx, &dy);
        if (dx < minx) minx = dx;
        if (dx > maxx) maxx = dx;
        if (dy < miny) miny = dy;
        if (dy > maxy) maxy = dy;
    }
    int32_t ext = maxx - minx > maxy - miny ? maxx - minx : maxy - miny;
    if (ext < 50) ext = 50;
    int32_t mpp = ext / (MW - 16) + 1;   /* metres per pixel */
    int32_t midx = (minx + maxx) / 2, midy = (miny + maxy) / 2;
    const int cx = MX + MW / 2, cy = MY + MH / 2;
#define TOX(x) (cx + (int)(((x) - midx) / mpp))
#define TOY(y) (cy - (int)(((y) - midy) / mpp))

    int lx = 0, ly = 0;
    for (unsigned i = 0; i < s->count; i++) {
        nav_delta(s->lat, s->lon, s->t_lat[i], s->t_lon[i], &dx, &dy);
        if (i) line(lx, ly, TOX(dx), TOY(dy), GFX_BLUE);
        lx = TOX(dx);
        ly = TOY(dy);
    }
    line(TOX(0), TOY(0), TOX(tx), TOY(ty), GFX_YELLOW);
    circle(TOX(tx), TOY(ty), 3, GFX_RED);
    px(TOX(tx), TOY(ty), GFX_WHITE);
    circle(TOX(0), TOY(0), 3, GFX_GREEN);
#undef TOX
#undef TOY

    /* scale bar: the longest step that fits in 50 px */
    unsigned c = 0;
    while (c + 1 < sizeof STEP / sizeof STEP[0] && STEP[c + 1] / (uint32_t)mpp <= 50) c++;
    int bar = (int)(STEP[c] / (uint32_t)mpp);
    char b[12];
    if (bar >= 4) {
        const int sbx = MX + 5, sby = MY + MH - 5;
        gfx_fill(sbx, sby, (unsigned)bar + 1, 1, GFX_WHITE);
        gfx_fill(sbx, sby - 3, 1, 3, GFX_WHITE);
        gfx_fill(sbx + bar, sby - 3, 1, 3, GFX_WHITE);
        dist_text(b, STEP[c]);
        gfx_text(sbx, sby - 12, b, 6, 1, GFX_WHITE, GFX_BLACK);
    }

    gfx_text(124, 10, "DIST", 4, 1, GFX_GREY, GFX_BLACK);
    dist_text(b, dist_m);
    gfx_text(124, 22, b, 6, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(124, 48, "PTS", 3, 1, GFX_GREY, GFX_BLACK);
    num(b, s->count, 1);
    gfx_text(124, 62, b, 4, 1, C_CYAN, GFX_BLACK);
    gfx_text(124, 92, "MENU", 4, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(124, 106, "View", 4, 1, GFX_GREY, GFX_BLACK);
}

/* ---- compass view ------------------------------------------------------------ */

/* The arrow follows the course while moving (heading up), else north up. */
static void draw_compass(const struct state *s, uint32_t dist_m, unsigned brg)
{
    enum { CX = 45, CY = 64, R = 35 };
    int moving = s->spd10 >= 11;   /* about 2 km/h: RMC course is noise below that */
    unsigned up = moving ? s->course : 0;
    unsigned arrow = (brg + 360 - up) % 360;
    int tx, ty, lx, ly, rx, ry;

    circle(CX, CY, R, GFX_WHITE);
    spoke(CX, CY, (360 - up) % 360, R + 6, &tx, &ty);
    gfx_text((unsigned)(tx - 3), (unsigned)(ty - 4), "N", 1, 1, GFX_RED, GFX_BLACK);
    spoke(CX, CY, arrow, R - 4, &tx, &ty);
    spoke(CX, CY, arrow + 140, R * 6 / 10, &lx, &ly);
    spoke(CX, CY, arrow + 220, R * 6 / 10, &rx, &ry);
    line(tx, ty, lx, ly, GFX_GREEN);
    line(tx, ty, rx, ry, GFX_GREEN);
    line(lx, ly, CX, CY, GFX_GREEN);
    line(rx, ry, CX, CY, GFX_GREEN);

    char b[12];
    gfx_text(88, 20, "DST:", 4, 1, GFX_WHITE, GFX_BLACK);
    dist_text(b, dist_m);
    gfx_text(88, 32, b, 10, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(88, 44, "BRG:", 4, 1, GFX_YELLOW, GFX_BLACK);
    num(b, brg, 3);
    gfx_text(88, 56, b, 3, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(88, 68, moving ? "HEAD UP" : "NORTH UP", 8, 1, GFX_GREY, GFX_BLACK);
    gfx_text(88, 92, "MENU Map", 8, 1, GFX_GREY, GFX_BLACK);
}

/* ---- coordinate editor -------------------------------------------------------- */

static void editor_init(editor_t *e, int32_t lat, int32_t lon)
{
    uint32_t v = (uint32_t)(lat < 0 ? -lat : lat) / 10;   /* 1e-5 degree */
    e->sign_lat = lat < 0 ? -1 : 1;
    for (int i = 6; i >= 0; i--) { e->lat[i] = (uint8_t)(v % 10); v /= 10; }
    v = (uint32_t)(lon < 0 ? -lon : lon) / 10;
    e->sign_lon = lon < 0 ? -1 : 1;
    for (int i = 7; i >= 0; i--) { e->lon[i] = (uint8_t)(v % 10); v /= 10; }
    e->cur = 1;
}

static void editor_to_deg(const editor_t *e, int32_t *lat, int32_t *lon)
{
    int32_t la = 0, lo = 0;
    for (int i = 0; i < 7; i++) la = la * 10 + e->lat[i];
    for (int i = 0; i < 8; i++) lo = lo * 10 + e->lon[i];
    if (la > 9000000) la = 9000000;
    if (lo > 18000000) lo = 18000000;
    *lat = la * 10 * e->sign_lat;
    *lon = lo * 10 * e->sign_lon;
}

/* -1 cancel, 1 go, 0 stay */
static int editor_key(editor_t *e, unsigned k)
{
    if (k == H9_KEY_BACK) return -1;
    if (k == H9_KEY_MENU) return 1;
    if (k == H9_KEY_UP) e->cur = e->cur ? e->cur - 1 : 16;
    else if (k == H9_KEY_DOWN) e->cur = e->cur < 16 ? e->cur + 1 : 0;
    else if (k >= 2 && k <= 11) {
        uint8_t d = (uint8_t)(k - 2);
        if (e->cur == 0) e->sign_lat = -e->sign_lat;
        else if (e->cur <= 7) e->lat[e->cur - 1] = d;
        else if (e->cur == 8) e->sign_lon = -e->sign_lon;
        else e->lon[e->cur - 9] = d;
        if (e->cur < 16) e->cur++;
    }
    return 0;
}

static void editor_draw(const editor_t *e)
{
    char b[16];
    int j = 0, col;
    gfx_text(4, 4, "ENTER WAYPOINT", 14, 1, GFX_YELLOW, GFX_BLACK);

    b[j++] = e->sign_lat > 0 ? 'N' : 'S';
    b[j++] = ' ';
    for (int i = 0; i < 7; i++) {
        if (i == 2) b[j++] = '.';
        b[j++] = (char)('0' + e->lat[i]);
    }
    b[j] = 0;
    gfx_text(4, 28, b, 11, 2, GFX_WHITE, GFX_BLACK);

    j = 0;
    b[j++] = e->sign_lon > 0 ? 'E' : 'W';
    b[j++] = ' ';
    for (int i = 0; i < 8; i++) {
        if (i == 3) b[j++] = '.';
        b[j++] = (char)('0' + e->lon[i]);
    }
    b[j] = 0;
    gfx_text(4, 58, b, 11, 2, GFX_WHITE, GFX_BLACK);

    /* underline the cursor: character column in the line above, 12 px cells */
    if (e->cur == 0) col = 0;
    else if (e->cur <= 2) col = (int)e->cur + 1;
    else if (e->cur <= 7) col = (int)e->cur + 2;
    else if (e->cur == 8) col = 0;
    else if (e->cur <= 11) col = (int)e->cur - 7;
    else col = (int)e->cur - 6;
    gfx_fill((unsigned)(4 + col * 12), e->cur <= 7 ? 45 : 75, 12, 2, GFX_YELLOW);

    gfx_text(4, 96, "0-9 DIGIT  UP/DN MOVE", 21, 1, GFX_GREY, GFX_BLACK);
    gfx_text(4, 110, "MENU GO  BACK CANCEL", 20, 1, GFX_GREY, GFX_BLACK);
}

/* ---- screens -------------------------------------------------------------------- */

static void draw(const struct state *s, int live)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    if (s->mode == MODE_SELECT) {
        gfx_text(4, 15, "SELECT TARGET:", 14, 1, GFX_YELLOW, GFX_BLACK);
        gfx_text(4, 45, "1 Current GPS Pos", 17, 1, live ? GFX_GREEN : GFX_GREY, GFX_BLACK);
        gfx_text(4, 65, "2 Manual Input", 14, 1, GFX_WHITE, GFX_BLACK);
        gfx_text(4, 100, live ? "GPS: FIX OK" : "GPS: SEARCHING...", 17, 1, C_CYAN, GFX_BLACK);
        gfx_text(4, 115, "BACK Quit", 9, 1, GFX_GREY, GFX_BLACK);
    } else if (s->mode == MODE_EDIT) {
        editor_draw(&s->ed);
    } else if (!live) {
        gfx_text(10, 45, "WAITING FOR GPS...", 18, 1, GFX_RED, GFX_BLACK);
        gfx_text(10, 70, "BACK Change Target", 18, 1, GFX_GREY, GFX_BLACK);
    } else {
        int32_t dx, dy;
        nav_delta(s->lat, s->lon, s->tlat, s->tlon, &dx, &dy);
        uint32_t dist = nav_distance(dx, dy);
        if (s->mode == MODE_COMPASS) draw_compass(s, dist, nav_bearing(dx, dy));
        else draw_map(s, dist);
    }
}

/* A key once two readings agree (the keypad ADC has no debounce). */
static unsigned key_press(struct state *s)
{
    unsigned k = h9_key();
    if (k != s->key) { s->key = k; return 0; }
    if (k == s->key_seen) return 0;
    s->key_seen = k;
    return k;
}

/* 1 to leave the module */
static int handle_key(struct state *s, unsigned k, int live)
{
    if (s->mode == MODE_SELECT) {
        if (k == H9_KEY_1 && live) {
            s->tlat = s->lat;
            s->tlon = s->lon;
            track_restart(s, live);
            s->mode = MODE_COMPASS;
        } else if (k == H9_KEY_2) {
            editor_init(&s->ed, s->have_fix ? s->lat : 44609800, s->have_fix ? s->lon : 40100600);
            s->mode = MODE_EDIT;
        } else if (k == H9_KEY_BACK) return 1;
        else return 0;
    } else if (s->mode == MODE_EDIT) {
        int r = editor_key(&s->ed, k);
        if (r < 0) s->mode = MODE_SELECT;
        else if (r > 0) {
            editor_to_deg(&s->ed, &s->tlat, &s->tlon);
            track_restart(s, live);
            s->mode = MODE_COMPASS;
        }
    } else {
        if (k == H9_KEY_BACK) s->mode = MODE_SELECT;
        else if (k == H9_KEY_MENU) s->mode = s->mode == MODE_COMPASS ? MODE_MAP : MODE_COMPASS;
        else return 0;
    }
    s->dirty = 1;
    return 0;
}

/* ---- module entry ------------------------------------------------------------------ */

unsigned module_main(const struct h9_api *api, unsigned self)
{
    struct state *s = (struct state *)STATE_ADDR;
    uint8_t *raw = (uint8_t *)s;
    (void)api; (void)self;
    for (unsigned i = 0; i < sizeof *s; i++) raw[i] = 0;

    if (GPS_VALID) {   /* stock's last fix before the module started */
        got_fix(s, GPS_LAT, GPS_LON);
        s->fresh = 0;
    }
    s->key = s->key_seen = h9_key();
    s->mode = MODE_SELECT;
    s->dirty = 1;

    for (;;) {
        poll_ring(s);
        uint32_t now = TICK;
        int live = s->have_fix && now - s->fix_tick < FIX_TIMEOUT;

        if (s->fresh) {
            s->fresh = 0;
            track_add(s, s->lat, s->lon);
            if (s->mode >= MODE_COMPASS) s->dirty = 1;
        }
        unsigned k = key_press(s);
        if (k && handle_key(s, k, live)) break;
        if ((unsigned)live != s->shown_live) s->dirty = 1;
        if (s->mode >= MODE_COMPASS && now - s->last_draw >= 100) s->dirty = 1;

        if (s->dirty) {
            draw(s, live);
            s->shown_live = (unsigned)live;
            s->last_draw = now;
            s->dirty = 0;
        }
        h9_delay(10);
    }
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    return 0;
}
