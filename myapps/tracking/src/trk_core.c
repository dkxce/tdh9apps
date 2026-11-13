/* TRACKER core, linked into every tracker module: state, NMEA, navigation
 * math, track recording, keys, text. Track editing and point queries are in
 * trk_track.c, linked only where used. */
#include "trk.h"

#define RING ((volatile uint8_t *)0x16634)
#define RING_WRITE (*(volatile uint16_t *)0x16E34)
#define RING_READ (*(volatile uint16_t *)0x16E36)
#define RING_MASK 2047u

_Static_assert(sizeof(struct trk) <= STATE_BYTES, "state must fit below the request mailbox");

const uint16_t STEP_M[16] = { 10, 25, 50, 75, 100, 150, 200, 250, 300, 350, 400, 450, 500, 1000, 2500, 5000 };

/* ---- text ------------------------------------------------------------------ */

char *put(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

char *num(char *d, uint32_t v, unsigned width)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n < width) t[n++] = '0';
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}

char *fixed(char *d, int32_t v, unsigned frac)
{
    uint32_t m = (uint32_t)(v < 0 ? -v : v), scale = 1;
    for (unsigned i = 0; i < frac; i++) scale *= 10;
    if (v < 0) *d++ = '-';
    d = num(d, m / scale, 1);
    if (frac) { *d++ = '.'; d = num(d, m % scale, frac); }
    return d;
}

char *km_text(char *d, uint32_t m)
{
    if (m < 10000) return fixed(d, (int32_t)m, 3);
    if (m < 100000) return fixed(d, (int32_t)(m / 10), 2);
    if (m < 1000000) return fixed(d, (int32_t)(m / 100), 1);
    return num(d, m / 1000, 1);
}

char *dist_label(char *d, uint32_t m)
{
    if (m < 1000) { d = num(d, m, 1); return put(d, "M"); }
    d = num(d, m / 1000, 1);
    if (m % 1000) { *d++ = '.'; d = num(d, m % 1000 / 100, 1); }
    return put(d, "KM");
}

/* ---- trigonometry (Q10, gps.c table) and navigation ---------------------------- */

int sin_deg(unsigned a)
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
int cos_deg(unsigned a) { return sin_deg(a + 90); }
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

/* East / north offset in metres, tangent plane. 0.11132 m per microdegree
 * is 116726 in Q20; 32 x 32 -> 64 multiplies as in fix_store.h. */
void nav_delta(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2, int32_t *dx, int32_t *dy)
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

uint32_t nav_distance(int32_t dx, int32_t dy)
{
    uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx), ay = (uint32_t)(dy < 0 ? -dy : dy);
    if (ax < 32768 && ay < 32768) return isqrt(ax * ax + ay * ay);
    ax /= 1000;
    ay /= 1000;
    return isqrt(ax * ax + ay * ay) * 1000;
}

/* Compass bearing (0 = north, clockwise) of the offset dx east, dy north. */
unsigned nav_bearing(int32_t dx, int32_t dy)
{
    uint32_t ax = (uint32_t)(dx < 0 ? -dx : dx), ay = (uint32_t)(dy < 0 ? -dy : dy);
    if (!ax && !ay) return 0;
    unsigned a = ax <= ay ? atan_oct(ax, ay) : 90 - atan_oct(ay, ax);
    if (dy >= 0) return dx >= 0 ? a : (360 - a) % 360;
    return dx >= 0 ? 180 - a : 180 + a;
}

uint32_t dist_between(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2)
{
    int32_t dx, dy;
    nav_delta(lat1, lon1, lat2, lon2, &dx, &dy);
    return nav_distance(dx, dy);
}

/* ---- track ------------------------------------------------------------------ */

void trk_drop_oldest(struct trk *s)
{
    if (s->count > 1) s->base_dist += dist_between(s->p_lat[0], s->p_lon[0], s->p_lat[1], s->p_lon[1]);
    for (unsigned i = 1; i < s->count; i++) {
        s->p_lat[i - 1] = s->p_lat[i];
        s->p_lon[i - 1] = s->p_lon[i];
        s->p_t[i - 1] = s->p_t[i];
    }
    s->count--;
    if (s->show >= 0) s->show--;
}

/* UTC second of day from the last RMC / GGA time, -1 unknown */
int32_t trk_utc_now(const struct trk *s)
{
    if (!s->utc[0]) return -1;
    return (int32_t)(((s->utc[0] - '0') * 10 + (s->utc[1] - '0')) * 3600 +
                     ((s->utc[2] - '0') * 10 + (s->utc[3] - '0')) * 60 + (s->utc[4] - '0') * 10 + (s->utc[5] - '0'));
}

