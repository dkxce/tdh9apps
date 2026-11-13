/* tracking.c вЂ” GPS tracking / navigation app for the TD-H9.
 *
 * Adapted from a vector-graphics "sketch" into a real module. It:
 *   - reads the live stock GPS fix straight from RAM:
 *       0x12550  byte   fix valid (1 = fix)
 *       0x12558  i32    latitude,  signed microdegrees
 *       0x12560  i32    longitude, signed microdegrees
 *   - draws the track, a vector to the target and a compass, all in integer
 *     math (no libm): haversine distance, initial bearing, and a Q10
 *     sin/cos table (as in h9toolbox modules/gps/gps.c).
 *   - keys via h9_key(): digits 3..11 (0-9), 12 Menu, 13 Up, 14 Down,
 *     15 Back. Uses 1/2 in the mode select like the sketch; OK = 12 toggles
 *     compass/map; Back = 15 leaves / cancels.
 *
 * Pure integer: no floats, no <math.h>, no libc beyond the shared gfx lib.
 */
#include "h9mod.h"
#include "gfx.h"

/* extra colour not present in gfx.h */
#define C_CYAN 0x07ff

/* ---- live GPS fix (stock NMEA parser) -------------------------------- */
#define GPS_VALID (*(volatile uint8_t  *)0x12550)
#define GPS_LAT   (*(volatile int32_t  *)0x12558)
#define GPS_LON   (*(volatile int32_t  *)0x12560)

/* Q10 sine/cosine (gps.c-derived, integer only) ------------------------- */
static int sin_deg(unsigned a)
{
    static const uint16_t T[10] = { 0, 178, 350, 512, 658, 784, 887, 962, 1008, 1024 };
    int neg = a >= 180;
    a %= 180;
    if (a > 90) a = 180 - a;
    unsigned i = a / 10, f = a % 10;
    int v = (int)T[i] + (i < 9 ? ((int)T[i + 1] - (int)T[i]) * (int)f / 10 : 0);
    return neg ? -v : v;
}
static int cos_deg(unsigned a) { return sin_deg((a + 90) % 360); }

/* ---- pixel / line / circle primitives ---------------------------------- */
static void px(int x, int y, uint16_t c)
{
    if (x < 0 || x >= H9_WIDTH || y < 0 || y >= H9_HEIGHT) return;
    gfx_fill((unsigned)x, (unsigned)y, 1, 1, c);
}

/* Bresenham line segment. */
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
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

/* Outline circle (midpoint algorithm). */
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

/* ---- navigation (integer, microdegrees) -------------------------------- */
#define R_EARTH 6371000   /* metres */

/* integer square root (Newton) */
static long isqrt(long n)
{
    if (n <= 0) return 0;
    long x = n, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + n / x) / 2; }
    return x;
}

/* Great-circle distance in metres between two points (microdegrees).
 * Integer tangent-plane approximation, early-divided to avoid long overflow. */
static long nav_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    long cosl = cos_deg((unsigned)((lat1 >= 0 ? (unsigned)lat1 : -(unsigned)lat1) / 1000000) % 360);
    long dlat = (long)(lat2 - lat1);
    long dlon = (long)(lon2 - lon1);
    /* 0.11132 m per microdegree of latitude */
    long dy = (dlat / 100) * 111 / 10;            /* metres */
    long dx = ((dlon / 100) * 111 / 10) * cosl / 1024;   /* metres */
    long dxa = dx > 0 ? dx : -dx, dya = dy > 0 ? dy : -dy;
    if (dxa > 40000 || dya > 40000) return dxa > dya ? dxa : dya; /* >40 km: square would overflow */
    return isqrt(dx * dx + dy * dy);
}

/* Initial bearing in degrees (0-359) from p1 to p2 (microdegrees).
 * Integer atan2 via the Q10 table: map the dy/dx ratio through asin-like
 * arctan using the sin table (acceptable for navigation). */
/* Initial bearing in degrees (0-359) from p1 to p2 (microdegrees).
 * Integer atan2 approximation. */
