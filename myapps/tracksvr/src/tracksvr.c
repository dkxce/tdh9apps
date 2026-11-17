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
 * without erase); the count byte is never touched. Max points = 255.
 *
 * Menu is drawn once and only the changed rows are redrawn on navigation
 * (no flicker), the same way myapps/tracking avoids flicker.
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
#define KEY_5     7u

#define TICK (*(const volatile uint32_t *)0x1D8B0)   /* 100 Hz */

/* Menu geometry (like trk_menu.c). */
enum { ROW_Y = 16, ROW_H = 14, ROWS = 7 };

/* The tracker's shared state lives just below the mailbox; the leading
 * fields (magic + sources + key debounce) match struct trk. TRK exists in
 * RAM always (0x2B7EC), so we use its key/key_seen for debounce even when
 * the tracker itself is not running. */
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

#define STOCK_ERASE(sec)            ((void (*)(unsigned))0x01E50000)((sec))
#define STOCK_PROGRAM(src, adr, len) ((void (*)(const void *, unsigned, unsigned))0x01E5006E)((src), (adr), (len))

struct point { char name[8]; int32_t lat, lon; };

/* ---- string helpers ------------------------------------------------------ */
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
static char *coord_str(char *o, int32_t micro)
{
    int neg = micro < 0;
    uint32_t a = (uint32_t)(neg ? -micro : micro);
    char tmp[12]; unsigned n = 0;
    while (a) { tmp[n++] = (char)('0' + a % 10); a /= 10; }
    while (n < 6) tmp[n++] = '0';
    if (neg) *o++ = '-';
    for (unsigned i = n; i--;) { if (i == 5) *o++ = '.'; *o++ = tmp[i]; }
    *o = 0;
    return o;
}

static void center(unsigned y, const char *s, unsigned scale)
{
    unsigned w = 0;
    while (s[w]) w++;
    w *= 6 * scale;
    gfx_text(w < H9_WIDTH ? (H9_WIDTH - w) / 2 : 0, y, s, 26, scale, GFX_YELLOW, GFX_BLACK);
}

/* ---- keys with debounce (mirrors trk_key: two equal reads, one report) --- */
static unsigned trk_key(void)
{
    unsigned k = h9_key();
    if (k != TRK->key) { TRK->key = (uint8_t)k; return 0; }
    if (k == TRK->key_seen) return 0;
    if (k == H9_KEY_NONE) { TRK->key_seen = H9_KEY_NONE; return 0; }
    TRK->key_seen = (uint8_t)k;
    return k;
}

static void msg(const char *s)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(4, 50, s, 26, 2, GFX_WHITE, GFX_BLACK);
    gfx_wait_release();
    for (;;) { if (trk_key() != H9_KEY_NONE) break; h9_delay(10); }
    gfx_wait_release();
}

/* ---- persistence ---------------------------------------------------------- */

static int magic_ok(const unsigned char h[HEAD])
{
    static const char m[15] = MAGIC;
    for (unsigned i = 0; i < 15; i++) if (h[i] != (unsigned char)m[i]) return 0;
    return 1;
}

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

static void header_write(void)
{
    unsigned char h[HEAD];
    static const char m[15] = MAGIC;
    for (unsigned i = 0; i < 15; i++) h[i] = (unsigned char)m[i];
    h[15] = 0xFF;
    STOCK_PROGRAM(h, SAVE_ADDR, HEAD);
}

static int point_append(const struct h9_api *api, const struct point *p)
{
    int n = saves_count(api);
    if (n < 0) { STOCK_ERASE(SECTOR_IDX); header_write(); n = 0; }
    if (n >= (int)MAX_SAVES) { msg("<NO MEMORY>"); return 0; }
    unsigned char b[REC];
    pack_rec(b, p);
    STOCK_PROGRAM(b, SAVE_ADDR + HEAD + (unsigned)n * REC, REC);
    return 1;
}

/* Replace point `idx` in place (name/lat/lon). Erase-free impossible: the
 * name/coord bytes are already written, so rewrite the whole sector. */
static void point_replace(const struct h9_api *api, int idx, int n, const struct point *p)
{
    struct point buf[MAX_SAVES];
    for (unsigned i = 0; i < (unsigned)n; i++) {
        if ((int)i == idx) buf[i] = *p; else point_get(api, i, &buf[i]);
    }
    STOCK_ERASE(SECTOR_IDX);
    header_write();
    unsigned char b[REC];
    for (unsigned j = 0; j < (unsigned)n; j++) { pack_rec(b, &buf[j]); STOCK_PROGRAM(b, SAVE_ADDR + HEAD + j * REC, REC); }
}

