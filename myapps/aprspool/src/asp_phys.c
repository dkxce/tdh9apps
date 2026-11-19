/* ASPRPOOl heavy physics + rendering. Linked only into .PL RADIO (the game
 * holds it), NOT into the small APRSPOOL menu module. Pported from
 * myapps/8ballpool (working), wide 110x62 table per AprsPool design. */
#include "asp.h"

static const uint16_t BALL_COLORS[MAX_BALLS] = {
    0xFFFF, /* 0: белый биток */
    0xFFE0, /* 1: жёлтый */
    0x001F, /* 2: синий */
    0xF800, /* 3: красный */
    0xF81F, /* 4: пурпурный */
    0xFD20, /* 5: оранжевый */
    0x07E0, /* 6: салатовый */
    0x07FF, /* 7: голубой */
    0xC618, /* 8: серебряный */
    0x2104  /* 9: чёрный 8-ка */
};

static uint32_t isqrt(uint32_t n)
{
    if (!n) return 0;
    uint32_t x = n, y = x / 2u + 1u;
    while (y < x) { x = y; y = (x + n / x) / 2u; }
    return x;
}

static void num_str(char *d, uint32_t v)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n) *d++ = t[--n];
    *d = 0;
}

void asp_draw_table(void)
{
    gfx_fill(TBL_X - 4, TBL_Y - 4, TBL_W + 8, TBL_H + 8, COL_BORDER);
    gfx_fill(TBL_X, TBL_Y, TBL_W, TBL_H, COL_CLOTH);
    static const int px[6] = { TBL_X, TBL_X + TBL_W / 2, TBL_X + TBL_W,
                               TBL_X, TBL_X + TBL_W / 2, TBL_X + TBL_W };
    static const int py[6] = { TBL_Y, TBL_Y, TBL_Y,
                               TBL_Y + TBL_H, TBL_Y + TBL_H, TBL_Y + TBL_H };
    for (int i = 0; i < 6; i++)
        gfx_fill(px[i] - 4, py[i] - 4, 9, 9, GFX_BLACK);
}

void asp_ball(int idx)
{
    struct asp *a = ASP;
    if (!a->act[idx]) return;
    int ix = TO_PX(a->bx[idx] + 128);
    int iy = TO_PX(a->by[idx] + 128);
    uint16_t c = BALL_COLORS[idx];
    gfx_fill(ix - 2, iy - 3, 5, 7, c);
    gfx_fill(ix - 3, iy - 2, 7, 5, c);
    gfx_fill(ix - 1, iy - 1, 2, 2, GFX_WHITE);
    a->ox[idx] = a->bx[idx];
    a->oy[idx] = a->by[idx];
}

void asp_redraw_ball(int idx)
{
    struct asp *a = ASP;
    int ox = TO_PX(a->ox[idx] + 128);
    int oy = TO_PX(a->oy[idx] + 128);
    if (!a->act[idx]) {
        gfx_fill(ox - 4, oy - 4, 9, 9, COL_CLOTH);
        return;
    }
    int ix = TO_PX(a->bx[idx] + 128);
    int iy = TO_PX(a->by[idx] + 128);
    if (ox == ix && oy == iy) return;
    gfx_fill(ox - 4, oy - 4, 9, 9, COL_CLOTH);
    asp_ball(idx);
}

void asp_draw_aim(void)
{
    struct asp *a = ASP;
    if (!a->act[0]) return;
    int32_t cs = asp_cos(a->aim);
    int32_t sn = asp_sin(a->aim);
    int bx = TO_PX(a->bx[0] + 128);
    int by = TO_PX(a->by[0] + 128);
    for (int dist = 8; dist <= 44; dist += 6) {
        int ax = bx + (int)((dist * cs) >> 10);
        int ay = by + (int)((dist * sn) >> 10);
        if (ax <= TBL_X + 2 || ax >= TBL_X + TBL_W - 2 ||
            ay <= TBL_Y + 2 || ay >= TBL_Y + TBL_H - 2) break;
        gfx_fill(ax, ay, 2, 2, COL_AIM);
    }
}

void asp_draw_hud(void)
{
    struct asp *a = ASP;
    char buf[12];
    gfx_fill(0, 0, H9_WIDTH, 13, GFX_BLACK);
    gfx_text(2, 3, "SHT:", 4, 1, GFX_WHITE, GFX_BLACK);
    num_str(buf, a->shots);
    gfx_text(28, 3, buf, 4, 1, COL_AIM, GFX_BLACK);
    gfx_text(58, 3, "PWR:", 4, 1, GFX_WHITE, GFX_BLACK);
    gfx_fill(84, 4, 34, 6, 0x4208);
    gfx_fill(84, 4, a->power * 3, 6, (a->power > 8) ? GFX_RED : 0x07E0);
    gfx_text(120, 3, "#:NEW", 5, 1, COL_AIM, GFX_BLACK);
    gfx_text(140, 3, "B:X", 3, 1, GFX_RED, GFX_BLACK);
}

