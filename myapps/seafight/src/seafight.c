/* SEAFIGHT: battleship, vs CPU or APRS. State sits at 0x2B8E0 (no .bss).
 * Body must end below satshare 0x2B880; sf_end is the last word. */
#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)
#define MYCALL ((const volatile uint8_t *)0xC4A1)
#define RING ((volatile uint8_t *)0x16634)
#define RING_WRITE (*(volatile uint16_t *)0x16E34)
#define RING_READ (*(volatile uint16_t *)0x16E36)
#define RING_MASK 2047u
#define RF_ASLEEP (*(volatile uint8_t *)0x11B01)
#define RF_WAKE() ((void (*)(void))0x01E5969Cu)()
#define AF_SELECT(m) ((void (*)(unsigned))0x01E593E4u)(m)
#define RF_WRITE(r, v) ((void (*)(unsigned, unsigned))0x01E5070Au)((r), (v))
#define SPEAKER(on) ((void (*)(unsigned))0x01E52328u)(on)
#define STOCK_KISS_TX ((void (*)(const uint8_t *, unsigned))0x01E5DBCCu)
#define SIDE_BYTE (*(volatile uint8_t *)0x11B50)
#define ROUTE_BYTE (*(volatile uint8_t *)0x11AEC)
#define RF_SETUP(x) ((void (*)(unsigned))0x01E58E3Eu)(x)
#define RX_CH (*(const volatile uint8_t *)0xC532)

enum { KEY_0 = 2, KEY_1 = 3, KEY_2 = 4, KEY_4 = 6, KEY_5 = 7, KEY_6 = 8,
       KEY_8 = 10, KEY_9 = 11, KEY_MENU = 12, KEY_BACK = 15, KEY_HASH = 0x14 };
enum { N = 10, CELL = 7, MY_X = 6, EN_X = 84, GRID_Y = 24 };
enum { C_WATER, C_SHIP, C_MISS, C_HIT };
enum { COL_SEA = 0x0015, COL_SHIP = 0x07E0, COL_MISS = 0xC618, COL_HIT = 0xF800, COL_CUR = 0xFFE0 };
enum { BODY_TOP = 0x2B880, FIRE_TIMEOUT = 12000u };

#define AT(g, x, y) ((g)[(y) * N + (x)])

struct sf {
    uint8_t my[100], en[100], real[100], vis[100], rx[128];
    int8_t qx[32], qy[32];
    char call[7], room[2];
    int8_t cx, cy, fx, fy;
    uint8_t myhp, enhp, rxn, esc, klast, kseen;
    uint8_t aprs, turn, started, pending, timeout;
    uint32_t rng, ftick;
};

#define SF ((struct sf *)0x2B8E0)

_Static_assert(sizeof(struct sf) <= 0x600, "state must fit in the APRS slot");
_Static_assert(0x2B8E0u + sizeof(struct sf) <= 0x2BEE0u, "state below the mailbox");

extern void *memset(void *dst, int value, unsigned n);
extern const uint32_t sf_end;

static const int8_t DX[4] = { 0, 1, 0, -1 }, DY[4] = { -1, 0, 1, 0 };

static uint32_t rnd(void)
{
    struct sf *s = SF;
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}

static unsigned poll_key(void)
{
    struct sf *s = SF;
    unsigned k = h9_key();
    if (k != s->klast) { s->klast = (uint8_t)k; return 0; }
    if (k == s->kseen) return 0;
    if (k == H9_KEY_NONE) { s->kseen = 0; return 0; }
    s->kseen = (uint8_t)k;
    return k;
}

static void aprs_quiet(void)
{
    if (RF_ASLEEP) RF_WAKE();
    AF_SELECT(1);
    RF_WRITE(0x30, 0xBFF5);
    SPEAKER(0);
}

