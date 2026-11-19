/* Table, physics, keys. Linked into APRSPOOL and .PL RADIO. */
#include "pl.h"

const uint16_t PL_COLORS[MAX_BALLS] = {
    0xFFFF, 0xFFE0, 0x001F, 0xF800, 0xF81F, 0xFD20, 0x07E0, 0x07FF, 0xC618, 0x2104
};

_Static_assert(sizeof(struct pl) <= 0x600, "state must fit in the APRS slot");
_Static_assert(0x2B8E0u + sizeof(struct pl) <= 0x2BEE0u, "state below the mailbox");

struct pl *pl_open(void)
{
    uint32_t end = (uint32_t)&pl_end + sizeof pl_end;
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
    return PL;
}

unsigned pl_key(void)
{
    struct pl *s = PL;
    unsigned k = h9_key();
    if (k != s->klast) { s->klast = (uint8_t)k; return 0; }
    if (k == s->kseen) return 0;
    if (k == H9_KEY_NONE) { s->kseen = 0; return 0; }
    s->kseen = (uint8_t)k;
    return k;
}

static int32_t sin_deg(int a)
{
    static const uint16_t T[10] = { 0, 178, 350, 512, 658, 784, 887, 962, 1008, 1024 };
    int neg, i, f;
    int32_t v;
    a %= 360;
    if (a < 0) a += 360;
    neg = (a >= 180);
    a %= 180;
    if (a > 90) a = 180 - a;
    i = a / 10;
    f = a % 10;
    v = (int32_t)T[i] + (i < 9 ? ((int32_t)T[i + 1] - (int32_t)T[i]) * f / 10 : 0);
    return neg ? -v : v;
}

static int32_t cos_deg(int a) { return sin_deg(a + 90); }

static uint32_t isqrt(uint32_t n)
{
    uint32_t x, y;
    if (!n) return 0;
    x = n;
    y = x / 2u + 1u;
    while (y < x) { x = y; y = (x + n / x) / 2u; }
    return x;
}

static void num_to_str(char *d, uint32_t v)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n) *d++ = t[--n];
    *d = 0;
}

static void draw_table(void)
{
    static const int px[6] = { TBL_X, TBL_X + TBL_W / 2, TBL_X + TBL_W, TBL_X, TBL_X + TBL_W / 2, TBL_X + TBL_W };
    static const int py[6] = { TBL_Y, TBL_Y, TBL_Y, TBL_Y + TBL_H, TBL_Y + TBL_H, TBL_Y + TBL_H };
    int i;
    gfx_fill(TBL_X - 4, TBL_Y - 4, TBL_W + 8, TBL_H + 8, COL_BORDER);
    gfx_fill(TBL_X, TBL_Y, TBL_W, TBL_H, COL_CLOTH);
    for (i = 0; i < 6; i++) gfx_fill(px[i] - 4, py[i] - 4, 9, 9, GFX_BLACK);
}

static void draw_ball(int idx)
{
    struct pl_ball *b = &PL->balls[idx];
    int ix, iy;
    uint16_t c;
    if (!b->active) return;
    ix = TO_PX(b->x + 128);
    iy = TO_PX(b->y + 128);
    c = PL_COLORS[idx];
    gfx_fill(ix - 2, iy - 3, 5, 7, c);
    gfx_fill(ix - 3, iy - 2, 7, 5, c);
    gfx_fill(ix - 1, iy - 1, 2, 2, GFX_WHITE);
    b->ox = b->x;
    b->oy = b->y;
}

static void redraw_ball(int idx)
{
    struct pl_ball *b = &PL->balls[idx];
    int ox = TO_PX(b->ox + 128), oy = TO_PX(b->oy + 128), ix, iy;
    if (!b->active) {
        gfx_fill(ox - 4, oy - 4, 9, 9, COL_CLOTH);
        return;
    }
    ix = TO_PX(b->x + 128);
    iy = TO_PX(b->y + 128);
    if (ox == ix && oy == iy) return;
    gfx_fill(ox - 4, oy - 4, 9, 9, COL_CLOTH);
    draw_ball(idx);
}

static void draw_aim_line(void)
{
    struct pl *s = PL;
    int32_t cs, sn;
    int bx, by, dist, ax, ay;
    if (!s->balls[0].active) return;
    cs = cos_deg(s->aim);
    sn = sin_deg(s->aim);
    bx = TO_PX(s->balls[0].x + 128);
    by = TO_PX(s->balls[0].y + 128);
    for (dist = 8; dist <= 44; dist += 6) {
        ax = bx + (int)((dist * cs) >> 10);
        ay = by + (int)((dist * sn) >> 10);
        if (ax <= TBL_X + 2 || ax >= TBL_X + TBL_W - 2 ||
            ay <= TBL_Y + 2 || ay >= TBL_Y + TBL_H - 2) break;
        gfx_fill(ax, ay, 2, 2, COL_AIM);
    }
}

