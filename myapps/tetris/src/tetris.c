#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

/* Системный таймер 100 Гц (из trk.h) */
#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* Коды кнопок TD-H9 */
enum {
    KEY_0    = 2,
    KEY_2    = 4,
    KEY_4    = 6,
    KEY_5    = 7,
    KEY_6    = 8,
    KEY_8    = 10,
    KEY_BACK = 15,
    KEY_STAR = 0x13,
    KEY_HASH = 0x14
};

/* Размеры стакана: 10x20 клеток по 6x6 пикселей = 60x120 px */
#define BOARD_W 10
#define BOARD_H 20
#define CELL_SZ 6
#define OFF_X   12
#define OFF_Y   4

/* Палитра цветов RGB565 */
static const uint16_t COLORS[8] = {
    0x0000, /* 0: Пусто (черный) */
    0x07FF, /* 1: I (голубой) */
    0xFFE0, /* 2: O (желтый) */
    0xF81F, /* 3: T (пурпурный) */
    0x07E0, /* 4: S (зеленый) */
    0xF800, /* 5: Z (красный) */
    0x001F, /* 6: J (синий) */
    0xFD20  /* 7: L (оранжевый) */
};

/* Битовые маски 7 фигур (4x4) */
static const uint16_t SHAPES[7] = {
    0x0F00, /* I */
    0x0660, /* O */
    0x0E40, /* T */
    0x06C0, /* S */
    0x0C60, /* Z */
    0x0E20, /* J */
    0x0E80  /* L */
};

static uint8_t board[BOARD_H][BOARD_W];
static uint16_t cur_shape;
static uint8_t cur_color;
static int8_t cur_x, cur_y;
static uint32_t score;
static uint32_t rng_state;

/* Подавление дребезга клавиатуры (из trk_core.c) */
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

/* Генератор случайных чисел */
static uint32_t fast_rand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Форматирование числа в строку */
static void num_to_str(char *d, uint32_t v)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) *d++ = t[--n];
    *d = 0;
}

/* Поворот фигуры 4x4 на 90 градусов по часовой стрелке */
static uint16_t rotate_mask(uint16_t m)
{
    uint16_t res = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (m & (1u << (15 - (y * 4 + x)))) {
                int nx = 3 - y;
                int ny = x;
                res |= (1u << (15 - (ny * 4 + nx)));
            }
        }
    }
    return res;
}

/* Проверка столкновений */
static int check_fit(uint16_t mask, int8_t px, int8_t py)
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (mask & (1u << (15 - (y * 4 + x)))) {
                int bx = px + x;
                int by = py + y;
                if (bx < 0 || bx >= BOARD_W || by >= BOARD_H) return 0;
                if (by >= 0 && board[by][bx] != 0) return 0;
            }
        }
    }
    return 1;
}

/* Создание новой фигуры */
static int spawn_piece(void)
{
    uint8_t idx = (uint8_t)(fast_rand() % 7u);
    cur_shape = SHAPES[idx];
    cur_color = idx + 1;
    cur_x = 3;
    cur_y = 0;
    return check_fit(cur_shape, cur_x, cur_y);
}

/* Фиксация фигуры и удаление заполненных линий */
static void lock_and_clear(void)
{
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            if (cur_shape & (1u << (15 - (y * 4 + x)))) {
                int by = cur_y + y;
                int bx = cur_x + x;
                if (by >= 0 && by < BOARD_H && bx >= 0 && bx < BOARD_W) {
                    board[by][bx] = cur_color;
                }
            }
        }
    }

    for (int y = BOARD_H - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < BOARD_W; x++) {
            if (board[y][x] == 0) { full = 0; break; }
        }
        if (full) {
            score += 100;
            for (int ty = y; ty > 0; ty--) {
                for (int x = 0; x < BOARD_W; x++) board[ty][x] = board[ty - 1][x];
            }
            for (int x = 0; x < BOARD_W; x++) board[0][x] = 0;
            y++;
        }
    }
}