static void pack_addr(uint8_t *d, const char *call, int last)
{
    int i = 0;
    for (; i < 6 && call[i] && call[i] != '-'; i++) {
        uint8_t c = (uint8_t)call[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        d[i] = (uint8_t)(c << 1);
    }
    for (; i < 6; i++) d[i] = (uint8_t)(' ' << 1);
    d[6] = (uint8_t)(0x60 | (last ? 1 : 0));
}

static void load_call(void)
{
    struct sf *s = SF;
    int n = 0;
    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) s->call[n] = (char)MYCALL[n];
    if (!n) {
        s->call[0] = 'N'; s->call[1] = '0'; s->call[2] = 'C';
        s->call[3] = 'A'; s->call[4] = 'L'; s->call[5] = 'L';
        n = 6;
    }
    s->call[n] = 0;
}

/* dst=APH9BS, src=call, ">BS<room>:<cmd>,x,y[,len,h]". Do not touch the RF box after TX. */
static void send_pkt(const char *cmd, int x, int y, int l, int h)
{
    struct sf *s = SF;
    uint8_t kiss[88];
    uint8_t frame[64];
    int p = 0, k = 0;
    pack_addr(frame + p, "APH9BS", 0); p += 7;
    pack_addr(frame + p, s->call, 1); p += 7;
    frame[p++] = 0x03;
    frame[p++] = 0xF0;
    frame[p++] = '>';
    frame[p++] = 'B';
    frame[p++] = 'S';
    frame[p++] = (uint8_t)s->room[0];
    frame[p++] = (uint8_t)s->room[1];
    frame[p++] = ':';
    while (*cmd) frame[p++] = (uint8_t)*cmd++;
    if (x >= 0 && y >= 0) {
        frame[p++] = ',';
        frame[p++] = (uint8_t)('0' + x);
        frame[p++] = ',';
        frame[p++] = (uint8_t)('0' + y);
        if (l >= 0) {
            frame[p++] = ',';
            frame[p++] = (uint8_t)('0' + l);
            frame[p++] = ',';
            frame[p++] = (uint8_t)('0' + h);
        }
    }
    kiss[k++] = 0xC0;
    kiss[k++] = 0x00;
    for (int i = 0; i < p; i++) {
        if (frame[i] == 0xC0) { kiss[k++] = 0xDB; kiss[k++] = 0xDC; }
        else if (frame[i] == 0xDB) { kiss[k++] = 0xDB; kiss[k++] = 0xDD; }
        else kiss[k++] = frame[i];
    }
    kiss[k++] = 0xC0;
    STOCK_KISS_TX(kiss, (unsigned)k);
}

static int self_rx(void)
{
    struct sf *s = SF;
    for (int i = 0; i < 6; i++) {
        uint8_t c = (uint8_t)s->call[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!s->call[i] || s->call[i] == '-') c = ' ';
        if (s->rx[8 + i] != (uint8_t)(c << 1)) return 0;
    }
    return 1;
}

/* 1: a game packet from someone else is in cmd / v[0..3]. */
static int poll_rx(char *cmd, int *v, int *nv)
{
    struct sf *s = SF;
    unsigned write = RING_WRITE & RING_MASK, read = RING_READ & RING_MASK;
    int got = 0;
    while (read != write) {
        uint8_t b = RING[read];
        read = (read + 1u) & RING_MASK;
        if (b == 0xC0) {
            if (s->rxn > 20 && s->rx[0] == 0 && !self_rx()) {
                unsigned at = 17, p;
                int n = 0;
                if (at + 8 <= s->rxn && s->rx[at] == '>' && s->rx[at + 1] == 'B' && s->rx[at + 2] == 'S' &&
                    s->rx[at + 3] == (uint8_t)s->room[0] && s->rx[at + 4] == (uint8_t)s->room[1] &&
                    s->rx[at + 5] == ':') {
                    p = at + 6;
                    while (p < s->rxn && s->rx[p] != ',' && n < 7) cmd[n++] = (char)s->rx[p++];
                    cmd[n] = 0;
                    v[0] = v[1] = v[2] = v[3] = -1;
                    *nv = 0;
                    while (p < s->rxn && *nv < 4 && s->rx[p] == ',') {
                        p++;
                        v[(*nv)++] = (p < s->rxn && s->rx[p] >= '0' && s->rx[p] <= '9') ? s->rx[p] - '0' : -1;
                        p++;
                    }
                    if (n) got = 1;
                }
            }
            s->rxn = s->esc = 0;
            if (got) break;
            continue;
        }
        if (s->esc) { b = b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b; s->esc = 0; }
        else if (b == 0xDB) { s->esc = 1; continue; }
        if (s->rxn < sizeof s->rx) s->rx[s->rxn++] = b;
    }
    RING_READ = (uint16_t)read;
    return got;
}