void asp_reset_rack(void)
{
    struct asp *a = ASP;
    a->bx[0] = TO_FP(TBL_X + 26);
    a->by[0] = TO_FP(TBL_Y + TBL_H / 2);
    a->vx[0] = a->vy[0] = 0;
    a->act[0] = 1;

    int32_t start_x = TO_FP(TBL_X + 82);
    int32_t mid_y   = TO_FP(TBL_Y + TBL_H / 2);
    int32_t step_y  = 1850;
    int32_t step_x  = 1640;

    static const int order[9] = { 1, 2, 3, 4, 9, 5, 6, 7, 8 };
    int k = 0;
    for (int col = 0; col < 4; col++) {
        int count = (col < 3) ? (col + 1) : 3;
        int32_t cx = start_x + col * step_x;
        int32_t sy = mid_y - ((count - 1) * step_y) / 2;
        for (int row = 0; row < count && k < 9; row++) {
            int b = order[k++];
            a->bx[b] = cx;
            a->by[b] = sy + row * step_y;
            a->vx[b] = a->vy[b] = 0;
            a->act[b] = 1;
        }
    }
    a->aim = 0;
    a->power = 7;
    a->state = ST_AIM;
}

void asp_balls_stop(void)
{
    struct asp *a = ASP;
    for (int i = 0; i < MAX_BALLS; i++) a->vx[i] = a->vy[i] = 0;
}

int asp_step(void)
{
    struct asp *a = ASP;
    int moving = 0;
    static const int32_t px[6] = {
        TO_FP(TBL_X), TO_FP(TBL_X + TBL_W / 2), TO_FP(TBL_X + TBL_W),
        TO_FP(TBL_X), TO_FP(TBL_X + TBL_W / 2), TO_FP(TBL_X + TBL_W)
    };
    static const int32_t py[6] = {
        TO_FP(TBL_Y), TO_FP(TBL_Y), TO_FP(TBL_Y),
        TO_FP(TBL_Y + TBL_H), TO_FP(TBL_Y + TBL_H), TO_FP(TBL_Y + TBL_H)
    };
    int32_t pock_r2 = (int32_t)TO_FP(POCK_R) * TO_FP(POCK_R);
    int32_t min_x = TO_FP(TBL_X + BALL_R);
    int32_t max_x = TO_FP(TBL_X + TBL_W - BALL_R);
    int32_t min_y = TO_FP(TBL_Y + BALL_R);
    int32_t max_y = TO_FP(TBL_Y + TBL_H - BALL_R);

    for (int i = 0; i < MAX_BALLS; i++) {
        if (!a->act[i]) continue;
        a->bx[i] += a->vx[i];
        a->by[i] += a->vy[i];
        a->vx[i] = (a->vx[i] * 241) >> 8;
        a->vy[i] = (a->vy[i] * 241) >> 8;
        if (a->vx[i] > -18 && a->vx[i] < 18) a->vx[i] = 0;
        if (a->vy[i] > -18 && a->vy[i] < 18) a->vy[i] = 0;
        if (a->vx[i] != 0 || a->vy[i] != 0) moving = 1;

        int pocketed = 0;
        for (int p = 0; p < 6; p++) {
            int32_t dx = a->bx[i] - px[p];
            int32_t dy = a->by[i] - py[p];
            if ((dx * dx + dy * dy) <= pock_r2) { pocketed = 1; break; }
        }
        if (pocketed) {
            a->act[i] = 0;
            a->vx[i] = a->vy[i] = 0;
            if (i == 9) {
                int rem = 0;
                for (int c = 1; c <= 8; c++) if (a->act[c]) rem++;
                a->state = (rem == 0) ? ST_WIN : ST_LOSE;
            }
            continue;
        }
        if (a->bx[i] < min_x) { a->bx[i] = min_x; a->vx[i] = (-a->vx[i] * 218) >> 8; }
        else if (a->bx[i] > max_x) { a->bx[i] = max_x; a->vx[i] = (-a->vx[i] * 218) >> 8; }
        if (a->by[i] < min_y) { a->by[i] = min_y; a->vy[i] = (-a->vy[i] * 218) >> 8; }
        else if (a->by[i] > max_y) { a->by[i] = max_y; a->vy[i] = (-a->vy[i] * 218) >> 8; }
    }

    int32_t min_dist = TO_FP(BALL_R * 2);
    int32_t min_dist2 = min_dist * min_dist;
    for (int i = 0; i < MAX_BALLS; i++) {
        if (!a->act[i]) continue;
        for (int j = i + 1; j < MAX_BALLS; j++) {
            if (!a->act[j]) continue;
            int32_t dx = a->bx[j] - a->bx[i];
            int32_t dy = a->by[j] - a->by[i];
            int32_t d2 = dx * dx + dy * dy;
            if (d2 > 0 && d2 < min_dist2) {
                int32_t dist = (int32_t)isqrt((uint32_t)d2);
                if (dist == 0) dist = 1;
                int32_t nx = (dx << 8) / dist;
                int32_t ny = (dy << 8) / dist;
                int32_t overlap = (min_dist - dist) / 2;
                a->bx[i] -= (nx * overlap) >> 8;
                a->by[i] -= (ny * overlap) >> 8;
                a->bx[j] += (nx * overlap) >> 8;
                a->by[j] += (ny * overlap) >> 8;
                int32_t rvx = a->vx[j] - a->vx[i];
                int32_t rvy = a->vy[j] - a->vy[i];
                int32_t vel_norm = (rvx * nx + rvy * ny) >> 8;
                if (vel_norm < 0) {
                    int32_t imp = (vel_norm * 235) >> 8;
                    a->vx[i] += (imp * nx) >> 8;
                    a->vy[i] += (imp * ny) >> 8;
                    a->vx[j] -= (imp * nx) >> 8;
                    a->vy[j] -= (imp * ny) >> 8;
                    moving = 1;
                }
            }
        }
    }
    return moving;
}

void asp_render(void)
{
    asp_draw_table();
    for (int i = 0; i < MAX_BALLS; i++) asp_ball(i);
    if (ASP->state == ST_AIM && ASP->myturn) asp_draw_aim();
}