void pl_hud(void)
{
    struct pl *s = PL;
    char buf[12];
    gfx_fill(0, 0, H9_WIDTH, 13, GFX_BLACK);
    gfx_text(2, 3, "SHT:", 4, 1, GFX_WHITE, GFX_BLACK);
    num_to_str(buf, s->shots);
    gfx_text(28, 3, buf, 4, 1, COL_AIM, GFX_BLACK);
    gfx_text(58, 3, "PWR:", 4, 1, GFX_WHITE, GFX_BLACK);
    gfx_fill(84, 4, 34, 6, 0x4208);
    gfx_fill(84, 4, s->power * 3, 6, (s->power > 8) ? GFX_RED : 0x07E0);
    if (s->aprs) {
        buf[0] = 'R';
        buf[1] = s->room[0];
        buf[2] = s->room[1];
        buf[3] = 0;
        gfx_text(120, 3, buf, 4, 1, 0xF81F, GFX_BLACK);
        gfx_text(142, 3, s->turn ? "YOU" : "WT", 3, 1, s->turn ? COL_AIM : GFX_RED, GFX_BLACK);
    } else {
        gfx_text(120, 3, "*:PAU", 5, 1, COL_AIM, GFX_BLACK);
        gfx_text(148, 3, "X", 1, 1, GFX_RED, GFX_BLACK);
    }
}

void pl_render(void)
{
    int i;
    draw_table();
    for (i = 0; i < MAX_BALLS; i++) draw_ball(i);
    if (PL->state == ST_AIM && (!PL->aprs || PL->turn)) draw_aim_line();
}

void pl_reset(void)
{
    struct pl *s = PL;
    static const int order[9] = { 1, 2, 3, 4, 9, 5, 6, 7, 8 };
    int32_t start_x = TO_FP(TBL_X + 96), mid_y = TO_FP(TBL_Y + TBL_H / 2);
    int32_t step_y = 1850, step_x = 1640;
    int k = 0, col, count, row;
    int32_t cx, sy;
    int b;

    s->balls[0].x = TO_FP(TBL_X + 36);
    s->balls[0].y = TO_FP(TBL_Y + TBL_H / 2);
    s->balls[0].vx = s->balls[0].vy = 0;
    s->balls[0].active = 1;

    for (col = 0; col < 4; col++) {
        count = (col < 3) ? (col + 1) : 3;
        cx = start_x + col * step_x;
        sy = mid_y - ((count - 1) * step_y) / 2;
        for (row = 0; row < count && k < 9; row++) {
            b = order[k++];
            s->balls[b].x = cx;
            s->balls[b].y = sy + row * step_y;
            s->balls[b].vx = s->balls[b].vy = 0;
            s->balls[b].active = 1;
        }
    }
    s->aim = 0;
    s->power = 7;
    s->shots = 0;
    s->state = ST_AIM;
    s->we_shot = 0;
    s->started = 0;
    s->last_tick = TICK;
}

void pl_strike(int angle, int power)
{
    struct pl *s = PL;
    int32_t spd = (int32_t)power * 170;
    s->aim = (int16_t)angle;
    s->power = (uint8_t)power;
    s->balls[0].vx = (spd * cos_deg(angle)) >> 10;
    s->balls[0].vy = (spd * sin_deg(angle)) >> 10;
    s->shots++;
    s->state = ST_ROLL;
    s->started = 1;
    s->last_tick = TICK;
    pl_hud();
}

static void eval_eight(struct pl *s, int i)
{
    int c, rem = 0;
    if (i != 9) return;
    for (c = 1; c <= 8; c++) if (s->balls[c].active) rem++;
    s->state = (rem == 0) ? ST_WIN : ST_LOSE;
}