static void draw_cell(int bx, int x, int y, uint8_t st, int cur)
{
    int sx = bx + x * CELL, sy = GRID_Y + y * CELL;
    gfx_fill((unsigned)sx, (unsigned)sy, CELL - 1, CELL - 1, st == C_SHIP ? COL_SHIP : COL_SEA);
    if (st == C_MISS) gfx_fill((unsigned)(sx + 2), (unsigned)(sy + 2), 2, 2, COL_MISS);
    else if (st == C_HIT) {
        gfx_fill((unsigned)(sx + 1), (unsigned)(sy + 1), CELL - 3, CELL - 3, COL_HIT);
        gfx_fill((unsigned)(sx + 2), (unsigned)(sy + 2), 2, 2, GFX_YELLOW);
    }
    if (cur) {
        gfx_fill((unsigned)sx, (unsigned)sy, CELL - 1, 1, COL_CUR);
        gfx_fill((unsigned)sx, (unsigned)(sy + CELL - 2), CELL - 1, 1, COL_CUR);
        gfx_fill((unsigned)sx, (unsigned)sy, 1, CELL - 1, COL_CUR);
        gfx_fill((unsigned)(sx + CELL - 2), (unsigned)sy, 1, CELL - 1, COL_CUR);
    }
}

static void redraw(void)
{
    struct sf *s = SF;
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            draw_cell(MY_X, x, y, AT(s->my, x, y), 0);
            draw_cell(EN_X, x, y, AT(s->en, x, y), x == s->cx && y == s->cy);
        }
}

static void status(const char *m, uint16_t c)
{
    gfx_fill(0, 100, H9_WIDTH, 13, GFX_BLACK);
    gfx_text(4, 102, m, 25, 1, c, GFX_BLACK);
}

static int can_place(uint8_t *g, int x, int y, int len, int h)
{
    for (int i = 0; i < len; i++) {
        int nx = x + (h ? i : 0), ny = y + (h ? 0 : i);
        if (nx >= N || ny >= N) return 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                int cx = nx + dx, cy = ny + dy;
                if (cx >= 0 && cx < N && cy >= 0 && cy < N && AT(g, cx, cy) != C_WATER) return 0;
            }
    }
    return 1;
}

static void put_ship(uint8_t *g, int x, int y, int len, int h)
{
    for (int i = 0; i < len; i++) AT(g, x + (h ? i : 0), y + (h ? 0 : i)) = C_SHIP;
}

static void place_fleet(uint8_t *g)
{
    static const uint8_t SHIPS[10] = { 4, 3, 3, 2, 2, 2, 1, 1, 1, 1 };
    for (int round = 0; round < 40; round++) {
        int s, ok = 1;
        for (int i = 0; i < 100; i++) g[i] = C_WATER;
        for (s = 0; s < 10; s++) {
            int n, placed = 0;
            for (n = 0; n < 200 && !placed; n++) {
                int x = (int)(rnd() % N), y = (int)(rnd() % N), h = (int)(rnd() & 1u);
                if (can_place(g, x, y, SHIPS[s], h)) { put_ship(g, x, y, SHIPS[s], h); placed = 1; }
            }
            if (!placed) { ok = 0; break; }
        }
        if (ok) return;
    }
    for (int i = 0; i < 100; i++) g[i] = C_WATER;
    put_ship(g, 0, 0, 4, 1);
}