static unsigned nav_bearing_deg(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    long cosl = cos_deg((unsigned)((lat1 >= 0 ? (unsigned)lat1 : -(unsigned)lat1) / 1000000) % 360);
    long dx = ((long)(lon2 - lon1) / 256) * cosl / 4;   /* scale to avoid overflow */
    long dy = (long)(lat2 - lat1) / 256;
    unsigned ang = 0;
    if (dx == 0 && dy == 0) return 0;
    if (dx == 0) ang = dy > 0 ? 90 : 270;
    else {
        long ax = dx > 0 ? dx : -dx, ay = dy > 0 ? dy : -dy;
        /* normalise so the larger axis is in [1000..2000] to keep tan in Q10 */
        long big = ax > ay ? ax : ay;
        long s = big / 1500 + 1;
        long nx = ax / s, ny = ay / s;
        if (nx == 0) nx = 1;
        if (ny == 0) ny = 1;
        long r = (ax >= ay) ? (ny * 1024) / nx : (nx * 1024) / ny;  /* |slope| in Q10 */
        for (int d = 0; d <= 45; d++) {
            long t = (long)sin_deg((unsigned)d) * 1024 / cos_deg((unsigned)d);
            if (t >= r) { ang = (unsigned)d; break; }
        }
        if (ang == 0 && r > 1024) ang = 45;
        if (ax < ay) ang = 90 - ang;
    }
    if (dx < 0 && dy >= 0) ang = 180 - ang;
    else if (dx < 0 && dy < 0) ang = 180 + ang;
    else if (dx >= 0 && dy < 0) ang = 360 - (ang ? ang : 360);
    return (unsigned)(ang % 360);
}

/* ---- track state --------------------------------------------------------- */
#define MAX_TRACK 250
#define MIN_DIST_M 4  /* ~4 m min spacing between track points (metres) */

static int32_t t_lat[MAX_TRACK], t_lon[MAX_TRACK];
static int t_count;

static void track_add(int32_t lat, int32_t lon)
{
    if (t_count) {
        long d = nav_distance_m(t_lat[t_count - 1], t_lon[t_count - 1], lat, lon);
        if (d < MIN_DIST_M) return;
    }
    if (t_count >= MAX_TRACK) {
        for (int i = 1; i < MAX_TRACK; i++) { t_lat[i - 1] = t_lat[i]; t_lon[i - 1] = t_lon[i]; }
        t_count = MAX_TRACK - 1;
    }
    t_lat[t_count] = lat; t_lon[t_count] = lon; t_count++;
}

