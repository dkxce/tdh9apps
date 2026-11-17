/* tracksvr.c: TRACKER SAVER - user point save/load for the TD-H9.
 *
 * Stores named lat/lon points in resource flash sector 0x7FE000 (one of the
 * two free sectors at the end of the font tail).
 *
 * Sector layout (0x7FE000, 4096 bytes):
 *   [0..14]  "DKXCETRACKSAVES"  15-byte magic (always)
 *   [15]     0xFF               count byte - unused, stays 0xFF
 *   [16+i*16] point i:           name[8] + lat(int32 LE) + lon(int32 LE) = 16 B
 * The point count is found by scanning to the first 0xFF byte, so a new
 * point is appended by programming the next erased slot (NOR allows 1->0
 * without erase); the count byte is never touched. Max points =
 * (4096-16)/16 = 255.
 *
 * Menu:
 *   -- TRACKER SAVER --
 *   [1] Save Start Point   [2] Save Current Point   [3] Save NavTo Point
 *   [4] Save GPS Point     [5] Save My Own          [6] Saved Points
 * Operations that need struct trk (1..4) show "<LAUNCH TRACKER FIRST>" if
 * the tracker state is not present; Saved Points works standalone.
 *
 * Freestanding: no globals/.bss; the delete path uses one stack buffer.
 */
#include <stdint.h>

#include "h9mod.h"
#include "gfx.h"

#define SAVE_ADDR   0x7FE000u
#define SAVE_SIZE   0x1000u
#define MAGIC       "DKXCETRACKSAVES"        /* 15 characters */
#define HEAD        16u                       /* 15 magic + 1 count(0xFF) */
#define REC         16u                       /* name[8] + lat[4] + lon[4] */
#define MAX_SAVES   ((SAVE_SIZE - HEAD) / REC)  /* 255 */
#define SECTOR_IDX  (SAVE_ADDR / SAVE_SIZE)     /* 0x7fe */

#define C_DGREY   0x4208u
#define KEY_STAR  0x13u
#define KEY_HASH  0x14u
#define KEY_0     2u

/* The tracker's shared state lives just below the mailbox; this app only
 * reads the leading fields (magic + point sources). Header matches the
 * first bytes of struct trk in myapps/tracking/src/trk.h. */
struct trk_hdr {
    uint32_t magic;
    uint8_t cmd, frame_len, escape, nmea_len;
    char nmea[84];
    int32_t lat, lon, alt10;
    uint16_t course, spd10, sats;
    char utc[6];
    uint8_t have_fix, fresh, key, key_seen;
    uint32_t fix_tick;
    int32_t tlat, tlon;
    uint8_t have_target, rec, has_start, screen;
    int32_t slat, slon;
};
#define TRK         ((volatile struct trk_hdr *)0x2B7ECu)   /* H9_SHARED_TOP - STATE_BYTES */
#define TRK_MAGIC   0x324B5254u       /* "TRK2" */

/* Stock SPI2 NOR primitives (see passlock.c): 4 KiB sector erase by chip
 * sector index, and page program by chip address. */
#define STOCK_ERASE(sec)            ((void (*)(unsigned))0x01E50000)((sec))
#define STOCK_PROGRAM(src, adr, len) ((void (*)(const void *, unsigned, unsigned))0x01E5006E)((src), (adr), (len))

struct point { char name[8]; int32_t lat, lon; };

/* ---- tiny string helpers ------------------------------------------------ */
static char *put(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char *put_num(char *o, unsigned v)
{
    char t[8]; unsigned n = 0;
    if (!v) { *o++ = '0'; *o = 0; return o; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n--) *o++ = t[n];
    *o = 0;
    return o;
}

/* render microdegrees as "-55.75123"; returns end pointer */
static char *coord_str(char *o, int32_t micro)
{
    int neg = micro < 0;
    uint32_t a = (uint32_t)(neg ? -micro : micro);
    char tmp[12]; unsigned n = 0;
    while (a) { tmp[n++] = (char)('0' + a % 10); a /= 10; }
    while (n < 6) tmp[n++] = '0';          /* at least 6 digits */
    if (neg) *o++ = '-';
    for (unsigned i = n; i--;) { if (i == 5) *o++ = '.'; *o++ = tmp[i]; }
    *o = 0;
    return o;
}

/* ---- simple key with debounce (two equal reads) -------------------------- */
static unsigned key(void)
{
    unsigned a = h9_key(), b = h9_key();
    return a == b ? a : H9_KEY_NONE;
}

static void msg(const char *s)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(4, 50, s, 26, 2, GFX_WHITE, GFX_BLACK);
    gfx_wait_release();
    for (;;) { if (key() != H9_KEY_NONE) break; h9_delay(10); }
    gfx_wait_release();
}