/* Cells of type a or b, 4-connected, into SF->qx/qy. Returns count. */
static int walk(uint8_t *g, int x, int y, uint8_t a, uint8_t b)
{
    struct sf *s = SF;
    int head = 0, tail = 0, i;
    uint8_t t;
    for (i = 0; i < 100; i++) s->vis[i] = 0;
    if (x < 0 || x >= N || y < 0 || y >= N) return 0;
    t = AT(g, x, y);
    if (t != a && t != b) return 0;
    s->qx[0] = (int8_t)x;
    s->qy[0] = (int8_t)y;
    tail = 1;
    s->vis[y * N + x] = 1;
    while (head < tail) {
        int cx = s->qx[head], cy = s->qy[head];
        head++;
        for (i = 0; i < 4; i++) {
            int nx = cx + DX[i], ny = cy + DY[i];
            if (nx < 0 || nx >= N || ny < 0 || ny >= N || s->vis[ny * N + nx]) continue;
            t = AT(g, nx, ny);
            if (t != a && t != b) continue;
            s->vis[ny * N + nx] = 1;
            if (tail < 32) { s->qx[tail] = (int8_t)nx; s->qy[tail] = (int8_t)ny; tail++; }
        }
    }
    return tail;
}

static int sunk(uint8_t *orig, uint8_t *dmg, int x, int y)
{
    struct sf *s = SF;
    int n = walk(orig, x, y, C_SHIP, C_HIT), i;
    if (!n) return 0;
    for (i = 0; i < n; i++)
        if (AT(dmg, s->qx[i], s->qy[i]) != C_HIT) return 0;
    return 1;
}

static void sink_area(uint8_t *g, int bx, int x, int y)
{
    struct sf *s = SF;
    int n = walk(g, x, y, C_HIT, C_HIT), q;
    for (q = 0; q < n; q++) {
        int gx = s->qx[q], gy = s->qy[q], dx, dy;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                int nx = gx + dx, ny = gy + dy;
                if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
                if (AT(g, nx, ny) == C_WATER) {
                    AT(g, nx, ny) = C_MISS;
                    draw_cell(bx, nx, ny, C_MISS, bx == EN_X && nx == s->cx && ny == s->cy);
                }
            }
    }
}

static void ship_span(int x, int y, int *sx, int *sy, int *len, int *horiz)
{
    struct sf *s = SF;
    int x0 = x, y0 = y, l = 1;
    int h = (x > 0 && AT(s->my, x - 1, y) == C_HIT) || (x < N - 1 && AT(s->my, x + 1, y) == C_HIT);
    if (h) {
        while (x0 > 0 && AT(s->my, x0 - 1, y) == C_HIT) x0--;
        while (x0 + l < N && AT(s->my, x0 + l, y) == C_HIT) l++;
        *sx = x0;
        *sy = y;
    } else {
        while (y0 > 0 && AT(s->my, x, y0 - 1) == C_HIT) y0--;
        while (y0 + l < N && AT(s->my, x, y0 + l) == C_HIT) l++;
        *sx = x;
        *sy = y0;
    }
    *len = l;
    *horiz = h;
}

static void sink_span_en(int sx, int sy, int len, int h)
{
    struct sf *s = SF;
    for (int i = 0; i < len; i++) {
        int gx = sx + (h ? i : 0), gy = sy + (h ? 0 : i), dx, dy;
        if (AT(s->en, gx, gy) != C_HIT) {
            AT(s->en, gx, gy) = C_HIT;
            draw_cell(EN_X, gx, gy, C_HIT, gx == s->cx && gy == s->cy);
        }
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                int nx = gx + dx, ny = gy + dy;
                if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
                if (AT(s->en, nx, ny) == C_WATER) {
                    AT(s->en, nx, ny) = C_MISS;
                    draw_cell(EN_X, nx, ny, C_MISS, nx == s->cx && ny == s->cy);
                }
            }
    }
}