/* ---- map view (160x128) -------------------------------------------------- */
static void draw_map(int32_t clat, int32_t clon, int32_t tlat, int32_t tlon, long dist_m)
{
    const int MX = 4, MY = 6, MW = 116, MH = 116;
    line(MX, MY, MX + MW, MY, GFX_GREY);
    line(MX, MY + MH, MX + MW, MY + MH, GFX_GREY);
    line(MX, MY, MX, MY + MH, GFX_GREY);
    line(MX + MW, MY, MX + MW, MY + MH, GFX_GREY);

    int32_t mnlat = clat, mxlat = clat, mnlon = clon, mxlon = clon;
    if (tlat < mnlat) mnlat = tlat; if (tlat > mxlat) mxlat = tlat;
    if (tlon < mnlon) mnlon = tlon; if (tlon > mxlon) mxlon = tlon;
    for (int i = 0; i < t_count; i++) {
        if (t_lat[i] < mnlat) mnlat = t_lat[i]; if (t_lat[i] > mxlat) mxlat = t_lat[i];
        if (t_lon[i] < mnlon) mnlon = t_lon[i]; if (t_lon[i] > mxlon) mxlon = t_lon[i];
    }
    /* widen empty spans */
    if (mxlat - mnlat < 200) { int32_t m = (mxlat + mnlat) / 2; mnlat = m - 100; mxlat = m + 100; }
    if (mxlon - mnlon < 200) { int32_t m = (mxlon + mnlon) / 2; mnlon = m - 100; mxlon = m + 100; }

    int cosl = cos_deg((unsigned)((clat >= 0 ? (unsigned)clat : -(unsigned)clat) / 1000000) % 360);
    long dlat = (long)(mxlat - mnlat) / 4;
    long dlon = ((long)(mxlon - mnlon) / 4) * cosl / 1024;
    long span = dlat > dlon ? dlat : dlon;
    if (span < 1) span = 1;

    int cx = MX + MW / 2, cy = MY + MH / 2;
    int32_t clatc = (mnlat + mxlat) / 2, clonc = (mnlon + mxlon) / 2;
#define TOX(lo) (cx + (int)((((long)((lo) - clonc) / 4) * cosl / 1024) * (MW - 16) / span))
#define TOY(la) (cy - (int)((((long)((la) - clatc) / 4)) * (MH - 16) / span))

    /* track */
    for (int i = 1; i < t_count; i++)
        line(TOX(t_lon[i - 1]), TOY(t_lat[i - 1]), TOX(t_lon[i]), TOY(t_lat[i]), GFX_BLUE);
    /* vector to target */
    line(TOX(clon), TOY(clat), TOX(tlon), TOY(tlat), GFX_YELLOW);
    circle(TOX(tlon), TOY(tlat), 3, GFX_RED); px(TOX(tlon), TOY(tlat), GFX_WHITE);
    circle(TOX(clon), TOY(clat), 3, GFX_GREEN);
#undef TOX
#undef TOY

    /* scale bar: metres per pixel. span is the lat/lon extent in
     * microdegrees; ~0.11133 m per microdegree of latitude. */
    long span_m = span * 111 / 100;                 /* metres (approx, avoids overflow) */
    long mpp = span_m / (MW - 16);                  /* metres per pixel */
    long steps[] = { 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
    long chosen = 10000; int bar = 30;
    for (int i = 0; i < 10; i++) {
        int p = (int)(steps[i] / (mpp ? mpp : 1));
        if (p >= 18 && p <= 52) { chosen = steps[i]; bar = p; break; }
        if (p > 52) { chosen = i ? steps[i - 1] : 10; bar = (int)(chosen / (mpp ? mpp : 1)); if (bar < 8) bar = 8; if (bar > 60) bar = 60; break; }
    }
    int sbx = MX + 5, sby = MY + MH - 5;
    line(sbx, sby, sbx + bar, sby, GFX_WHITE);
    line(sbx, sby - 3, sbx, sby, GFX_WHITE);
    line(sbx + bar, sby - 3, sbx + bar, sby, GFX_WHITE);
    char sb[8];
    if (chosen < 1000) { sb[0]='0'+(char)(chosen/10%10); sb[1]='0'+(char)(chosen%10); sb[2]='m'; sb[3]=0; }
    else { long k = chosen / 1000; sb[0]=(char)('0'+k); sb[1]='k'; sb[2]='m'; sb[3]=0; }
    gfx_text((unsigned)sbx, (unsigned)(sby - 12), sb, 4, 1, GFX_WHITE, GFX_BLACK);

    /* telemetry panel */
    gfx_text(124, 10, "DIST", 4, 1, GFX_GREY, GFX_BLACK);
    char bm[10]; int bj = 0;
    if (dist_m >= 100000) {
        long kk = dist_m / 1000;                       /* km */
        if (kk >= 100) { bm[bj++] = '0' + (char)(kk / 100); }
        if (kk >= 10)  { bm[bj++] = '0' + (char)((kk / 10) % 10); }
        bm[bj++] = '0' + (char)(kk % 10);
        bm[bj++] = 'k'; bm[bj++] = 'm';
    } else {
        long mm = dist_m;                               /* m */
        if (mm >= 1000) { bm[bj++] = '0' + (char)(mm / 1000); }
        if (mm >= 100)  { bm[bj++] = '0' + (char)((mm / 100) % 10); }
        if (mm >= 10)   { bm[bj++] = '0' + (char)((mm / 10) % 10); }
        bm[bj++] = '0' + (char)(mm % 10);
        bm[bj++] = 'm';
    }
    bm[bj] = 0;
    gfx_text(124, 22, bm, 9, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(124, 48, "PTS", 3, 1, GFX_GREY, GFX_BLACK);
    char p2[6]; int pj = 0;
    if (t_count >= 100) { p2[pj++] = '0' + (char)((t_count / 100) % 10); }
    if (t_count >= 10)  { p2[pj++] = '0' + (char)((t_count / 10) % 10); }
    p2[pj++] = '0' + (char)(t_count % 10); p2[pj] = 0;
    gfx_text(124, 62, p2, 4, 1, C_CYAN, GFX_BLACK);
    gfx_text(124, 92, "[OK]", 4, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(124, 106, "View", 4, 1, GFX_GREY, GFX_BLACK);
}

/* compass view */
static void draw_compass(long dist_m, unsigned brg, unsigned arrow)
{
    int cx = 45, cy = 64, r = 35;
    circle(cx, cy, r, GFX_WHITE);
    int rad = (int)((arrow + 360) % 360);
    int tx = cx + cos_deg((unsigned)((rad + 90) % 360)) * (r - 4) / 1024;
    int ty = cy - sin_deg((unsigned)((rad + 90) % 360)) * (r - 4) / 1024;
    int lx = cx + cos_deg((unsigned)((rad + 140 - 90 + 360) % 360)) * (r * 60 / 100) / 1024;
    int ly = cy - sin_deg((unsigned)((rad + 140 - 90 + 360) % 360)) * (r * 60 / 100) / 1024;
    int rx = cx + cos_deg((unsigned)((rad - 140 - 90 + 720) % 360)) * (r * 60 / 100) / 1024;
    int ry = cy - sin_deg((unsigned)((rad - 140 - 90 + 720) % 360)) * (r * 60 / 100) / 1024;
    line(tx, ty, lx, ly, GFX_GREEN); line(tx, ty, rx, ry, GFX_GREEN);
    line(lx, ly, cx, cy, GFX_GREEN); line(rx, ry, cx, cy, GFX_GREEN);
    (void)tx; (void)ty; (void)lx; (void)ly; (void)rx; (void)ry;

    char b2[10]; int j = 0;
    if (dist_m >= 100000) {
        long km = dist_m / 1000;
        if (km >= 100) { b2[j++] = '0' + (char)(km / 100); }
        if (km >= 10)  { b2[j++] = '0' + (char)((km / 10) % 10); }
        b2[j++] = '0' + (char)(km % 10); b2[j++] = 'k'; b2[j++] = 'm';
    } else {
        long mm = dist_m;
        if (mm >= 1000) { b2[j++] = '0' + (char)(mm / 1000); }
        if (mm >= 100)  { b2[j++] = '0' + (char)((mm / 100) % 10); }
        if (mm >= 10)   { b2[j++] = '0' + (char)((mm / 10) % 10); }
        b2[j++] = '0' + (char)(mm % 10); b2[j++] = 'm';
    }
    b2[j] = 0; gfx_text(88, 32, b2, 9, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(88, 20, "DST:", 4, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(88, 44, "BRG:", 4, 1, GFX_YELLOW, GFX_BLACK);
    unsigned bb = brg % 360;
    char b3[4]; b3[0] = '0' + (char)((bb / 100) % 10); b3[1] = '0' + (char)((bb / 10) % 10); b3[2] = '0' + (char)(bb % 10); b3[3] = 0;
    gfx_text(88, 56, b3, 4, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(88, 78, "[OK] Map", 8, 1, GFX_GREY, GFX_BLACK);
}

/* ---- coordinate editor (integer microdegrees) --------------------------- */
typedef struct { int sign_lat; int lat[7]; int sign_lon; int lon[8]; int cur; } editor_t;

static void editor_init(editor_t *e, int32_t lat, int32_t lon)
{
    e->sign_lat = lat >= 0 ? 1 : -1;
    long v = lat >= 0 ? lat : -lat; v /= 100; /* micro/.1deg to 100knd -> 5 frac digits */
    long t = v;
    for (int i = 6; i >= 0; i--) { e->lat[i] = (int)(t % 10); t /= 10; }
    e->sign_lon = lon >= 0 ? 1 : -1;
    v = lon >= 0 ? lon : -lon; v /= 10;
    t = v;
    for (int i = 7; i >= 0; i--) { e->lon[i] = (int)(t % 10); t /= 10; }
    e->cur = 0;
}

static void editor_to_deg(const editor_t *e, int32_t *lat, int32_t *lon)
{
    long la = 0;
    for (int i = 0; i < 7; i++) la = la * 10 + e->lat[i];
    long lo = 0;
    for (int i = 0; i < 8; i++) lo = lo * 10 + e->lon[i];
    *lat = (int32_t)((la * 100) * e->sign_lat);
    *lon = (int32_t)((lo * 10) * e->sign_lon);
}

static int editor_key(editor_t *e, unsigned k)
{
    if (k == H9_KEY_BACK) return -1;
    if (k == H9_KEY_MENU) return 1;
    if (k == H9_KEY_UP)   { e->cur = e->cur > 0 ? e->cur - 1 : 16; }
    else if (k == H9_KEY_DOWN) { e->cur = e->cur < 16 ? e->cur + 1 : 0; }
    else if (k >= H9_KEY_1 && k <= 11) {
        int d = (int)k - (int)H9_KEY_1;
        if (e->cur == 0) e->sign_lat = (d % 2 == 0) ? 1 : -1;
        else if (e->cur <= 7) e->lat[e->cur - 1] = d;
        else if (e->cur == 8) e->sign_lon = (d % 2 == 0) ? 1 : -1;
        else e->lon[e->cur - 9] = d;
        if (e->cur < 16) e->cur++;
    }
    return 0;
}

static void editor_draw(const editor_t *e)
{
    char b[20]; char dig = 0;
    gfx_text(4, 10, "ENTER WAYPOINT", 16, 1, GFX_YELLOW, GFX_BLACK);
    /* LAT: S 44.123456 (7 digits, 5 frac) */
    int j = 0; b[j++] = e->sign_lat > 0 ? 'N' : 'S'; b[j++] = ' '; b[j++] = '0' + e->lat[0]; b[j++] = '0' + e->lat[1]; b[j++] = '.';
    for (int i = 2; i < 7; i++) b[j++] = '0' + e->lat[i]; b[j] = 0;
    gfx_text(4, 35, b, 16, 1, GFX_WHITE, GFX_BLACK);
    j = 0; b[j++] = e->sign_lon > 0 ? 'E' : 'W'; b[j++] = ' '; b[j++] = '0' + e->lon[0]; b[j++] = '0' + e->lon[1]; b[j++] = '0' + e->lon[2]; b[j++] = '.';
    for (int i = 3; i < 8; i++) b[j++] = '0' + e->lon[i]; b[j] = 0;
    gfx_text(4, 60, b, 16, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(4, 95, "[0-9] Edit", 10, 1, GFX_GREY, GFX_BLACK);
    gfx_text(4, 110, "[MENU] Start [BACK] Back", 22, 1, GFX_GREY, GFX_BLACK);
    (void)dig;
}

/* ---- module entry -------------------------------------------------------- */
unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    int32_t target_lat = 0, target_lon = 0;
    editor_t ed; int ed_ready = 0;
    typedef enum { MODE_SELECT, MODE_EDIT, MODE_COMPASS, MODE_MAP } state_t;
    state_t state = MODE_SELECT;
    t_count = 0;

    for (;;) {
        int valid = GPS_VALID;
        int32_t clat = GPS_LAT, clon = GPS_LON;
        unsigned key = h9_key();

        if (valid) track_add(clat, clon);

        gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);

        if (state == MODE_SELECT) {
            gfx_text(4, 15, "SELECT TARGET:", 15, 1, GFX_YELLOW, GFX_BLACK);
            gfx_text(4, 45, "1 Current GPS Pos", 18, 1, valid ? GFX_GREEN : GFX_GREY, GFX_BLACK);
            gfx_text(4, 65, "2 Manual Input", 18, 1, GFX_WHITE, GFX_BLACK);
            gfx_text(4, 100, valid ? "GPS: FIX OK" : "GPS: SEARCHING...", 24, 1, C_CYAN, GFX_BLACK);
            gfx_text(4, 115, "[BACK] Quit", 12, 1, GFX_GREY, GFX_BLACK);
            if (key == H9_KEY_1 && valid) { target_lat = clat; target_lon = clon; t_count = 0; track_add(clat, clon); state = MODE_COMPASS; }
            else if (key == H9_KEY_2) { editor_init(&ed, valid ? clat : 44609800, valid ? clon : 40100600); ed_ready = 1; state = MODE_EDIT; }
            else if (key == H9_KEY_BACK) break;
        } else if (state == MODE_EDIT) {
            int r = editor_key(&ed, key);
            if (r == -1) state = MODE_SELECT;
            else if (r == 1) { editor_to_deg(&ed, &target_lat, &target_lon); t_count = 0; if (valid) track_add(clat, clon); state = MODE_COMPASS; }
            else editor_draw(&ed);
        } else { /* compass or map */
            if (key == H9_KEY_BACK) state = MODE_SELECT;
            else if (key == H9_KEY_MENU) state = (state == MODE_COMPASS) ? MODE_MAP : MODE_COMPASS;
            else if (!valid) {
                gfx_text(10, 45, "WAITING FOR GPS...", 20, 1, GFX_RED, GFX_BLACK);
                gfx_text(10, 70, "[BACK] Change Target", 20, 1, GFX_GREY, GFX_BLACK);
            } else {
                long dist = nav_distance_m(clat, clon, target_lat, target_lon);
                unsigned brg = nav_bearing_deg(clat, clon, target_lat, target_lon);
                if (state == MODE_COMPASS) draw_compass(dist, brg, brg); /* arrow = bearing (no live course in RAM) */
                else draw_map(clat, clon, target_lat, target_lon, dist);
            }
        }
        gfx_text(0, 0, "", 1, 1, GFX_BLACK, GFX_BLACK);
        h9_delay(3);
    }
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    return 0;
}