/* Отрисовка стакана и счета */
static void draw_frame(void)
{
    char buf[12];
    num_to_str(buf, score);
    gfx_text(82, 24, buf, 10, 1, GFX_YELLOW, GFX_BLACK);

    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) {
            uint8_t c = board[y][x];
            int fx = x - cur_x;
            int fy = y - cur_y;
            if (fx >= 0 && fx < 4 && fy >= 0 && fy < 4) {
                if (cur_shape & (1u << (15 - (fy * 4 + fx)))) {
                    c = cur_color;
                }
            }
            gfx_fill(OFF_X + x * CELL_SZ, OFF_Y + y * CELL_SZ, CELL_SZ - 1, CELL_SZ - 1, COLORS[c]);
        }
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);

    /* Рамка стакана */
    gfx_fill(OFF_X - 2, OFF_Y - 2, BOARD_W * CELL_SZ + 4, 2, GFX_WHITE);
    gfx_fill(OFF_X - 2, OFF_Y + BOARD_H * CELL_SZ, BOARD_W * CELL_SZ + 4, 2, GFX_WHITE);
    gfx_fill(OFF_X - 2, OFF_Y, 2, BOARD_H * CELL_SZ, GFX_WHITE);
    gfx_fill(OFF_X + BOARD_W * CELL_SZ, OFF_Y, 2, BOARD_H * CELL_SZ, GFX_WHITE);

    /* Интерфейс и подсказки справа */
    gfx_text(82, 8,  "SCORE:", 10, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(82, 46, "4:LEFT", 10, 1, 0x07FF,    GFX_BLACK);
    gfx_text(82, 58, "6:RIGHT",10, 1, 0x07FF,    GFX_BLACK);
    gfx_text(82, 70, "2:ROT",  10, 1, 0x07FF,    GFX_BLACK);
    gfx_text(82, 82, "8:DOWN", 10, 1, 0x07FF,    GFX_BLACK);
    gfx_text(82, 100,"*:EXIT", 10, 1, GFX_RED,   GFX_BLACK);

    for (int y = 0; y < BOARD_H; y++)
        for (int x = 0; x < BOARD_W; x++) board[y][x] = 0;

    rng_state = TICK ^ 0xA5A5A5A5u;
    if (!rng_state) rng_state = 12345;
    score = 0;
    key_last = key_seen = h9_key();

    spawn_piece();
    draw_frame();

    uint32_t last_drop = TICK;

    for (;;) {
        uint32_t now = TICK;
        unsigned k = poll_key();
        int redraw = 0;

        /* Выход в меню рации по кнопке * или Back */
        if (k == KEY_STAR || k == KEY_BACK) {
            return 0;
        }

        if (k == KEY_4 && check_fit(cur_shape, cur_x - 1, cur_y)) {
            cur_x--; redraw = 1;
        } else if (k == KEY_6 && check_fit(cur_shape, cur_x + 1, cur_y)) {
            cur_x++; redraw = 1;
        } else if (k == KEY_2) {
            uint16_t rot = rotate_mask(cur_shape);
            static const int8_t KICK[5] = { 0, -1, 1, -2, 2 };
            for (unsigned i = 0; i < 5; i++) {
                if (check_fit(rot, (int8_t)(cur_x + KICK[i]), cur_y)) {
                    cur_x += KICK[i];
                    cur_shape = rot;
                    redraw = 1;
                    break;
                }
            }
        } else if (k == KEY_8 || k == KEY_5) {
            if (check_fit(cur_shape, cur_x, cur_y + 1)) { cur_y++; redraw = 1; }
        }

        /* Автопадение раз в 50 тиков (0.5 сек) */
        if ((now - last_drop) >= 50) {
            last_drop = now;
            if (check_fit(cur_shape, cur_x, cur_y + 1)) {
                cur_y++;
            } else {
                lock_and_clear();
                if (!spawn_piece()) {
                    gfx_text(82, 114, "GAME OVER", 12, 1, GFX_RED, GFX_BLACK);
                    gfx_wait_key();
                    return 0;
                }
            }
            redraw = 1;
        }

        if (redraw) {
            draw_frame();
        }

        /* Обязательная системная пауза 10 мс для сброса Watchdog */
        h9_delay(10);
    }
}