static int free_cell(uint8_t t) { return t == C_WATER || t == C_SHIP; }

static int cpu_turn(void)
{
    struct sf *s = SF;
    int tx = -1, ty = -1, hx = -1, hy = -1, x, y, d, guard;
    for (y = 0; y < N && ty < 0; y++)
        for (x = 0; x < N; x++) {
            if (AT(s->my, x, y) != C_HIT) continue;
            if (hx < 0) { hx = x; hy = y; }
            if ((x > 0 && AT(s->my, x - 1, y) == C_HIT) || (x < N - 1 && AT(s->my, x + 1, y) == C_HIT)) {
                if (x < N - 1 && free_cell(AT(s->my, x + 1, y))) { tx = x + 1; ty = y; break; }
                if (x > 0 && free_cell(AT(s->my, x - 1, y))) { tx = x - 1; ty = y; break; }
            }
            if ((y > 0 && AT(s->my, x, y - 1) == C_HIT) || (y < N - 1 && AT(s->my, x, y + 1) == C_HIT)) {
                if (y < N - 1 && free_cell(AT(s->my, x, y + 1))) { tx = x; ty = y + 1; break; }
                if (y > 0 && free_cell(AT(s->my, x, y - 1))) { tx = x; ty = y - 1; break; }
            }
        }
    if (tx < 0 && hx >= 0)
        for (d = 0; d < 4 && tx < 0; d++) {
            int nx = hx + DX[d], ny = hy + DY[d];
            if (nx >= 0 && nx < N && ny >= 0 && ny < N && free_cell(AT(s->my, nx, ny))) {
                tx = nx;
                ty = ny;
            }
        }
    if (tx < 0) {
        for (guard = 400; tx < 0 && guard--; ) {
            int rx = (int)(rnd() % N), ry = (int)(rnd() % N);
            if (free_cell(AT(s->my, rx, ry))) { tx = rx; ty = ry; }
        }
        if (tx < 0) return 0;
    }
    if (AT(s->my, tx, ty) == C_SHIP) {
        AT(s->my, tx, ty) = C_HIT;
        if (s->myhp) s->myhp--;
        draw_cell(MY_X, tx, ty, C_HIT, 0);
        if (sunk(s->my, s->my, tx, ty)) {
            sink_area(s->my, MY_X, tx, ty);
            status("ENEMY HIT YOU!", GFX_RED);
        } else status("ENEMY HIT SHIP!", GFX_RED);
        return 1;
    }
    AT(s->my, tx, ty) = C_MISS;
    draw_cell(MY_X, tx, ty, C_MISS, 0);
    status("ENEMY MISSED!", COL_SHIP);
    return 0;
}

/* MENU confirms; 0-9 type digits (5 is a digit, not OK). */
static int select_room(void)
{
    struct sf *s = SF;
    int pos = 0;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(30, 30, "ROOM (0-99):", 12, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(8, 102, "0-9 DIGIT  MENU OK", 20, 1, GFX_GREY, GFX_BLACK);
    for (;;) {
        char r[3];
        r[0] = s->room[0];
        r[1] = s->room[1];
        r[2] = 0;
        gfx_fill(52, 56, 56, 30, GFX_BLACK);
        gfx_text(64, 62, r, 4, 2, COL_CUR, GFX_BLACK);
        unsigned k = poll_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_MENU) return 1;
        if (k == KEY_0 || (k >= KEY_1 && k <= KEY_9)) {
            char d = (char)('0' + (k - 2));
            if (!pos) { s->room[0] = d; pos = 1; }
            else s->room[1] = d;
        }
        h9_delay(10);
    }
}

