/* LOCK RESEARCH: finds the RAM byte that holds the stock keypad lock.
 *
 * A 2 KiB RAM window is compared across module runs:
 *   1  keypad unlocked now: snapshot the window, 2 s of re-reads mark the
 *      bytes that change on their own (counters) as noisy;
 *   2  keypad locked now: the first time, candidates are the quiet bytes
 *      that differ from the snapshot; later, keep those equal to that value;
 *   3  keypad unlocked now: keep the candidates equal to the snapshot again.
 * Each step re-reads for 2 s and drops what changes meanwhile. Between steps
 * leave to the radio, toggle the lock, come back with # held.
 *
 * The research lives just below satshare (0x2B880), above the menu's body,
 * and survives the radio and the menu; any other module run in between can
 * overwrite it (magic check: start again with 1).
 * Keys: 1 / 2 / 3 steps, * / # window for the next 1, Up / Down page the
 * candidates, Back exit. */
#include "h9mod.h"
#include "gfx.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)   /* 100 Hz */

enum { WIN = 0x800, MAXC = 240, SAMPLE = 200, ROWS = 8, ROW_Y = 52 };
enum { BASE_MIN = 0xC000, BASE_MAX = 0x29000, SAT_TOP = 0x2B880, LR_MAGIC = 0x31524B4C /* "LKR1" */ };
enum { PH_NONE, PH_A, PH_B };   /* nothing / snapshot taken / candidates picked */
enum { KEY_1 = 3, KEY_2 = 4, KEY_3 = 5, KEY_STAR = 0x13, KEY_HASH = 0x14 };
enum { C_DGREY = 0x4208 };

struct lr {
    uint32_t magic, base, next;   /* base: window of this research, next: for the next 1 */
    uint16_t ncand, lost;         /* lost: differences beyond MAXC at the first 2 */
    uint8_t phase, steps, spare[2];
    uint16_t off[MAXC];           /* candidate offsets in the window */
    uint8_t b[MAXC];              /* candidate value while locked */
    uint8_t noisy[WIN / 8];
    uint8_t a[WIN];               /* snapshot while unlocked */
};

#define LR ((struct lr *)((SAT_TOP - sizeof(struct lr)) & ~3u))

extern const uint32_t lr_end;

_Static_assert(((SAT_TOP - sizeof(struct lr)) & ~3u) >= 0x2AC00, "keep clear of the menu body");

static char *put(char *d, const char *s) { while (*s) *d++ = *s++; *d = 0; return d; }

static char *hex(char *d, uint32_t v, unsigned n)
{
    for (unsigned i = n; i--;) { d[i] = "0123456789ABCDEF"[v & 15]; v >>= 4; }
    d[n] = 0;
    return d + n;
}

static char *dec(char *d, unsigned v)
{
    char t[6];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 6);
    while (n) *d++ = t[--n];
    *d = 0;
    return d;
}

static void line(unsigned y, const char *s, uint16_t c)
{
    gfx_fill(0, y, H9_WIDTH, 9, GFX_BLACK);
    gfx_text(2, y, s, 26, 1, c, GFX_BLACK);
}

static int noisy(const struct lr *r, unsigned i) { return r->noisy[i >> 3] >> (i & 7) & 1; }

static const volatile uint8_t *win(const struct lr *r) { return (const volatile uint8_t *)r->base; }

/* 1: snapshot, then mark the bytes that move within SAMPLE ticks */
static void step_new(struct lr *r)
{
    const volatile uint8_t *m;
    r->base = r->next;
    m = win(r);
    r->ncand = r->lost = 0;
    for (unsigned i = 0; i < WIN; i++) r->a[i] = m[i];
    for (unsigned i = 0; i < WIN / 8; i++) r->noisy[i] = 0;
    uint32_t t0 = TICK;
    do {
        for (unsigned i = 0; i < WIN; i++)
            if (m[i] != r->a[i]) r->noisy[i >> 3] |= (uint8_t)(1u << (i & 7));
        h9_delay(10);
    } while (TICK - t0 < SAMPLE);
    r->phase = PH_A;
    r->steps = 1;
}

/* Keep the candidates that read as expected (locked: b, unlocked: a) for SAMPLE ticks. */
static void keep(struct lr *r, int locked)
{
    const volatile uint8_t *m = win(r);
    uint32_t t0 = TICK;
    do {
        unsigned n = 0;
        for (unsigned k = 0; k < r->ncand; k++) {
            unsigned i = r->off[k];
            if (m[i] != (locked ? r->b[k] : r->a[i])) continue;
            r->off[n] = (uint16_t)i;
            r->b[n] = r->b[k];
            n++;
        }
        r->ncand = (uint16_t)n;
        h9_delay(10);
    } while (TICK - t0 < SAMPLE);
}

