/* SEAFIGHT board, keys, placement. Linked into SEAFIGHT and .SF RADIO. */
#include "sf.h"

const int8_t SF_DX[4] = { 0, 1, 0, -1 };
const int8_t SF_DY[4] = { -1, 0, 1, 0 };

_Static_assert(sizeof(struct sf) <= 0x600, "state must fit in the APRS slot");
_Static_assert(0x2B8E0u + sizeof(struct sf) <= 0x2BEE0u, "state below the mailbox");

struct sf *sf_open(void)
{
    uint32_t end = (uint32_t)&sf_end + sizeof sf_end;
    if (end > BODY_TOP) {
        char b[24], t[10];
        char *d = b;
        const char *p = "MODULE TOO BIG +";
        uint32_t v = end - BODY_TOP;
        unsigned n = 0;
        while (*p) *d++ = *p++;
        do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
        while (n) *d++ = t[--n];
        *d = 0;
        gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
        gfx_text(4, 50, b, 22, 1, GFX_RED, GFX_BLACK);
        gfx_wait_key();
        return 0;
    }
    return SF;
}

unsigned sf_key(void)
{
    struct sf *s = SF;
    unsigned k = h9_key();
    if (k != s->klast) { s->klast = (uint8_t)k; return 0; }
    if (k == s->kseen) return 0;
    if (k == H9_KEY_NONE) { s->kseen = 0; return 0; }
    s->kseen = (uint8_t)k;
    return k;
}

uint32_t sf_rnd(void)
{
    struct sf *s = SF;
    s->rng ^= s->rng << 13;
    s->rng ^= s->rng >> 17;
    s->rng ^= s->rng << 5;
    return s->rng;
}

void sf_status(const char *m, uint16_t c)
{
    gfx_fill(0, 100, H9_WIDTH, 13, GFX_BLACK);
    gfx_text(4, 102, m, 25, 1, c, GFX_BLACK);
}

void sf_cell(int bx, int x, int y, uint8_t st, int cur)
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

void sf_redraw(void)
{
    struct sf *s = SF;
    int y, x;
    for (y = 0; y < N; y++)
        for (x = 0; x < N; x++) {
            sf_cell(MY_X, x, y, AT(s->my, x, y), 0);
            sf_cell(EN_X, x, y, AT(s->en, x, y), x == s->cx && y == s->cy);
        }
}

void sf_board(void)
{
    struct sf *s = SF;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(10, 12, "MY", 8, 1, COL_SHIP, GFX_BLACK);
    if (s->aprs) {
        char hdr[8] = { 'R', 'O', 'O', 'M', ' ', s->room[0], s->room[1], 0 };
        gfx_text(EN_X + 6, 12, hdr, 8, 1, 0xF81F, GFX_BLACK);
    } else gfx_text(EN_X + 6, 12, "EN", 8, 1, 0x07FF, GFX_BLACK);
    gfx_text(4, 117, "24568:AIM 0:FIRE #:NEW", 26, 1, GFX_GREY, GFX_BLACK);
    sf_redraw();
}

static int can_place(uint8_t *g, int x, int y, int len, int h)
{
    int i, dy, dx, nx, ny, cx, cy;
    for (i = 0; i < len; i++) {
        nx = x + (h ? i : 0);
        ny = y + (h ? 0 : i);
        if (nx >= N || ny >= N) return 0;
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                cx = nx + dx;
                cy = ny + dy;
                if (cx >= 0 && cx < N && cy >= 0 && cy < N && AT(g, cx, cy) != C_WATER) return 0;
            }
    }
    return 1;
}

static void put_ship(uint8_t *g, int x, int y, int len, int h)
{
    int i;
    for (i = 0; i < len; i++) AT(g, x + (h ? i : 0), y + (h ? 0 : i)) = C_SHIP;
}

void sf_place(uint8_t *g)
{
    static const uint8_t SHIPS[10] = { 4, 3, 3, 2, 2, 2, 1, 1, 1, 1 };
    int round, s, i, n, x, y, h, ok, placed;
    for (round = 0; round < 40; round++) {
        ok = 1;
        for (i = 0; i < 100; i++) g[i] = C_WATER;
        for (s = 0; s < 10; s++) {
            placed = 0;
            for (n = 0; n < 200 && !placed; n++) {
                x = (int)(sf_rnd() % N);
                y = (int)(sf_rnd() % N);
                h = (int)(sf_rnd() & 1u);
                if (can_place(g, x, y, SHIPS[s], h)) { put_ship(g, x, y, SHIPS[s], h); placed = 1; }
            }
            if (!placed) { ok = 0; break; }
        }
        if (ok) return;
    }
    for (i = 0; i < 100; i++) g[i] = C_WATER;
    put_ship(g, 0, 0, 4, 1);
}