static void count_hp(void)
{
    struct sf *s = SF;
    s->myhp = s->enhp = 0;
    for (int i = 0; i < 100; i++) {
        if (s->my[i] == C_SHIP) s->myhp++;
        if (!s->aprs && s->real[i] == C_SHIP) s->enhp++;
    }
    if (s->aprs) s->enhp = 20;
}

static void init_game(void)
{
    struct sf *s = SF;
    s->rng = TICK ^ 0x5EA12345u;
    if (!s->rng) s->rng = 101;
    place_fleet(s->my);
    if (!s->aprs) place_fleet(s->real);
    for (int i = 0; i < 100; i++) s->en[i] = C_WATER;
    s->cx = s->cy = 4;
    count_hp();
    s->turn = 1;
    s->started = s->pending = s->timeout = 0;
    s->fx = s->fy = -1;
    s->ftick = 0;
    s->rxn = s->esc = 0;
}

static void draw_board(void)
{
    struct sf *s = SF;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(10, 12, "MY", 8, 1, COL_SHIP, GFX_BLACK);
    if (s->aprs) {
        char hdr[8] = { 'R', 'O', 'O', 'M', ' ', s->room[0], s->room[1], 0 };
        gfx_text(EN_X + 6, 12, hdr, 8, 1, 0xF81F, GFX_BLACK);
    } else gfx_text(EN_X + 6, 12, "EN", 8, 1, 0x07FF, GFX_BLACK);
    gfx_text(4, 117, "2468:AIM 5:FIRE #:NEW", 26, 1, GFX_GREY, GFX_BLACK);
    redraw();
}

static void clear_shot(void)
{
    struct sf *s = SF;
    s->pending = s->timeout = 0;
    s->fx = s->fy = -1;
}

/* Incoming FIRE: apply=1 first time, 0 retry. HIT/SUNK keep their turn; MISS gives us the turn. */
static void on_fire(int x, int y, int apply)
{
    struct sf *s = SF;
    uint8_t t = AT(s->my, x, y);
    if (t == C_SHIP && apply) {
        AT(s->my, x, y) = C_HIT;
        if (s->myhp) s->myhp--;
        draw_cell(MY_X, x, y, C_HIT, 0);
        if (sunk(s->my, s->my, x, y)) {
            int sx, sy, len, h;
            sink_area(s->my, MY_X, x, y);
            ship_span(x, y, &sx, &sy, &len, &h);
            send_pkt("SUNK", sx, sy, len, h);
            status("YOUR SHIP SUNK!", GFX_RED);
        } else {
            send_pkt("HIT", x, y, -1, -1);
            status("HIT! ENEMY SHOTS", GFX_RED);
        }
        s->turn = 0;
        return;
    }
    if (t == C_HIT) {
        if (sunk(s->my, s->my, x, y)) {
            int sx, sy, len, h;
            ship_span(x, y, &sx, &sy, &len, &h);
            send_pkt("SUNK", sx, sy, len, h);
        } else send_pkt("HIT", x, y, -1, -1);
        return;
    }
    if (t == C_WATER && apply) {
        AT(s->my, x, y) = C_MISS;
        s->turn = 1;
    }
    draw_cell(MY_X, x, y, C_MISS, 0);
    send_pkt("MISS", x, y, -1, -1);
    if (apply && t == C_WATER) status("ENEMY MISS! TURN", COL_SHIP);
}