static void step_locked(struct lr *r)
{
    if (r->phase == PH_A) {
        const volatile uint8_t *m = win(r);
        unsigned n = 0;
        r->lost = 0;
        for (unsigned i = 0; i < WIN; i++) {
            uint8_t v = m[i];
            if (noisy(r, i) || v == r->a[i]) continue;
            if (n < MAXC) { r->off[n] = (uint16_t)i; r->b[n] = v; n++; }
            else r->lost++;
        }
        r->ncand = (uint16_t)n;
        r->phase = PH_B;
    }
    keep(r, 1);
}

/* Before any 2 an unlocked pass only widens the noise mask. */
static void step_unlocked(struct lr *r)
{
    if (r->phase == PH_B) { keep(r, 0); return; }
    const volatile uint8_t *m = win(r);
    uint32_t t0 = TICK;
    do {
        for (unsigned i = 0; i < WIN; i++)
            if (m[i] != r->a[i]) r->noisy[i >> 3] |= (uint8_t)(1u << (i & 7));
        h9_delay(10);
    } while (TICK - t0 < SAMPLE);
}

static void draw(const struct lr *r, unsigned top)
{
    static const char *const PH[3] = { "NO DATA, PRESS 1", "UNLOCKED SNAP", "CANDIDATES" };
    char b[28], *d;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(2, 2, "LOCK RESEARCH", 13, 1, GFX_YELLOW, GFX_BLACK);
    d = put(b, "WIN ");
    d = hex(d, r->base, 5);
    d = put(d, " NEXT ");
    hex(d, r->next, 5);
    line(12, b, GFX_WHITE);
    d = put(b, "STEP ");
    d = dec(d, r->steps);
    d = put(d, " ");
    put(d, PH[r->phase]);
    line(22, b, GFX_WHITE);
    line(32, "1 NEW 2 LOCKED 3 UNLOCKED", GFX_GREY);
    if (r->phase != PH_B) {
        line(42, "*/# WIN  BACK EXIT", GFX_GREY);
        return;
    }
    d = put(b, "CAND ");
    d = dec(d, r->ncand);
    if (r->lost) { d = put(d, " LOST "); dec(d, r->lost); }
    line(42, b, r->lost ? GFX_RED : GFX_GREEN);
    gfx_fill(0, ROW_Y - 2, H9_WIDTH, 1, C_DGREY);
    for (unsigned k = top, y = ROW_Y; k < r->ncand && y < ROW_Y + ROWS * 9; k++, y += 9) {
        unsigned i = r->off[k];
        d = hex(b, r->base + i, 5);
        d = put(d, "  UNL ");
        d = hex(d, r->a[i], 2);
        d = put(d, "  LCK ");
        hex(d, r->b[k], 2);
        line(y, b, GFX_WHITE);
    }
    d = put(b, "UP/DN ");
    d = dec(d, top + 1);
    d = put(d, "-");
    dec(d, top + ROWS < r->ncand ? top + ROWS : r->ncand);
    line(H9_HEIGHT - 9, b, GFX_GREY);
}

static void busy(const char *s)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(8, 56, s, 24, 1, GFX_YELLOW, GFX_BLACK);
}

/* A key once two readings agree (the keypad ADC has no debounce). */
static unsigned key(unsigned *last, unsigned *seen)
{
    unsigned k = h9_key();
    if (k != *last) { *last = k; return 0; }
    if (k == *seen) return 0;
    *seen = k;
    return k;
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct lr *r = LR;
    if ((uint32_t)&lr_end + sizeof lr_end > (uint32_t)r) {
        busy("MODULE TOO BIG");
        gfx_wait_key();
        return 0;
    }
    if (r->magic != LR_MAGIC || r->phase > PH_B || r->ncand > MAXC || r->base > BASE_MAX) {
        r->magic = LR_MAGIC;
        r->base = r->next = BASE_MIN;
        r->phase = PH_NONE;
        r->steps = 0;
        r->ncand = r->lost = 0;
    }
    unsigned last = h9_key(), seen = last, top = 0, dirty = 1;   /* the held # that opened the menu is not a key */
    for (;;) {
        if (dirty) { draw(r, top); dirty = 0; }
        unsigned k = key(&last, &seen);
        if (k == H9_KEY_BACK) return 0;
        if (k == KEY_1 || ((k == KEY_2 || k == KEY_3) && r->phase != PH_NONE)) {
            busy("READING 2 S, HANDS OFF");
            gfx_wait_release();
            if (k == KEY_1) step_new(r);
            else {
                if (k == KEY_2) step_locked(r);
                else step_unlocked(r);
                if (r->steps < 255) r->steps++;
            }
            top = 0;
            dirty = 1;
        } else if (k == KEY_STAR && r->next > BASE_MIN) { r->next -= WIN; dirty = 1; }
        else if (k == KEY_HASH && r->next < BASE_MAX) { r->next += WIN; dirty = 1; }
        else if (k == H9_KEY_UP && top) { top = top >= ROWS ? top - ROWS : 0; dirty = 1; }
        else if (k == H9_KEY_DOWN && top + ROWS < r->ncand) { top += ROWS; dirty = 1; }
        h9_delay(10);
    }
}
