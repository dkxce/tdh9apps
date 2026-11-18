#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* Коды кнопок TD-H9 */
enum {
    KEY_1    = 3,
    KEY_2    = 4,
    KEY_3    = 5,
    KEY_4    = 6,
    KEY_5    = 7,
    KEY_6    = 8,
    KEY_8    = 10,
    KEY_BACK = 15,
    KEY_STAR = 0x13
};

/* Размеры стола в пикселях и фиксированной точке Q8 (1 px = 256) */
#define FP_SHIFT  8
#define TO_FP(v)  ((int32_t)(v) << FP_SHIFT)
#define TO_PX(v)  ((int)((v) >> FP_SHIFT))

#define TBL_X     8
#define TBL_Y     18
#define TBL_W     144
#define TBL_H     104
#define BALL_R    3
#define POCK_R    6
#define MAX_BALLS 10 /* 0: белый биток, 1..8: цветные, 9: черный (8-ка) */

/* Цвета RGB565 */
#define COL_CLOTH  0x03E0 /* Зеленое сукно */
#define COL_BORDER 0x8200 /* Деревянный борт */
#define COL_AIM    0xFFE0 /* Желтый прицел */

static const uint16_t BALL_COLORS[MAX_BALLS] = {
    0xFFFF, /* 0: Белый биток */
    0xFFE0, /* 1: Желтый */
    0x001F, /* 2: Синий */
    0xF800, /* 3: Красный */
    0xF81F, /* 4: Пурпурный */
    0xFD20, /* 5: Оранжевый */
    0x07E0, /* 6: Салатовый */
    0x07FF, /* 7: Голубой */
    0xC618, /* 8: Серебряный */
    0x2104  /* 9: Черный шар (8-ка) */
};

/* Целочисленный синус Q10 (шаг 10 градусов) */
static int32_t sin_deg(int a)
{
    static const uint16_t T[10] = { 0, 178, 350, 512, 658, 784, 887, 962, 1008, 1024 };
    a %= 360;
    if (a < 0) a += 360;
    int neg = (a >= 180);
    a %= 180;
    if (a > 90) a = 180 - a;
    int i = a / 10, f = a % 10;
    int32_t v = (int32_t)T[i] + (i < 9 ? ((int32_t)T[i + 1] - (int32_t)T[i]) * f / 10 : 0);
    return neg ? -v : v;
}
static int32_t cos_deg(int a) { return sin_deg(a + 90); }

/* Целочисленный квадратный корень */
static uint32_t isqrt(uint32_t n)
{
    if (!n) return 0;
    uint32_t x = n, y = x / 2u + 1u;
    while (y < x) { x = y; y = (x + n / x) / 2u; }
    return x;
}

struct ball {
    int32_t x, y;   /* Координаты в формате Q8 */
    int32_t vx, vy; /* Скорость в формате Q8 */
    int32_t ox, oy; /* Предыдущие координаты в Q8 (для инкрементальной отрисовки) */
    uint8_t active;
};

static struct ball balls[MAX_BALLS];
static int aim_angle;
static int shot_power; /* 2..11 */
static uint32_t shots_taken;
static uint8_t game_state; /* 0: прицел, 1: шары катятся, 2: победа, 3: проигрыш */

static void num_to_str(char *d, uint32_t v)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n) *d++ = t[--n];
    *d = 0;
}

/* Опрос клавиш с подавлением дребезга (сброс при отпускании) */
static unsigned key_last = 0, key_seen = 0;
static unsigned poll_key(void)
{
    unsigned k = h9_key();
    if (k != key_last) { key_last = k; return 0; }
    if (k == key_seen) return 0;
    if (k == H9_KEY_NONE) { key_seen = H9_KEY_NONE; return 0; }
    key_seen = k;
    return k;
}