static void point_delete(const struct h9_api *api, unsigned del, int n)
{
    struct point buf[MAX_SAVES];
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

/* Build the string the editor shows: current buf up to pos, then a '_' cursor. */
static void edit_line(const char buf[9], unsigned pos, char out[10])
{
    for (unsigned i = 0; i < pos && i < 8; i++) out[i] = buf[i];
    out[pos] = '_';
    for (unsigned i = pos + 1; i < 9; i++) out[i] = ' ';
    out[9] = 0;
}

#define CYCLE_TICKS 50u   /* 0.5 s: same digit toggles char; after pause it adds a new one */

static int name_edit(char name[8])
{
    unsigned max = 8, pos = 0;
    char buf[9] = { 0 };
    unsigned last_digit = 0xFF, last_tick = 0;
    while (name[pos] && pos < max) { buf[pos] = name[pos]; pos++; }
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    center(4, "-- NAME --", 1);
    gfx_text(0, 100, "0-9 TAP  * / # CURSOR", 21, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(0, 112, "MENU SAVE  BACK CANCEL", 21, 1, GFX_WHITE, GFX_BLACK);
    for (;;) {
        char line[10];
        edit_line(buf, pos, line);
        gfx_fill(6, 44, 140, 24, GFX_BLACK);
        gfx_text(6, 46, line, 10, 2, GFX_GREEN, GFX_BLACK);
        unsigned k = trk_key();
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
            uint32_t now = TICK;
            unsigned n = pos, start = 0;
            if (n && digit == last_digit && now - last_tick < CYCLE_TICKS) {
                while (start < size && set[start] != buf[n - 1]) start++;
                start = (start + 1) % size;
                n--;
            } else if (n >= max) {
                continue;
            }
            for (unsigned c = 0; c < size; c++) {
                char ch = set[(start + c) % size];
                buf[n] = ch; buf[n + 1] = 0;
                pos = n + 1; last_digit = digit; last_tick = now;
                break;
            }
        }
        h9_delay(20);
    }
}

/* numeric coordinate editor (microdegrees), mirrors fixed.c edit */
static int coord_edit(const char *title, int32_t *v, unsigned ideg, char pos, char neg)
{
    uint8_t dg[8];
    unsigned n = ideg + 5, cur = 1, dirty = 1;
    int minus = *v < 0;
    uint32_t a = (uint32_t)(minus ? -*v : *v) / 10;
    for (unsigned i = n; i--;) { dg[i] = (uint8_t)(a % 10); a /= 10; }
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    center(4, title, 1);
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
        unsigned k = trk_key();
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

/* Show source coords (editable), enter name, then save. */
static void do_save(const struct h9_api *api, const char *title, int32_t lat, int32_t lon)
{
    static const char *const M[3] = { "EDIT LAT", "EDIT LON", "SAVE" };
    for (;;) {
        gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
        char b[32], *d = b;
        d = put(d, "LAT "); d = coord_str(d, lat);
        gfx_text(4, 20, b, 24, 1, GFX_WHITE, GFX_BLACK);
        d = put(b, "LON "); d = coord_str(d, lon);
        gfx_text(4, 34, b, 24, 1, GFX_WHITE, GFX_BLACK);
        center(2, title, 1);
        unsigned sel = 0, dirty = 1;
        for (;;) {                       /* 3-row choose screen */
            if (dirty) {
                gfx_fill(0, 60, H9_WIDTH, 48, GFX_BLACK);
                for (unsigned i = 0; i < 3; i++) {
                    int s = (i == sel);
                    gfx_fill(0, 60 + i * 16, H9_WIDTH, 16, s ? C_DGREY : GFX_BLACK);
                    gfx_text(4, 62 + i * 16, M[i], 12, 1, GFX_WHITE, s ? C_DGREY : GFX_BLACK);
                }
                dirty = 0;
            }
            unsigned k = trk_key();
            if (k == H9_KEY_UP) { sel = sel ? sel - 1 : 2; dirty = 1; }
            else if (k == H9_KEY_DOWN) { sel = (sel + 1) % 3; dirty = 1; }
            else if (k == H9_KEY_BACK) return;
            else if (k == H9_KEY_MENU) {
                if (sel == 0) { gfx_wait_release(); coord_edit("LAT", &lat, 2, 'N', 'S'); break; }
                else if (sel == 1) { gfx_wait_release(); coord_edit("LON", &lon, 3, 'E', 'W'); break; }
                else {
                    gfx_wait_release();
                    char name[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                    if (!name_edit(name)) return;
                    struct point p;
                    for (unsigned i = 0; i < 8; i++) p.name[i] = name[i];
                    p.lat = lat; p.lon = lon;
                    if (point_append(api, &p)) msg("<SAVED>");
                    return;
                }
            }
            h9_delay(10);
        }
    }
}

/* ---- menu drawing: redraw only changed rows (no flicker) ------------------ */
static void menu_row(unsigned i, unsigned sel, const char *labeltext)
{
    char b[22], *d = b;
    *d++ = '['; *d++ = (char)('1' + i); *d++ = ']'; *d++ = ' ';
    for (unsigned j = 0; labeltext[j] && j < 17; j++) *d++ = labeltext[j];
    *d = 0;
    int s = (i == sel);
    gfx_fill(0, ROW_Y + i * ROW_H, H9_WIDTH, ROW_H, s ? C_DGREY : GFX_BLACK);
    gfx_text(4, ROW_Y + i * ROW_H + 4, b, 26, 1, GFX_WHITE, s ? C_DGREY : GFX_BLACK);
}

static const char *main_label(unsigned i)
{
    static const char *const T[7] = {
        "Save Start Point", "Save Current Point", "Save NavTo Point",
        "Save GPS Point", "Save My Own", "Saved Points", "Clear All" };
    return T[i];
}

static void main_menu(unsigned sel)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    center(2, "-- TRACKER SAVER --", 1);
    for (unsigned i = 0; i < 7; i++) menu_row(i, sel, main_label(i));
    gfx_text(0, 116, "UP/DN MENU BACK", 16, 1, GFX_GREY, GFX_BLACK);
}

/* ---- saved points list + point view ---------------------------------------- */

static int point_view(const struct h9_api *api, int n, int idx);

static void saved_row(const struct h9_api *api, unsigned top_r, unsigned r, unsigned sel)
{
    struct point p;
    point_get(api, top_r, &p);
    char b[20], *d = b;
    *d++ = '['; d = put_num(d, top_r + 1); *d++ = ']'; *d++ = ' ';
    for (unsigned i = 0; i < 8 && p.name[i]; i++) *d++ = p.name[i];
    *d = 0;
    int s = (top_r == sel);
    gfx_fill(0, 17 + r * 12, H9_WIDTH, 12, s ? C_DGREY : GFX_BLACK);
    gfx_text(4, 18 + r * 12, b, 24, 1, GFX_WHITE, s ? C_DGREY : GFX_BLACK);
}

static void saved_points(const struct h9_api *api)
{
    int n = saves_count(api);
    if (n <= 0) { msg("<NO SAVED POINTS>"); return; }
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    center(4, "-- SAVED POINTS --", 1);
    unsigned top = 0, sel = 0, dirty = 1;
    for (;;) {
        if (dirty) {
            if (sel < top) top = sel;
            if (sel >= top + (unsigned)(n < 6 ? n : 6)) top = sel - (n < 6 ? 0 : n - 6);
            for (unsigned r = 0; r < (unsigned)(n < 8 ? n : 8); r++)
                if (top + r < (unsigned)n) saved_row(api, top + r, r, sel);
            dirty = 0;
        }
        unsigned k = trk_key();
        if (k == H9_KEY_BACK) return;
        if (k == H9_KEY_MENU) {
                if (point_view(api, n, (int)sel) == 1) { n = saves_count(api); top = 0; if (sel >= (unsigned)n) sel = 0; }
                dirty = 1;
                if (n <= 0) return;
                continue;
            }
        if (k == H9_KEY_UP || k == H9_KEY_DOWN) {
            unsigned old = sel, oldtop = top;
            if (k == H9_KEY_UP) sel = sel ? sel - 1 : (unsigned)n - 1;
            else sel = (sel + 1) % (unsigned)n;
            if (sel < top || sel >= top + 6) { dirty = 1; continue; }   /* page scroll: full */
            /* otherwise only the two changed rows */
            if (oldtop == top && old < (unsigned)n && old < top + 8)
                saved_row(api, old, old - top, sel);   /* unhighlight old */
            saved_row(api, sel, sel - top, sel);        /* highlight new */
        }
        h9_delay(10);
    }
}

static int point_view(const struct h9_api *api, int n, int idx)
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
    gfx_text(4, 84, "[5] NAVTO", 12, 1, GFX_YELLOW, GFX_BLACK);
    gfx_text(0, 96, "MENU = EDIT", 14, 1, GFX_GREY, GFX_BLACK);
    gfx_text(0, 108, "*/0/# = DELETE  BACK = LIST", 28, 1, GFX_GREY, GFX_BLACK);
    gfx_wait_release();
    for (;;) {
        unsigned k = trk_key();
        if (k == H9_KEY_BACK) return 0;
        if (k == KEY_5) {                    /* [5] NAVTO */
            if (TRK->magic != TRK_MAGIC) { msg("<NO TRACKER>"); continue; }
            TRK->tlat = p.lat; TRK->tlon = p.lon; TRK->have_target = 1;
            msg("<SET NAVTO>");
            continue;
        }
        if (k == H9_KEY_MENU) {
            char name[8];
            for (unsigned i = 0; i < 8; i++) name[i] = p.name[i];
            gfx_wait_release();
            if (name_edit(name)) {
                for (unsigned i = 0; i < 8; i++) p.name[i] = name[i];
                struct point np = p;
                if (coord_edit("LAT", &np.lat, 2, 'N', 'S') && coord_edit("LON", &np.lon, 3, 'E', 'W')) {
                    point_replace(api, idx, n, &np);
                    msg("<SAVED>");
                }
            }
            return 1;
        }
        if (k == KEY_STAR || k == KEY_0 || k == KEY_HASH) {
            point_delete(api, (unsigned)idx, n);
            msg("<DELETED>");
            return 1;
        }
        h9_delay(10);
    }
}

/* ---- main ---------------------------------------------------------------- */
unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)self;
    gfx_wait_release();          /* don't let the opening key select an item */
    unsigned sel = 0, dirty = 1;
    for (;;) {
        if (dirty) {
            main_menu(sel);
            dirty = 0;
        }
        unsigned k = trk_key();
        if (k == H9_KEY_UP || k == H9_KEY_DOWN) {
            menu_row(sel, 0xFFFFFFFF, main_label(sel));   /* unhighlight old */
            if (k == H9_KEY_UP) sel = sel ? sel - 1 : 6;
            else sel = sel + 1 < 7 ? sel + 1 : 0;
            menu_row(sel, sel, main_label(sel));          /* highlight new */
        } else if (k == H9_KEY_MENU) {
            if (sel == 6) {                              /* Clear all */
                gfx_wait_release();
                gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
                center(50, "<CLEAR ALL>", 2);
                gfx_text(0, 100, "MENU=YES BACK=NO", 16, 1, GFX_GREY, GFX_BLACK);
                for (;;) {
                    unsigned c = trk_key();
                    if (c == H9_KEY_MENU) { STOCK_ERASE(SECTOR_IDX); msg("<CLEARED>"); break; }
                    if (c == H9_KEY_BACK) { msg("<NOT CLEARED>"); break; }
                    h9_delay(10);
                }
                dirty = 1;
                continue;
            }
            if (sel == 5) { saved_points(api); dirty = 1; continue; }
            if (sel == 4) {
                char name[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                int32_t lat = 0, lon = 0;
                gfx_wait_release();
                if (!name_edit(name)) { dirty = 1; continue; }
                if (!coord_edit("LAT", &lat, 2, 'N', 'S')) { dirty = 1; continue; }
                if (!coord_edit("LON", &lon, 3, 'E', 'W')) { dirty = 1; continue; }
                struct point p;
                for (unsigned i = 0; i < 8; i++) p.name[i] = name[i];
                p.lat = lat; p.lon = lon;
                if (point_append(api, &p)) msg("<SAVED>");
                dirty = 1;
                continue;
            }
            if (TRK->magic != TRK_MAGIC) { msg("<LAUNCH TRACKER FIRST>"); dirty = 1; continue; }
            int32_t lat = TRK->lat, lon = TRK->lon;
            switch (sel) {
            case 0: lat = TRK->slat; lon = TRK->slon; if (!TRK->has_start)   { msg("<NO START>");  dirty=1; continue; } break;
            case 1: lat = TRK->lat;  lon = TRK->lon;  if (!TRK->have_fix)    { msg("<NO FIX>");    dirty=1; continue; } break;
            case 2: lat = TRK->tlat; lon = TRK->tlon; if (!TRK->have_target) { msg("<NO TARGET>"); dirty=1; continue; } break;
            case 3: lat = TRK->lat;  lon = TRK->lon;  if (!TRK->have_fix)    { msg("<NO FIX>");    dirty=1; continue; } break;
            }
            const char *t = sel == 0 ? "SAVE START" : sel == 1 ? "SAVE CURRENT"
                          : sel == 2 ? "SAVE NAVTO" : "SAVE GPS";
            do_save(api, t, lat, lon);
            dirty = 1;
        }
        else if (k == H9_KEY_BACK) return 0;
        h9_delay(10);
    }
}