/* ---- persistence --------------------------------------------------------- */

static int magic_ok(const unsigned char h[HEAD])
{
    static const char m[15] = MAGIC;
    for (unsigned i = 0; i < 15; i++) if (h[i] != (unsigned char)m[i]) return 0;
    return 1;
}

/* number of stored points, -1 if no valid magic. */
static int saves_count(const struct h9_api *api)
{
    unsigned char h[HEAD];
    api->read(h, SAVE_ADDR, HEAD);
    if (!magic_ok(h)) return -1;
    unsigned char r[REC];
    for (unsigned i = 0; i < MAX_SAVES; i++) {
        api->read(r, SAVE_ADDR + HEAD + i * REC, REC);
        if (r[0] == 0xFF) return (int)i;
    }
    return (int)MAX_SAVES;
}

static void point_get(const struct h9_api *api, unsigned i, struct point *p)
{
    unsigned char r[REC];
    api->read(r, SAVE_ADDR + HEAD + i * REC, REC);
    for (unsigned k = 0; k < 8; k++) p->name[k] = (char)r[k];
    p->lat = (int32_t)(uint32_t)(r[8] | (r[9] << 8) | (r[10] << 16) | ((uint32_t)r[11] << 24));
    p->lon = (int32_t)(uint32_t)(r[12] | (r[13] << 8) | (r[14] << 16) | ((uint32_t)r[15] << 24));
}

static void pack_rec(unsigned char b[REC], const struct point *p)
{
    for (unsigned k = 0; k < 8; k++) b[k] = (unsigned char)p->name[k];
    int32_t lat = p->lat, lon = p->lon;
    b[8]  = (unsigned char)(lat       ); b[9]  = (unsigned char)(lat >> 8);
    b[10] = (unsigned char)(lat >> 16); b[11] = (unsigned char)(lat >> 24);
    b[12] = (unsigned char)(lon       ); b[13] = (unsigned char)(lon >> 8);
    b[14] = (unsigned char)(lon >> 16); b[15] = (unsigned char)(lon >> 24);
}

/* write header (magic + count=0xFF). Only used once when the sector is empty. */
static void header_write(void)
{
    unsigned char h[HEAD];
    static const char m[15] = MAGIC;
    for (unsigned i = 0; i < 15; i++) h[i] = (unsigned char)m[i];
    h[15] = 0xFF;
    STOCK_PROGRAM(h, SAVE_ADDR, HEAD);
}

/* append one point without erasing the sector. Returns 1 on success. */
static int point_append(const struct h9_api *api, const struct point *p)
{
    int n = saves_count(api);
    if (n < 0) {
        STOCK_ERASE(SECTOR_IDX);
        header_write();
        n = 0;
    }
    if (n >= (int)MAX_SAVES) { msg("<NO MEMORY>"); return 0; }
    unsigned char b[REC];
    pack_rec(b, p);
    STOCK_PROGRAM(b, SAVE_ADDR + HEAD + (unsigned)n * REC, REC);
    return 1;
}

/* delete point `del` (shift left), erase + rewrite whole sector. */
static void point_delete(const struct h9_api *api, unsigned del, int n)
{
    struct point buf[MAX_SAVES];    /* stack buffer: up to 255*16 = 4080 B */
    unsigned m = 0;
    for (unsigned i = 0; i < (unsigned)n; i++) if (i != del) point_get(api, i, &buf[m++]);
    STOCK_ERASE(SECTOR_IDX);
    header_write();
    unsigned char b[REC];
    for (unsigned j = 0; j < m; j++) { pack_rec(b, &buf[j]); STOCK_PROGRAM(b, SAVE_ADDR + HEAD + j * REC, REC); }
}

