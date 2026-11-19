/* SEAFIGHT vs CPU. Linked only into SEAFIGHT. */
#include "sf.h"

static int free_cell(uint8_t t) { return t == C_WATER || t == C_SHIP; }

static int cpu_turn(void)
{
    struct sf *s = SF;
    int tx = -1, ty = -1, hx = -1, hy = -1, x, y, d, guard, nx, ny;
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
            nx = hx + SF_DX[d];
            ny = hy + SF_DY[d];
            if (nx >= 0 && nx < N && ny >= 0 && ny < N && free_cell(AT(s->my, nx, ny))) {
                tx = nx;
                ty = ny;
            }
        }
    if (tx < 0) {
        for (guard = 400; tx < 0 && guard--; ) {
            int rx = (int)(sf_rnd() % N), ry = (int)(sf_rnd() % N);
            if (free_cell(AT(s->my, rx, ry))) { tx = rx; ty = ry; }
        }
        if (tx < 0) return 0;
    }
    s->lx = (int8_t)tx;
    s->ly = (int8_t)ty;
    if (AT(s->my, tx, ty) == C_SHIP) {
        AT(s->my, tx, ty) = C_HIT;
        if (s->myhp) s->myhp--;
        sf_cell(MY_X, tx, ty, C_HIT, 2);
        if (sf_sunk(s->my, s->my, tx, ty)) {
            sf_sink(s->my, MY_X, tx, ty);
            sf_status("ENEMY HIT YOU!", GFX_RED);
        } else sf_status("ENEMY HIT SHIP!", GFX_RED);
        return 1;
    }
    AT(s->my, tx, ty) = C_MISS;
    sf_cell(MY_X, tx, ty, C_MISS, 2);
    sf_status("ENEMY MISSED!", COL_SHIP);
    return 0;
}

void sf_cpu_fire(void)
{
    struct sf *s = SF;
    int x = s->cx, y = s->cy;
    s->ox = (int8_t)x;
    s->oy = (int8_t)y;
    if (AT(s->real, x, y) == C_SHIP) {
        AT(s->en, x, y) = C_HIT;
        if (s->enhp) s->enhp--;
        sf_cell(EN_X, x, y, C_HIT, 1);
        if (sf_sunk(s->real, s->en, x, y)) {
            sf_sink(s->en, EN_X, x, y);
            sf_status("ENEMY SHIP SUNK!", COL_SHIP);
        } else sf_status("DIRECT HIT! SHOOT", COL_CUR);
        return;
    }
    AT(s->en, x, y) = C_MISS;
    sf_cell(EN_X, x, y, C_MISS, 1);
    sf_status("MISS! ENEMY FIRING...", COL_MISS);
    h9_delay(400);
    while (cpu_turn()) {
        h9_delay(400);
        if (!s->myhp) break;
    }
}