static int walk(uint8_t *g, int x, int y, uint8_t a, uint8_t b)
{
    struct sf *s = SF;
    int head = 0, tail = 0, i, cx, cy, nx, ny;
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
        cx = s->qx[head];
        cy = s->qy[head];
        head++;
        for (i = 0; i < 4; i++) {
            nx = cx + SF_DX[i];
            ny = cy + SF_DY[i];
            if (nx < 0 || nx >= N || ny < 0 || ny >= N || s->vis[ny * N + nx]) continue;
            t = AT(g, nx, ny);
            if (t != a && t != b) continue;
            s->vis[ny * N + nx] = 1;
            if (tail < 32) { s->qx[tail] = (int8_t)nx; s->qy[tail] = (int8_t)ny; tail++; }
        }
    }
    return tail;
}

int sf_sunk(uint8_t *orig, uint8_t *dmg, int x, int y)
{
    struct sf *s = SF;
    int n = walk(orig, x, y, C_SHIP, C_HIT), i;
    if (!n) return 0;
    for (i = 0; i < n; i++)
        if (AT(dmg, s->qx[i], s->qy[i]) != C_HIT) return 0;
    return 1;
}

void sf_sink(uint8_t *g, int bx, int x, int y)
{
    struct sf *s = SF;
    int n = walk(g, x, y, C_HIT, C_HIT), q, gx, gy, dx, dy, nx, ny;
    for (q = 0; q < n; q++) {
        gx = s->qx[q];
        gy = s->qy[q];
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                nx = gx + dx;
                ny = gy + dy;
                if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
                if (AT(g, nx, ny) == C_WATER) {
                    AT(g, nx, ny) = C_MISS;
                    sf_cell(bx, nx, ny, C_MISS, bx == EN_X && nx == s->cx && ny == s->cy);
                }
            }
    }
}

void sf_count_hp(void)
{
    struct sf *s = SF;
    int i;
    s->myhp = s->enhp = 0;
    for (i = 0; i < 100; i++) {
        if (s->my[i] == C_SHIP) s->myhp++;
        if (!s->aprs && s->real[i] == C_SHIP) s->enhp++;
    }
    if (s->aprs) s->enhp = 20;
}

void sf_init(void)
{
    struct sf *s = SF;
    int i;
    s->rng = TICK ^ 0x5EA12345u;
    if (!s->rng) s->rng = 101;
    sf_place(s->my);
    if (!s->aprs) sf_place(s->real);
    for (i = 0; i < 100; i++) s->en[i] = C_WATER;
    s->cx = s->cy = 4;
    sf_count_hp();
    s->turn = 1;
    s->started = s->pending = s->timeout = 0;
    s->fx = s->fy = -1;
    s->ftick = 0;
    s->rxn = s->esc = 0;
    s->klast = s->kseen = (uint8_t)h9_key();
}

void sf_play(void (*do_fire)(void), void (*on_tick)(void))
{
    struct sf *s = SF;
    for (;;) {
        unsigned k = sf_key();
        int8_t ox, oy;
        if (on_tick) on_tick();

        if (k == KEY_BACK) {
            if (s->pending) {
                s->pending = s->timeout = 0;
                s->fx = s->fy = -1;
                s->turn = 1;
                sf_status("SHOT CANCELLED", COL_MISS);
                k = 0;
            } else return;
        }

        if (s->pending && !s->timeout && (uint32_t)(TICK - s->ftick) >= FIRE_TIMEOUT) {
            s->timeout = 1;
            sf_status("NO REPLY! 0:RETRY", GFX_RED);
        }
        if (s->timeout && k == KEY_0) {
            do_fire();
            k = 0;
        }

        if (k == KEY_HASH && !s->started) {
            sf_place(s->my);
            if (!s->aprs) sf_place(s->real);
            sf_count_hp();
            sf_redraw();
            sf_status("FLEET REARRANGED", 0x07FF);
        }

        if (s->myhp && s->enhp && k && !s->pending && s->turn) {
            ox = s->cx;
            oy = s->cy;
            if (k == KEY_2 && s->cy) s->cy--;
            else if ((k == KEY_8 || k == KEY_5) && s->cy < N - 1) s->cy++;
            else if (k == KEY_4 && s->cx) s->cx--;
            else if (k == KEY_6 && s->cx < N - 1) s->cx++;
            if (s->cx != ox || s->cy != oy) {
                sf_cell(EN_X, ox, oy, AT(s->en, ox, oy), 0);
                sf_cell(EN_X, s->cx, s->cy, AT(s->en, s->cx, s->cy), 1);
            }
            if (k == KEY_0 && AT(s->en, s->cx, s->cy) == C_WATER)
                do_fire();
        }

        if (!s->myhp || !s->enhp) {
            gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
            if (!s->enhp) gfx_text(42, 52, "YOU WIN", 8, 2, COL_SHIP, GFX_BLACK);
            else gfx_text(38, 52, "YOU LOOSE", 8, 2, GFX_RED, GFX_BLACK);
            gfx_text(48, 92, "BACK: EXIT", 10, 1, GFX_GREY, GFX_BLACK);
            while (h9_key() != H9_KEY_NONE) h9_delay(3);
            for (;;) {
                if (sf_key() == KEY_BACK) break;
                h9_delay(10);
            }
            return;
        }
        h9_delay(10);
    }
}