static void draw_table(void)
{
    gfx_fill(TBL_X - 4, TBL_Y - 4, TBL_W + 8, TBL_H + 8, COL_BORDER);
    gfx_fill(TBL_X, TBL_Y, TBL_W, TBL_H, COL_CLOTH);

    static const int px[6] = { TBL_X, TBL_X + TBL_W / 2, TBL_X + TBL_W, TBL_X, TBL_X + TBL_W / 2, TBL_X + TBL_W };
    static const int py[6] = { TBL_Y, TBL_Y,             TBL_Y,         TBL_Y + TBL_H, TBL_Y + TBL_H, TBL_Y + TBL_H };

    for (int i = 0; i < 6; i++) {
        gfx_fill(px[i] - 4, py[i] - 4, 9, 9, GFX_BLACK);
    }
}

/* Нарисовать шар целиком (для полной отрисовки) и запомнить позицию. */
static void draw_ball(int idx)
{
    struct ball *b = &balls[idx];
    if (!b->active) return;
    int ix = TO_PX(b->x + 128);
    int iy = TO_PX(b->y + 128);
    uint16_t c = BALL_COLORS[idx];

    gfx_fill(ix - 2, iy - 3, 5, 7, c);
    gfx_fill(ix - 3, iy - 2, 7, 5, c);
    gfx_fill(ix - 1, iy - 1, 2, 2, GFX_WHITE);
    b->ox = b->x;
    b->oy = b->y;
}

/* Инкрементальное обновление кадра во время движения. */
static void redraw_ball(int idx)
{
    struct ball *b = &balls[idx];
    int ox = TO_PX(b->ox + 128);
    int oy = TO_PX(b->oy + 128);

    if (!b->active) {
        /* Шар забит — стереть его последнюю позицию сукном */
        gfx_fill(ox - 4, oy - 4, 9, 9, COL_CLOTH);
        return;
    }
    int ix = TO_PX(b->x + 128);
    int iy = TO_PX(b->y + 128);
    if (ox == ix && oy == iy) return;

    gfx_fill(ox - 4, oy - 4, 9, 9, COL_CLOTH);
    draw_ball(idx);
}

static void draw_aim_line(void)
{
    if (!balls[0].active) return;
    int32_t cs = cos_deg(aim_angle);
    int32_t sn = sin_deg(aim_angle);
    int bx = TO_PX(balls[0].x + 128);
    int by = TO_PX(balls[0].y + 128);

    for (int dist = 8; dist <= 44; dist += 6) {
        int ax = bx + (int)((dist * cs) >> 10);
        int ay = by + (int)((dist * sn) >> 10);
        if (ax <= TBL_X + 2 || ax >= TBL_X + TBL_W - 2 ||
            ay <= TBL_Y + 2 || ay >= TBL_Y + TBL_H - 2) break;
        gfx_fill(ax, ay, 2, 2, COL_AIM);
    }
}

static void draw_hud(void)
{
    char buf[12];
    gfx_fill(0, 0, H9_WIDTH, 13, GFX_BLACK);

    gfx_text(2, 3, "SHT:", 4, 1, GFX_WHITE, GFX_BLACK);
    num_to_str(buf, shots_taken);
    gfx_text(28, 3, buf, 4, 1, COL_AIM, GFX_BLACK);

    gfx_text(58, 3, "PWR:", 4, 1, GFX_WHITE, GFX_BLACK);
    gfx_fill(84, 4, 34, 6, 0x4208);
    gfx_fill(84, 4, shot_power * 3, 6, (shot_power > 8) ? GFX_RED : 0x07E0);

    gfx_text(120, 3, "*:PAU", 5, 1, COL_AIM,  GFX_BLACK);
    gfx_text(140, 3, "B:X",   3, 1, GFX_RED, GFX_BLACK);
}

