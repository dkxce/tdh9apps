#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* Коды кнопок TD-H9 */
enum {
    KEY_2    = 4,
    KEY_4    = 6,
    KEY_5    = 7,
    KEY_6    = 8,
    KEY_8    = 10,
    KEY_BACK = 15,
    KEY_STAR = 0x13
};

/* Поле 26x19 клеток по 6x6 пикселей (156x114 px) */
#define GRID_W  26
#define GRID_H  19
#define CELL_SZ 6
#define OFF_X   2
#define OFF_Y   12
#define MAX_LEN (GRID_W * GRID_H)

/* Направления: 0-вверх, 1-вправо, 2-вниз, 3-влево */
enum { DIR_UP = 0, DIR_RIGHT = 1, DIR_DOWN = 2, DIR_LEFT = 3 };

/* Цвета RGB565 */
#define COL_HEAD 0xFFE0 /* Желтая голова */
#define COL_BODY 0x07E0 /* Зеленое тело */
#define COL_FOOD 0xF800 /* Красное яблоко */
#define COL_WALL 0x5C1F /* Сине-фиолетовая рамка */

static uint8_t sx[MAX_LEN];
static uint8_t sy[MAX_LEN];
static uint16_t length;
static uint8_t dir, next_dir;
static uint8_t fx, fy;
static uint32_t score;
static uint32_t rng_state;

static uint32_t fast_rand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
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

static void num_to_str(char *d, uint32_t v)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) *d++ = t[--n];
    *d = 0;
}

static void draw_cell(uint8_t gx, uint8_t gy, uint16_t color)
{
    gfx_fill(OFF_X + gx * CELL_SZ, OFF_Y + gy * CELL_SZ, CELL_SZ - 1, CELL_SZ - 1, color);
}

static void draw_hud(void)
{
    char buf[12];
    gfx_fill(44, 2, 48, 8, GFX_BLACK);
    num_to_str(buf, score);
    gfx_text(44, 2, buf, 8, 1, GFX_YELLOW, GFX_BLACK);
}

/* Спавн еды на свободной клетке */
static void spawn_food(void)
{
    for (;;) {
        uint8_t nx = (uint8_t)(fast_rand() % GRID_W);
        uint8_t ny = (uint8_t)(fast_rand() % GRID_H);
        int busy = 0;
        for (uint16_t i = 0; i < length; i++) {
            if (sx[i] == nx && sy[i] == ny) { busy = 1; break; }
        }
        if (!busy) {
            fx = nx;
            fy = ny;
            draw_cell(fx, fy, COL_FOOD);
            return;
        }
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);

    /* Рамка поля */
    gfx_fill(OFF_X - 2, OFF_Y - 2, GRID_W * CELL_SZ + 3, 2, COL_WALL);
    gfx_fill(OFF_X - 2, OFF_Y + GRID_H * CELL_SZ, GRID_W * CELL_SZ + 3, 2, COL_WALL);
    gfx_fill(OFF_X - 2, OFF_Y, 2, GRID_H * CELL_SZ, COL_WALL);
    gfx_fill(OFF_X + GRID_W * CELL_SZ - 1, OFF_Y, 2, GRID_H * CELL_SZ, COL_WALL);

    /* Верхняя панель */
    gfx_text(2,   2, "SCORE:", 6, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(92,  2, "*:PAUSE", 6, 1, GFX_GREEN, GFX_BLACK);
    gfx_text(140, 2, "B:EX",   3, 1, GFX_RED,   GFX_BLACK);

    rng_state = TICK ^ 0x12345678u;
    if (!rng_state) rng_state = 42;

    /* Начальная змейка длиной 4 клетки в центре */
    length = 4;
    dir = next_dir = DIR_RIGHT;
    score = 0;
    for (uint16_t i = 0; i < length; i++) {
        sx[i] = (uint8_t)(10 - i);
        sy[i] = 9;
        draw_cell(sx[i], sy[i], i == 0 ? COL_HEAD : COL_BODY);
    }

    draw_hud();
    spawn_food();

    uint32_t last_step = TICK;
    key_last = key_seen = h9_key();

    for (;;) {
        uint32_t now = TICK;
        unsigned k = poll_key();

        /* Выход по Back */
        if (k == KEY_BACK) {
            return 0;
        }

        /* Пауза по * */
        if (k == KEY_STAR) {
            gfx_text(52, 52, "PAUSED", 10, 1, GFX_GREEN, GFX_BLACK);
            gfx_wait_release();
            for (;;) {
                unsigned pk = poll_key();
                if (pk == KEY_STAR) break;
                if (pk == KEY_BACK) return 0;
                h9_delay(10);
            }
            gfx_wait_release();
            draw_hud();
            last_step = TICK;
            continue;
        }

        /* Мгновенный отклик на поворот (с защитой от разворота на 180 градусов) */
        if (k == KEY_2 && dir != DIR_DOWN)  next_dir = DIR_UP;
        if (k == KEY_8 && dir != DIR_UP)    next_dir = DIR_DOWN;
        if (k == KEY_4 && dir != DIR_RIGHT) next_dir = DIR_LEFT;
        if (k == KEY_6 && dir != DIR_LEFT)  next_dir = DIR_RIGHT;

        /* Скорость плавно растет по мере роста змейки (от 180 мс до 60 мс на шаг) */
        uint32_t speed = (score < 1200) ? (18u - score / 100u) : 6u;

        if ((now - last_step) >= speed) {
            last_step = now;
            dir = next_dir;

            int8_t nx = (int8_t)sx[0];
            int8_t ny = (int8_t)sy[0];
            if (dir == DIR_UP)    ny--;
            if (dir == DIR_DOWN)  ny++;
            if (dir == DIR_LEFT)  nx--;
            if (dir == DIR_RIGHT) nx++;

            /* Проверка врезания в стену */
            int crash = (nx < 0 || nx >= GRID_W || ny < 0 || ny >= GRID_H);

            /* Проверка врезания в свой хвост */
            if (!crash) {
                for (uint16_t i = 0; i < length - 1; i++) {
                    if (sx[i] == nx && sy[i] == ny) { crash = 1; break; }
                }
            }

            if (crash) {
                gfx_text(52, 60, "GAME OVER", 10, 1, GFX_RED, GFX_BLACK);
                gfx_wait_key();
                return 0;
            }

            /* Перекрашиваем старую голову в цвет тела */
            draw_cell(sx[0], sy[0], COL_BODY);

            if (nx == fx && ny == fy) {
                /* Съели яблоко: увеличиваем длину */
                if (length < MAX_LEN) length++;
                score += 10;
                draw_hud();
                spawn_food();
            } else {
                /* Стираем кончик хвоста */
                draw_cell(sx[length - 1], sy[length - 1], GFX_BLACK);
            }

            /* Сдвигаем сегменты тела */
            for (uint16_t i = length - 1; i > 0; i--) {
                sx[i] = sx[i - 1];
                sy[i] = sy[i - 1];
            }
            sx[0] = (uint8_t)nx;
            sy[0] = (uint8_t)ny;
            draw_cell(sx[0], sy[0], COL_HEAD);
        }

        h9_delay(10);
    }
}