void trk_track_add(struct trk *s)
{
    uint32_t sec = (TICK - s->start_tick) / 100u;
    if (s->start_utc < 0 && s->utc[0]) s->start_utc = (trk_utc_now(s) - (int32_t)(sec % 86400u) + 86400) % 86400;
    if (!s->has_start) { s->slat = s->lat; s->slon = s->lon; s->has_start = 1; }
    if (s->count) {
        unsigned n = s->count - 1u;
        if (s->step_mode == STEP_TIME) {
            if (sec < s->p_t[n] * 2u + s->step_time) return;
        } else if (dist_between(s->p_lat[n], s->p_lon[n], s->lat, s->lon) < STEP_M[s->step]) return;
    } else {
        s->base_dist = dist_between(s->slat, s->slon, s->lat, s->lon);
    }
    while (s->count >= s->cap) trk_drop_oldest(s);
    sec /= 2;
    s->p_lat[s->count] = s->lat;
    s->p_lon[s->count] = s->lon;
    s->p_t[s->count] = (uint16_t)(sec > 0xFFFF ? 0xFFFF : sec);
    s->count++;
}

void trk_start(void)
{
    struct trk *s = TRK;
    s->count = 0;
    s->base_dist = 0;
    s->has_start = 0;
    s->show = -1;
    s->cur = 0;
    s->start_tick = TICK;
    s->start_utc = trk_utc_now(s);
    s->rec = REC_ON;
    if (trk_live()) trk_track_add(s);
}

/* ---- NMEA (gps.c-derived: RMC and GGA) ---------------------------------------- */

static unsigned dig(char c) { return c >= '0' && c <= '9' ? (unsigned)(c - '0') : 0; }

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

static void got_fix(struct trk *s, int32_t lat, int32_t lon)
{
    s->lat = lat;
    s->lon = lon;
    s->fix_tick = TICK;
    s->have_fix = 1;
    s->fresh = 1;
}

static void sentence(struct trk *s)
{
    char *p = s->nmea, *f[16];
    unsigned n = 0;
    int32_t la, lo;
    if (s->nmea_len < 6 || p[0] != '$') return;
    for (unsigned i = 0; i < s->nmea_len && n < 16; i++) {
        if (p[i] == '*') { p[i] = 0; break; }
        if (p[i] == ',') { p[i] = 0; f[n++] = p + i + 1; }
    }
    int rmc = p[3] == 'R' && p[4] == 'M' && p[5] == 'C' && n >= 8;
    int gga = p[3] == 'G' && p[4] == 'G' && p[5] == 'A' && n >= 9;
    if (rmc || gga) {
        unsigned k = 0;
        while (k < 6 && f[0][k]) k++;
        if (k == 6)
            for (k = 0; k < 6; k++) s->utc[k] = f[0][k];
    }
    if (rmc) {
        if (f[1][0] != 'A') return;
        if (!coord(f[2], 2, f[3][0], &la) || !coord(f[4], 3, f[5][0], &lo)) return;
        s->spd10 = (unsigned)val(f[6], 1);
        s->course = (unsigned)val(f[7], 0) % 360;
        got_fix(s, la, lo);
    } else if (gga) {
        s->sats = (unsigned)val(f[6], 0);
        if (!val(f[5], 0)) return;
        s->alt10 = val(f[8], 1);
        if (coord(f[1], 2, f[2][0], &la) && coord(f[3], 3, f[4][0], &lo)) got_fix(s, la, lo);
    }
}

static void nmea_byte(struct trk *s, char c)
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
static void poll_ring(struct trk *s)
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

/* ---- module plumbing -------------------------------------------------------------- */

struct trk *trk_open(void)
{
    struct trk *s = TRK;
    uint32_t end = (uint32_t)&trk_end + sizeof trk_end;
    if (end > (uint32_t)s) {
        char b[24];
        num(put(b, "MODULE TOO BIG +"), end - (uint32_t)s, 1);
        gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
        gfx_text(4, 50, b, 22, 1, GFX_RED, GFX_BLACK);
        gfx_wait_key();
        return 0;
    }
    if (s->magic != TRK_MAGIC) {   /* only TRACKER sets the state up (tracker.c) */
        trk_run("TRACKER");
        return 0;
    }
    s->key = s->key_seen = h9_key();
    s->view_w = VIEW_W;
    return s;
}

void trk_service(void)
{
    struct trk *s = TRK;
    poll_ring(s);
    if (s->fresh) {
        s->fresh = 0;
        if (s->rec == REC_ON) trk_track_add(s);
    }
}

int trk_live(void)
{
    struct trk *s = TRK;
    return s->have_fix && TICK - s->fix_tick < FIX_TIMEOUT;
}

int trk_heading_ok(void)
{
    struct trk *s = TRK;
    return trk_live() && (!s->hdg_filter || s->spd10 >= 11);   /* 1.1 knots, about 2 km/h */
}

/* A key once two readings agree (the keypad ADC has no debounce). */
unsigned trk_key(void)
{
    struct trk *s = TRK;
    unsigned k = h9_key();
    if (k != s->key) { s->key = k; return 0; }
    if (k == s->key_seen) return 0;
    s->key_seen = k;
    return k;
}

/* Bundle names are space-padded to 16 by build_one.py; the menu compares all 16. */
void trk_run(const char *name)
{
    volatile struct h9_request *rq = H9_REQUEST;
    unsigned i = 0;
    for (; i < H9_NAME_BYTES && name[i]; i++) rq->name[i] = name[i];
    for (; i < H9_NAME_BYTES; i++) rq->name[i] = ' ';
    rq->magic = H9_REQUEST_MAGIC;
}