/* ---- multi-tap name entry (max 8), returns 1 if confirmed ---------------- */
static const char *const KEYS[10] = {
    " 0", ".,?!-/@1", "ABC2", "DEF3", "GHI4", "JKL5", "MNO6", "PQRS7", "TUV8", "WXYZ9" };

static int name_edit(char name[8])
{
    unsigned max = 8, pos = 0;
    char buf[9] = { 0 };
    unsigned last_digit = 0xFF;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(0, 4, "-- NAME --", 10, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(0, 100, "0-9 TAP  * / # CURSOR", 21, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(0, 112, "MENU SAVE  BACK CANCEL", 21, 1, GFX_WHITE, GFX_BLACK);
    for (;;) {
        gfx_fill(0, 44, H9_WIDTH, 24, GFX_BLACK);
        gfx_text(6, 46, buf[pos] ? buf : " ", 10, 2, GFX_GREEN, GFX_BLACK);
        gfx_fill(6 + pos * 12, 68, 12, 2, GFX_WHITE);
        unsigned k = key();
        if (k == H9_KEY_BACK) return 0;
        if (k == H9_KEY_MENU) {
            for (unsigned i = 0; i < 8; i++) name[i] = i < pos ? buf[i] : 0;
            return pos > 0;
        }
        if (k == KEY_STAR) { if (pos) pos--; last_digit = 0xFF; continue; }
        if (k == KEY_HASH) { if (pos < max) pos++; last_digit = 0xFF; continue; }
        if (k >= 2 && k <= 11) {
            unsigned digit = k == 2 ? 0 : k - 2;
            const char *set = KEYS[digit];
            unsigned size = 0; while (set[size]) size++;
            unsigned n = pos, start = 0;
            if (n && digit == last_digit) {           /* same digit: cycle char */
                while (start < size && set[start] != buf[n - 1]) start++;
                start = (start + 1) % size;
                n--;
            } else if (n >= max) {
                continue;
            }
            for (unsigned c = 0; c < size; c++) {
                char ch = set[(start + c) % size];
                if (ch == ' ') continue;              /* skip space */
                buf[n] = ch; buf[n + 1] = 0;
                pos = n + 1; last_digit = digit;
                break;
            }
        }
        h9_delay(20);
    }
}

/* numeric coordinate editor (microdegrees), adapted from fixed.c */
static int coord_edit(const char *title, int32_t *v, unsigned ideg, char pos, char neg)
{
    uint8_t dg[8];
    unsigned n = ideg + 5, cur = 1, dirty = 1;
    int minus = *v < 0;
    uint32_t a = (uint32_t)(minus ? -*v : *v) / 10;
    for (unsigned i = n; i--;) { dg[i] = (uint8_t)(a % 10); a /= 10; }
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(0, 4, title, 14, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(0, 100, "0-9 DIGIT  UP/DN MOVE", 21, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(0, 112, "MENU OK  BACK CANCEL", 20, 1, GFX_WHITE, GFX_BLACK);
    for (;;) {
        if (dirty) {
            char b[16];
            unsigned j = 0, col;
            b[j++] = minus ? neg : pos;
            b[j++] = ' ';
            for (unsigned i = 0; i < n; i++) {
                if (i == ideg) b[j++] = '.';
                b[j++] = (char)('0' + dg[i]);
            }
            b[j] = 0;
            gfx_fill(8, 40, 120, 24, GFX_BLACK);
            gfx_text(8, 44, b, 12, 2, GFX_WHITE, GFX_BLACK);
            gfx_fill(8, 62, 11 * 12, 2, GFX_BLACK);
            col = cur == 0 ? 0 : cur <= ideg ? cur + 1 : cur + 2;
            gfx_fill(8 + col * 12, 62, 12, 2, GFX_WHITE);
            dirty = 0;
        }
        unsigned k = key();
        if (k == H9_KEY_BACK) return 0;
        if (k == H9_KEY_MENU) {
            int32_t r = 0;
            for (unsigned i = 0; i < n; i++) r = r * 10 + dg[i];
            if (r > (int32_t)(ideg == 2 ? 9000000 : 18000000)) r = ideg == 2 ? 9000000 : 18000000;
            *v = (minus ? -r : r) * 10;
            return 1;
        }
        if (k == H9_KEY_UP) { cur = cur ? cur - 1 : n; dirty = 1; }
        else if (k == H9_KEY_DOWN) { cur = cur < n ? cur + 1 : 0; dirty = 1; }
        else if (k >= 2 && k <= 11) {
            if (cur == 0) minus = !minus;
            else dg[cur - 1] = (uint8_t)(k - 2);
            if (cur < n) cur++;
            dirty = 1;
        }
        h9_delay(10);
    }
}

/* show coords of a source, then name-edit and append. */
static void do_save(const struct h9_api *api, const char *title, int32_t lat, int32_t lon)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    char b[32], *d = b;
    d = put(d, "LAT "); d = coord_str(d, lat);
    gfx_text(4, 20, b, 24, 1, GFX_WHITE, GFX_BLACK);
    d = put(b, "LON "); d = coord_str(d, lon);
    gfx_text(4, 34, b, 24, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(0, 2, title, 16, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(0, 80, "ENTER NAME", 12, 1, GFX_GREY, GFX_BLACK);
    gfx_wait_release();
    char name[8];
    if (!name_edit(name)) return;
    struct point p;
    for (unsigned i = 0; i < 8; i++) p.name[i] = name[i];
    p.lat = lat; p.lon = lon;
    if (point_append(api, &p)) msg("<SAVED>");
}

/* ---- point view: set NavTo (tracker) or delete --------------------------- */
static void point_view(const struct h9_api *api, int n, int idx)
{
    struct point p;
    point_get(api, (unsigned)idx, &p);
    char b[32], *d;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    d = put(b, "NUM "); put_num(d, idx + 1);
    gfx_text(4, 8, b, 24, 1, GFX_YELLOW, GFX_BLACK);
    d = put(b, "NAME "); for (unsigned i = 0; i < 8 && p.name[i]; i++) *d++ = p.name[i]; *d = 0;
    gfx_text(4, 22, b, 24, 1, GFX_WHITE, GFX_BLACK);
    d = put(b, "LAT "); coord_str(d, p.lat);
    gfx_text(4, 36, b, 24, 1, GFX_WHITE, GFX_BLACK);
    d = put(b, "LON "); coord_str(d, p.lon);
    gfx_text(4, 50, b, 24, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(0, 90, "MENU = SET NAVTO", 18, 1, GFX_GREY, GFX_BLACK);
    gfx_text(0, 103, "*/0/# = DELETE", 16, 1, GFX_GREY, GFX_BLACK);
    gfx_text(0, 116, "BACK = LIST", 14, 1, GFX_GREY, GFX_BLACK);
    gfx_wait_release();
    for (;;) {
        unsigned k = key();
        if (k == H9_KEY_BACK) return;
        if (k == H9_KEY_MENU) {
            if (TRK->magic != TRK_MAGIC) { msg("<NO TRACKER>"); return; }
            TRK->tlat = p.lat; TRK->tlon = p.lon; TRK->have_target = 1;
            msg("<SET NAVTO>");
            return;
        }
        if (k == KEY_STAR || k == KEY_0 || k == KEY_HASH) {
            point_delete(api, (unsigned)idx, n);
            msg("<DELETED>");
            return;
        }
        h9_delay(10);
    }
}

/* ---- saved points list ---------------------------------------------------- */
static void saved_points(const struct h9_api *api)
{
    int n = saves_count(api);
    if (n <= 0) { msg("<NO SAVED POINTS>"); return; }
    unsigned top = 0, sel = 0;
    for (;;) {
        gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
        gfx_text(0, 4, "-- SAVED POINTS --", 18, 1, GFX_YELLOW, GFX_BLACK);
        if (sel < top) top = sel;
        if (sel >= top + 8) top = sel - 8 + 1;
        for (unsigned r = 0; r < 8 && top + r < (unsigned)n; r++) {
            struct point p;
            point_get(api, top + r, &p);
            char b[20], *d = b;
            *d++ = '['; d = put_num(d, top + r + 1); *d++ = ']'; *d++ = ' ';
            for (unsigned i = 0; i < 8 && p.name[i]; i++) *d++ = p.name[i];
            *d = 0;
            int selr = (top + r == sel);
            gfx_fill(0, 17 + r * 12, H9_WIDTH, 12, selr ? C_DGREY : GFX_BLACK);
            gfx_text(4, 18 + r * 12, b, 24, 1, GFX_WHITE, selr ? C_DGREY : GFX_BLACK);
        }
        gfx_text(0, 116, "MENU=VIEW BACK=EXIT", 20, 1, GFX_GREY, GFX_BLACK);
        unsigned k = key();
        if (k == H9_KEY_BACK) return;
        if (k == H9_KEY_MENU) { point_view(api, n, (int)sel); return; }
        if (k == H9_KEY_UP) { sel = sel ? sel - 1 : (unsigned)n - 1; }
        else if (k == H9_KEY_DOWN) { sel = (sel + 1) % (unsigned)n; }
        h9_delay(10);
    }
}

/* ---- main menu ------------------------------------------------------------ */
static void draw_main(int sel)
{
    static const char *const T[6] = {
        "Save Start Point", "Save Current Point", "Save NavTo Point",
        "Save GPS Point", "Save My Own", "Saved Points" };
    char b[20];
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(0, 4, "-- TRACKER SAVER --", 19, 1, GFX_YELLOW, GFX_BLACK);
    for (unsigned i = 0; i < 6; i++) {
        char *d = b;
        *d++ = '['; *d++ = (char)('1' + i); *d++ = ']'; *d++ = ' ';
        for (unsigned j = 0; T[i][j]; j++) *d++ = T[i][j];
        *d = 0;
        int selr = (i == (unsigned)sel);
        gfx_fill(0, 16 + i * 16, H9_WIDTH, 16, selr ? C_DGREY : GFX_BLACK);
        gfx_text(4, 18 + i * 16, b, 26, 1, GFX_WHITE, selr ? C_DGREY : GFX_BLACK);
    }
    gfx_text(0, 114, "UP/DN MENU BACK", 16, 1, GFX_GREY, GFX_BLACK);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)self;
    unsigned sel = 0;
    for (;;) {
        draw_main(sel);
        unsigned k = key();
        if (k == H9_KEY_UP) { sel = sel ? sel - 1 : 5; }
        else if (k == H9_KEY_DOWN) { sel = sel + 1 < 6 ? sel + 1 : 0; }
        else if (k == H9_KEY_MENU) {
            if (sel == 5) { saved_points(api); continue; }
            if (sel == 4) {                /* Save My Own: name + lat + lon, no tracker needed */
                char name[8];
                int32_t lat = 0, lon = 0;
                gfx_wait_release();
                if (!name_edit(name)) continue;
                if (!coord_edit("LAT", &lat, 2, 'N', 'S')) continue;
                if (!coord_edit("LON", &lon, 3, 'E', 'W')) continue;
                struct point p;
                for (unsigned i = 0; i < 8; i++) p.name[i] = name[i];
                p.lat = lat; p.lon = lon;
                if (point_append(api, &p)) msg("<SAVED>");
                continue;
            }
            if (TRK->magic != TRK_MAGIC) { msg("<LAUNCH TRACKER FIRST>"); continue; }
            int32_t lat = TRK->lat, lon = TRK->lon;
            switch (sel) {
            case 0: lat = TRK->slat; lon = TRK->slon; if (!TRK->has_start) { msg("<NO START>"); continue; } break;
            case 1: lat = TRK->lat;  lon = TRK->lon;  if (!TRK->have_fix)   { msg("<NO FIX>");  continue; } break;
            case 2: lat = TRK->tlat; lon = TRK->tlon; if (!TRK->have_target){ msg("<NO TARGET>");continue; } break;
            case 3: lat = TRK->lat;  lon = TRK->lon;  if (!TRK->have_fix)   { msg("<NO FIX>");  continue; } break;
            }
            const char *t = sel == 0 ? "SAVE START" : sel == 1 ? "SAVE CURRENT"
                          : sel == 2 ? "SAVE NAVTO" : "SAVE GPS";
            do_save(api, t, lat, lon);
        }
        else if (k == H9_KEY_BACK) return 0;
        h9_delay(10);
    }
}