int pl_step(void)
{
    struct pl *s = PL;
    int moving = 0, i, j, p;
    static const int32_t px[6] = {
        TO_FP(TBL_X), TO_FP(TBL_X + TBL_W / 2), TO_FP(TBL_X + TBL_W),
        TO_FP(TBL_X), TO_FP(TBL_X + TBL_W / 2), TO_FP(TBL_X + TBL_W)
    };
    static const int32_t py[6] = {
        TO_FP(TBL_Y), TO_FP(TBL_Y), TO_FP(TBL_Y),
        TO_FP(TBL_Y + TBL_H), TO_FP(TBL_Y + TBL_H), TO_FP(TBL_Y + TBL_H)
    };
    int32_t pock_r2 = (int32_t)TO_FP(POCK_R) * TO_FP(POCK_R);
    int32_t min_x = TO_FP(TBL_X + BALL_R), max_x = TO_FP(TBL_X + TBL_W - BALL_R);
    int32_t min_y = TO_FP(TBL_Y + BALL_R), max_y = TO_FP(TBL_Y + TBL_H - BALL_R);
    int32_t min_dist = TO_FP(BALL_R * 2), min_dist2 = min_dist * min_dist;

    for (i = 0; i < MAX_BALLS; i++) {
        int pocketed = 0;
        if (!s->balls[i].active) continue;
        s->balls[i].x += s->balls[i].vx;
        s->balls[i].y += s->balls[i].vy;
        s->balls[i].vx = (s->balls[i].vx * 241) >> 8;
        s->balls[i].vy = (s->balls[i].vy * 241) >> 8;
        if (s->balls[i].vx > -18 && s->balls[i].vx < 18) s->balls[i].vx = 0;
        if (s->balls[i].vy > -18 && s->balls[i].vy < 18) s->balls[i].vy = 0;
        if (s->balls[i].vx || s->balls[i].vy) moving = 1;
        for (p = 0; p < 6; p++) {
            int32_t dx = s->balls[i].x - px[p], dy = s->balls[i].y - py[p];
            if ((dx * dx + dy * dy) <= pock_r2) { pocketed = 1; break; }
        }
        if (pocketed) {
            s->balls[i].active = 0;
            s->balls[i].vx = s->balls[i].vy = 0;
            eval_eight(s, i);
            continue;
        }
        if (s->balls[i].x < min_x) { s->balls[i].x = min_x; s->balls[i].vx = (-s->balls[i].vx * 218) >> 8; }
        else if (s->balls[i].x > max_x) { s->balls[i].x = max_x; s->balls[i].vx = (-s->balls[i].vx * 218) >> 8; }
        if (s->balls[i].y < min_y) { s->balls[i].y = min_y; s->balls[i].vy = (-s->balls[i].vy * 218) >> 8; }
        else if (s->balls[i].y > max_y) { s->balls[i].y = max_y; s->balls[i].vy = (-s->balls[i].vy * 218) >> 8; }
    }

    for (i = 0; i < MAX_BALLS; i++) {
        if (!s->balls[i].active) continue;
        for (j = i + 1; j < MAX_BALLS; j++) {
            int32_t dx, dy, d2, dist, nx, ny, overlap, rvx, rvy, vel_norm, imp;
            if (!s->balls[j].active) continue;
            dx = s->balls[j].x - s->balls[i].x;
            dy = s->balls[j].y - s->balls[i].y;
            d2 = dx * dx + dy * dy;
            if (d2 <= 0 || d2 >= min_dist2) continue;
            dist = (int32_t)isqrt((uint32_t)d2);
            if (!dist) dist = 1;
            nx = (dx << 8) / dist;
            ny = (dy << 8) / dist;
            overlap = (min_dist - dist) / 2;
            s->balls[i].x -= (nx * overlap) >> 8;
            s->balls[i].y -= (ny * overlap) >> 8;
            s->balls[j].x += (nx * overlap) >> 8;
            s->balls[j].y += (ny * overlap) >> 8;
            rvx = s->balls[j].vx - s->balls[i].vx;
            rvy = s->balls[j].vy - s->balls[i].vy;
            vel_norm = (rvx * nx + rvy * ny) >> 8;
            if (vel_norm < 0) {
                imp = (vel_norm * 235) >> 8;
                s->balls[i].vx += (imp * nx) >> 8;
                s->balls[i].vy += (imp * ny) >> 8;
                s->balls[j].vx -= (imp * nx) >> 8;
                s->balls[j].vy -= (imp * ny) >> 8;
                moving = 1;
            }
        }
    }
    for (i = 0; i < MAX_BALLS; i++) redraw_ball(i);
    return moving;
}

void pl_after_rest(void)
{
    struct pl *s = PL;
    if (!s->balls[0].active) {
        s->balls[0].x = TO_FP(TBL_X + 36);
        s->balls[0].y = TO_FP(TBL_Y + TBL_H / 2);
        s->balls[0].vx = s->balls[0].vy = 0;
        s->balls[0].active = 1;
        s->shots++;
    }
    if (s->state == ST_ROLL) s->state = ST_AIM;
}