static void drop_shot(const char *msg)
{
    struct sf *s = SF;
    clear_shot();
    s->turn = 1;
    status(msg, COL_MISS);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    struct sf *s = SF;
    (void)api;
    (void)self;
    if ((uint32_t)&sf_end + sizeof sf_end > BODY_TOP) {
        gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
        gfx_text(4, 50, "MODULE TOO BIG", 16, 1, GFX_RED, GFX_BLACK);
        gfx_wait_key();
        return 0;
    }
    memset(s, 0, sizeof *s);
    s->room[0] = '0';
    s->room[1] = '1';
    load_call();

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(22, 14, "BATTLESHIP", 16, 1, 0x07FF, GFX_BLACK);
    gfx_text(14, 54, "1: PLAY VS CPU", 18, 1, COL_SHIP, GFX_BLACK);
    gfx_text(14, 78, "2: APRS RADIO LINK", 18, 1, 0xF81F, GFX_BLACK);
    gfx_text(14, 102, "BACK: EXIT", 12, 1, GFX_RED, GFX_BLACK);
    s->klast = s->kseen = (uint8_t)h9_key();
    for (;;) {
        unsigned k = poll_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_1) { s->aprs = 0; break; }
        if (k == KEY_2) {
            s->aprs = 1;
            if (!select_room()) return 0;
            break;
        }
        h9_delay(10);
    }

    if (s->aprs) {
        unsigned side = RX_CH == 1 ? 0 : RX_CH == 2 ? 1 : 2;
        SIDE_BYTE = (uint8_t)((SIDE_BYTE & 0x7F) | side << 7);
        ROUTE_BYTE = (uint8_t)side;
        RF_SETUP(0);
        aprs_quiet();
    }

    init_game();
    draw_board();
    status("YOUR TURN! FIRE", COL_CUR);

    for (;;) {
        unsigned k = poll_key();

        if (k == KEY_BACK) {
            if (s->aprs && s->pending) { drop_shot("SHOT CANCELLED"); continue; }
            return 0;
        }

        if (s->aprs && s->pending && !s->timeout && (uint32_t)(TICK - s->ftick) >= FIRE_TIMEOUT) {
            s->timeout = 1;
            status("NO REPLY! 5:RETRY", GFX_RED);
        }
        if (s->aprs && s->timeout) {
            if (k == KEY_5) {
                send_pkt("FIRE", s->fx, s->fy, -1, -1);
                s->ftick = TICK;
                s->timeout = 0;
                status("RETRY SENT...", COL_CUR);
                continue;
            }
            if (k == KEY_BACK) { drop_shot("SHOT DROPPED"); continue; }
        }

        if (k == KEY_HASH && !s->started) {
            place_fleet(s->my);
            if (!s->aprs) place_fleet(s->real);
            count_hp();
            redraw();
            status("FLEET REARRANGED", 0x07FF);
            continue;
        }

        if (s->aprs) {
            char cmd[8];
            int v[4], nv = 0;
            if (poll_rx(cmd, v, &nv)) {
                int x = v[0], y = v[1];
                if (cmd[0] == 'F' && nv >= 2 && x >= 0 && x < N && y >= 0 && y < N) {
                    int first = AT(s->my, x, y) == C_SHIP || AT(s->my, x, y) == C_WATER;
                    if (s->started && s->turn && !s->pending && first) {
                        /* our turn: ignore a stray first shot */
                    } else {
                        s->started = 1;
                        on_fire(x, y, first);
                    }
                } else if (cmd[0] == 'H' && nv >= 2 && x >= 0 && x < N && y >= 0 && y < N) {
                    if (AT(s->en, x, y) != C_HIT) {
                        AT(s->en, x, y) = C_HIT;
                        if (s->enhp) s->enhp--;
                        draw_cell(EN_X, x, y, C_HIT, x == s->cx && y == s->cy);
                    }
                    clear_shot();
                    s->turn = 1;
                    status("DIRECT HIT! AGAIN", COL_CUR);
                } else if (cmd[0] == 'S' && nv >= 4) {
                    int sx = v[0], sy = v[1], len = v[2], h = v[3];
                    if (len >= 1 && len <= 4 && h >= 0 && h <= 1 && sx >= 0 && sx < N && sy >= 0 && sy < N &&
                        sx + (h ? len - 1 : 0) < N && sy + (h ? 0 : len - 1) < N &&
                        s->fx >= sx && s->fx <= sx + (h ? len - 1 : 0) &&
                        s->fy >= sy && s->fy <= sy + (h ? 0 : len - 1)) {
                        if (s->enhp) s->enhp--;   /* sinking shot never came as HIT */
                        sink_span_en(sx, sy, len, h);
                        clear_shot();
                        s->turn = 1;
                        status("ENEMY SHIP SUNK!", COL_SHIP);
                    }
                } else if (cmd[0] == 'M' && nv >= 2 && x >= 0 && x < N && y >= 0 && y < N) {
                    AT(s->en, x, y) = C_MISS;
                    draw_cell(EN_X, x, y, C_MISS, x == s->cx && y == s->cy);
                    clear_shot();
                    s->turn = 0;
                    status("MISS! WAIT ENEMY", COL_MISS);
                }
            }
        }

        if (s->myhp && s->enhp && k && !(s->aprs && s->pending) && !(s->aprs && !s->turn)) {
            int8_t ox = s->cx, oy = s->cy;
            if (k == KEY_2 && s->cy) s->cy--;
            else if (k == KEY_8 && s->cy < N - 1) s->cy++;
            else if (k == KEY_4 && s->cx) s->cx--;
            else if (k == KEY_6 && s->cx < N - 1) s->cx++;
            if (s->cx != ox || s->cy != oy) {
                draw_cell(EN_X, ox, oy, AT(s->en, ox, oy), 0);
                draw_cell(EN_X, s->cx, s->cy, AT(s->en, s->cx, s->cy), 1);
            }
            if (k == KEY_5 && AT(s->en, s->cx, s->cy) == C_WATER) {
                if (s->aprs) {
                    send_pkt("FIRE", s->cx, s->cy, -1, -1);
                    s->started = s->pending = 1;
                    s->timeout = 0;
                    s->fx = s->cx;
                    s->fy = s->cy;
                    s->ftick = TICK;
                    s->turn = 0;
                    status("SENT! WAITING...", COL_CUR);
                } else if (AT(s->real, s->cx, s->cy) == C_SHIP) {
                    AT(s->en, s->cx, s->cy) = C_HIT;
                    if (s->enhp) s->enhp--;
                    draw_cell(EN_X, s->cx, s->cy, C_HIT, 1);
                    if (sunk(s->real, s->en, s->cx, s->cy)) {
                        sink_area(s->en, EN_X, s->cx, s->cy);
                        status("ENEMY SHIP SUNK!", COL_SHIP);
                    } else status("DIRECT HIT! SHOOT", COL_CUR);
                } else {
                    AT(s->en, s->cx, s->cy) = C_MISS;
                    draw_cell(EN_X, s->cx, s->cy, C_MISS, 1);
                    status("MISS! ENEMY FIRING...", COL_MISS);
                    h9_delay(400);
                    while (cpu_turn()) {
                        h9_delay(400);
                        if (!s->myhp) break;
                    }
                }
            }
        }

        if (!s->myhp || !s->enhp) {
            gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
            if (!s->enhp) gfx_text(42, 52, "YOU WIN", 8, 2, COL_SHIP, GFX_BLACK);
            else gfx_text(38, 52, "YOU LOOSE", 8, 2, GFX_RED, GFX_BLACK);
            gfx_text(48, 92, "BACK: EXIT", 10, 1, GFX_GREY, GFX_BLACK);
            while (h9_key() != H9_KEY_NONE) h9_delay(3);
            for (;;) {
                if (poll_key() == KEY_BACK) break;
                h9_delay(10);
            }
            return 0;
        }
        h9_delay(10);
    }
}

const uint32_t sf_end = 0x444E4653;   /* last .rodata; body must stay below 0x2B880 */