static void reset_rack(void)
{
    balls[0].x = TO_FP(TBL_X + 36);
    balls[0].y = TO_FP(TBL_Y + TBL_H / 2);
    balls[0].vx = balls[0].vy = 0;
    balls[0].active = 1;

    int32_t start_x = TO_FP(TBL_X + 96);
    int32_t mid_y   = TO_FP(TBL_Y + TBL_H / 2);
    int32_t step_y  = 1850; /* ~7.22 px в Q8 */
    int32_t step_x  = 1640; /* ~6.40 px в Q8 */

    static const int order[9] = { 1, 2, 3, 4, 9, 5, 6, 7, 8 };
    int k = 0;

    for (int col = 0; col < 4; col++) {
        int count = (col < 3) ? (col + 1) : 3;
        int32_t cx = start_x + col * step_x;
        int32_t sy = mid_y - ((count - 1) * step_y) / 2;
        for (int row = 0; row < count && k < 9; row++) {
            int b = order[k++];
            balls[b].x = cx;
            balls[b].y = sy + row * step_y;
            balls[b].vx = balls[b].vy = 0;
            balls[b].active = 1;
        }
    }

    aim_angle = 0;
    shot_power = 7;
    shots_taken = 0;
    game_state = 0;
}

static int step_physics(void)
{
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
        if (!balls[i].active) continue;

        balls[i].x += balls[i].vx;
        balls[i].y += balls[i].vy;

        /* Целочисленное трение сукна (умножение на 250/256 ≈ 0.977) */
        balls[i].vx = (balls[i].vx * 250) >> 8;
        balls[i].vy = (balls[i].vy * 250) >> 8;

        if (balls[i].vx > -9 && balls[i].vx < 9) balls[i].vx = 0;
        if (balls[i].vy > -9 && balls[i].vy < 9) balls[i].vy = 0;

        if (balls[i].vx != 0 || balls[i].vy != 0) moving = 1;

        /* Проверка попадания в лузу */
        int pocketed = 0;
        for (int p = 0; p < 6; p++) {
            int32_t dx = balls[i].x - px[p];
            int32_t dy = balls[i].y - py[p];
            if ((dx * dx + dy * dy) <= pock_r2) {
                pocketed = 1;
                break;
            }
        }

        if (pocketed) {
            balls[i].active = 0;
            balls[i].vx = balls[i].vy = 0;

            if (i == 9) {
                int rem = 0;
                for (int c = 1; c <= 8; c++) if (balls[c].active) rem++;
                game_state = (rem == 0) ? 2 : 3;
            }
            continue;
        }

        /* Отскок от бортов (потеря 15% скорости -> 218/256) */
        if (balls[i].x < min_x) {
            balls[i].x = min_x;
            balls[i].vx = (-balls[i].vx * 218) >> 8;
        } else if (balls[i].x > max_x) {
            balls[i].x = max_x;
            balls[i].vx = (-balls[i].vx * 218) >> 8;
        }

        if (balls[i].y < min_y) {
            balls[i].y = min_y;
            balls[i].vy = (-balls[i].vy * 218) >> 8;
        } else if (balls[i].y > max_y) {
            balls[i].y = max_y;
            balls[i].vy = (-balls[i].vy * 218) >> 8;
        }
    }

    /* Целочисленные упругие столкновения шаров */
    int32_t min_dist  = TO_FP(BALL_R * 2);
    int32_t min_dist2 = min_dist * min_dist;

    for (int i = 0; i < MAX_BALLS; i++) {
        if (!balls[i].active) continue;
        for (int j = i + 1; j < MAX_BALLS; j++) {
            if (!balls[j].active) continue;

            int32_t dx = balls[j].x - balls[i].x;
            int32_t dy = balls[j].y - balls[i].y;
            int32_t d2 = dx * dx + dy * dy;

            if (d2 > 0 && d2 < min_dist2) {
                int32_t dist = (int32_t)isqrt((uint32_t)d2);
                if (dist == 0) dist = 1;

                /* Нормаль в формате Q8 */
                int32_t nx = (dx << 8) / dist;
                int32_t ny = (dy << 8) / dist;

                /* Разведение слипшихся шаров */
                int32_t overlap = (min_dist - dist) / 2;
                balls[i].x -= (nx * overlap) >> 8;
                balls[i].y -= (ny * overlap) >> 8;
                balls[j].x += (nx * overlap) >> 8;
                balls[j].y += (ny * overlap) >> 8;

                /* Обмен импульсами */
                int32_t rvx = balls[j].vx - balls[i].vx;
                int32_t rvy = balls[j].vy - balls[i].vy;
                int32_t vel_norm = (rvx * nx + rvy * ny) >> 8;

                if (vel_norm < 0) {
                    int32_t imp = (vel_norm * 235) >> 8;
                    balls[i].vx += (imp * nx) >> 8;
                    balls[i].vy += (imp * ny) >> 8;
                    balls[j].vx -= (imp * nx) >> 8;
                    balls[j].vy -= (imp * ny) >> 8;
                    moving = 1;
                }
            }
        }
    }

    return moving;
}