int pl_ended(void)
{
    return PL->state == ST_WIN || PL->state == ST_LOSE;
}

void pl_show_end(void)
{
    gfx_fill(34, 58, 92, 16, GFX_BLACK);
    if (PL->state == ST_WIN) gfx_text(44, 62, "YOU WIN!", 10, 1, 0x07E0, GFX_BLACK);
    else gfx_text(38, 62, "8-BALL FOUL", 12, 1, GFX_RED, GFX_BLACK);
    gfx_wait_key();
    pl_reset();
    if (PL->aprs) PL->turn = 1;
    pl_hud();
    pl_render();
}

void pl_pack_sync(uint8_t *dst)
{
    struct pl *s = PL;
    int i, x, y;
    for (i = 0; i < MAX_BALLS; i++) {
        if (!s->balls[i].active) {
            dst[i * 2] = 0xFF;
            dst[i * 2 + 1] = 0xFF;
            continue;
        }
        x = TO_PX(s->balls[i].x);
        y = TO_PX(s->balls[i].y);
        if (x < 0) x = 0;
        if (x > 255) x = 255;
        if (y < 0) y = 0;
        if (y > 255) y = 255;
        dst[i * 2] = (uint8_t)x;
        dst[i * 2 + 1] = (uint8_t)y;
    }
}

void pl_apply_sync(const uint8_t *src)
{
    struct pl *s = PL;
    int i, rem = 0;
    for (i = 0; i < MAX_BALLS; i++) {
        uint8_t x = src[i * 2], y = src[i * 2 + 1];
        if (x == 0xFF && y == 0xFF) {
            s->balls[i].active = 0;
            s->balls[i].vx = s->balls[i].vy = 0;
            continue;
        }
        s->balls[i].x = TO_FP(x);
        s->balls[i].y = TO_FP(y);
        s->balls[i].vx = s->balls[i].vy = 0;
        s->balls[i].active = 1;
    }
    if (!s->balls[9].active) {
        for (i = 1; i <= 8; i++) if (s->balls[i].active) rem++;
        s->state = rem ? ST_LOSE : ST_WIN;
    } else s->state = ST_AIM;
}

void pl_play(void (*do_shot)(void), void (*on_tick)(void), void (*on_rest)(void))
{
    struct pl *s = PL;
    pl_hud();
    pl_render();
    s->last_tick = TICK;
    s->klast = s->kseen = (uint8_t)h9_key();
    for (;;) {
        uint32_t now = TICK;
        unsigned k = pl_key();
        if (on_tick) on_tick();
        if (k == KEY_BACK) return;
        if (!s->aprs && k == KEY_STAR) {
            gfx_text(56, 60, "PAUSED", 10, 1, COL_AIM, GFX_BLACK);
            gfx_wait_release();
            for (;;) {
                unsigned pk = pl_key();
                if (pk == KEY_STAR) break;
                if (pk == KEY_BACK) return;
                h9_delay(10);
            }
            gfx_wait_release();
            pl_hud();
            pl_render();
            continue;
        }
        if (s->state == ST_AIM && k && (!s->aprs || s->turn)) {
            int need = 0;
            if (k == KEY_4) { s->aim = (int16_t)((s->aim + 355) % 360); need = 1; }
            else if (k == KEY_6) { s->aim = (int16_t)((s->aim + 5) % 360); need = 1; }
            else if (k == KEY_1) { s->aim = (int16_t)((s->aim + 359) % 360); need = 1; }
            else if (k == KEY_3) { s->aim = (int16_t)((s->aim + 1) % 360); need = 1; }
            else if (k == KEY_2 && s->power < 11) { s->power++; pl_hud(); }
            else if (k == KEY_8 && s->power > 2) { s->power--; pl_hud(); }
            else if (k == KEY_0 || (!s->aprs && k == KEY_5)) {
                if (s->balls[0].active) do_shot();
            }
            if (need) pl_render();
            h9_delay(40);
            continue;
        }
        if (s->state == ST_ROLL && (now - s->last_tick) >= 2u) {
            s->last_tick = now;
            if (!pl_step() && s->state == ST_ROLL) {
                pl_after_rest();
                if (on_rest) on_rest();
                else if (pl_ended()) pl_show_end();
                else { pl_hud(); pl_render(); }
            }
        }
        h9_delay(10);
    }
}