static void render_scene(void)
{
    draw_table();
    for (int i = 0; i < MAX_BALLS; i++) draw_ball(i);
    if (game_state == 0) draw_aim_line();
}

/* Инкрементальный кадр движения: обновляем только переместившиеся шары. */
static void update_sim(void)
{
    for (int i = 0; i < MAX_BALLS; i++) {
        /* Обрабатываем и активные (перерисовать), и только что забитые
         * (стереть по последней позиции). */
        redraw_ball(i);
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    reset_rack();
    draw_hud();
    render_scene();

    uint32_t last_tick = TICK;
    key_last = key_seen = h9_key();

    for (;;) {
        uint32_t now = TICK;
        unsigned k = poll_key();

        /* Выход по Back */
        if (k == KEY_BACK) return 0;

        /* Пауза по * */
        if (k == KEY_STAR) {
            gfx_text(56, 60, "PAUSED", 10, 1, COL_AIM, GFX_BLACK);
            gfx_wait_release();
            for (;;) {
                unsigned pk = poll_key();
                if (pk == KEY_STAR) break;
                if (pk == KEY_BACK) return 0;
                h9_delay(10);
            }
            gfx_wait_release();
            draw_hud();
            render_scene();
            continue;
        }

        if (game_state == 0 && k != 0) {
            int need_draw = 0;
            if (k == KEY_4)      { aim_angle = (aim_angle + 355) % 360; need_draw = 1; }
            else if (k == KEY_6) { aim_angle = (aim_angle + 5) % 360;   need_draw = 1; }
            else if (k == KEY_1) { aim_angle = (aim_angle + 359) % 360; need_draw = 1; }
            else if (k == KEY_3) { aim_angle = (aim_angle + 1) % 360;   need_draw = 1; }
            else if (k == KEY_2 && shot_power < 11) { shot_power++; draw_hud(); }
            else if (k == KEY_8 && shot_power > 2)  { shot_power--; draw_hud(); }
            else if (k == KEY_5) {
                /* Скорость удара в формате Q8 (~170 единиц на деление шкалы) */
                int32_t spd = (int32_t)shot_power * 170;
                balls[0].vx = (spd * cos_deg(aim_angle)) >> 10;
                balls[0].vy = (spd * sin_deg(aim_angle)) >> 10;
                shots_taken++;
                game_state = 1;
                draw_hud();
            }

            if (need_draw) render_scene();
            h9_delay(40);
            continue;
        }

        if (game_state == 1 && (now - last_tick) >= 2u) {
            last_tick = now;
            int still_moving = step_physics();
            update_sim();

            if (!still_moving && game_state == 1) {
                if (!balls[0].active) {
                    balls[0].x = TO_FP(TBL_X + 36);
                    balls[0].y = TO_FP(TBL_Y + TBL_H / 2);
                    balls[0].vx = balls[0].vy = 0;
                    balls[0].active = 1;
                    shots_taken++;
                    draw_hud();
                }
                game_state = 0;
                render_scene();
            }

            if (game_state == 2 || game_state == 3) {
                gfx_fill(34, 58, 92, 16, GFX_BLACK);
                if (game_state == 2) {
                    gfx_text(44, 62, "YOU WIN!", 10, 1, 0x07E0, GFX_BLACK);
                } else {
                    gfx_text(38, 62, "8-BALL FOUL", 12, 1, GFX_RED, GFX_BLACK);
                }
                gfx_wait_key();
                reset_rack();
                draw_hud();
                render_scene();
            }
        }

        h9_delay(10);
    }
}
