#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

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

/* Поле 15x11 клеток по 10x10 пикселей (150x110 px) */
#define GRID_W    15
#define GRID_H    11
#define CELL_SZ   10
#define OFF_X     5
#define OFF_Y     15
#define MINES_CNT 22

/* Битовые флаги клетки */
#define F_MINE    0x10 /* В клетке мина */
#define F_OPEN    0x20 /* Клетка открыта */
#define F_FLAG    0x40 /* Поставлен флажок */
#define MASK_NUM  0x0F /* Количество мин вокруг (0..8) */

/* Цвета RGB565 */
#define COL_CLOSED 0x8410 /* Серая закрытая плитка */
#define COL_LIGHT  0xC618 /* Светлый блик плитки */
#define COL_OPEN   0x2104 /* Темно-серый фон открытой клетки */
#define COL_CURSOR 0xFFE0 /* Желтая рамка курсора */

/* Классические цвета цифр 1..8 */
static const uint16_t NUM_COLORS[9] = {
    0x0000, /* 0 */
    0x07FF, /* 1: Голубой */
    0x07E0, /* 2: Зеленый */
    0xF800, /* 3: Красный */
    0x001F, /* 4: Синий */
    0x8000, /* 5: Темно-красный */
    0x07E0, /* 6: Бирюзовый */
    0xF81F, /* 7: Пурпурный */
    0xFFFF  /* 8: Белый */
};

static uint8_t grid[GRID_H][GRID_W];
static int8_t cx, cy;
static uint8_t first_move;
static uint8_t game_over;
static uint8_t victory;
static int16_t flags_left;
static uint16_t opened_cnt;
static uint32_t rng_state;

/* Антидребезг клавиатуры */
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

static uint32_t fast_rand(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void num_to_str(char *d, int32_t v)
{
    char t[12];
    unsigned n = 0;
    if (v < 0) { *d++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 10);
    while (n) *d++ = t[--n];
    *d = 0;
}

static void draw_hud(void)
{
    char buf[12];
    gfx_fill(34, 3, 32, 9, GFX_BLACK);
    num_to_str(buf, flags_left);
    gfx_text(34, 3, buf, 4, 1, GFX_YELLOW, GFX_BLACK);
}

/* Отрисовка одной клетки с учетом курсора */
static void draw_cell(int8_t x, int8_t y)
{
    int sx = OFF_X + x * CELL_SZ;
    int sy = OFF_Y + y * CELL_SZ;
    uint8_t cell = grid[y][x];

    if (cell & F_OPEN) {
        gfx_fill(sx, sy, CELL_SZ - 1, CELL_SZ - 1, COL_OPEN);
        if (cell & F_MINE) {
            /* Взорванная или раскрытая мина */
            gfx_fill(sx + 2, sy + 2, 5, 5, GFX_RED);
            gfx_fill(sx + 4, sy + 1, 1, 7, GFX_WHITE);
            gfx_fill(sx + 1, sy + 4, 7, 1, GFX_WHITE);
        } else {
            uint8_t num = cell & MASK_NUM;
            if (num > 0) {
                char s[2] = { (char)('0' + num), 0 };
                gfx_text(sx + 2, sy + 1, s, 1, 1, NUM_COLORS[num], COL_OPEN);
            }
        }
    } else {
        /* Закрытая объемная кнопка */
        gfx_fill(sx, sy, CELL_SZ - 1, CELL_SZ - 1, COL_CLOSED);
        gfx_fill(sx, sy, CELL_SZ - 1, 1, COL_LIGHT);
        gfx_fill(sx, sy, 1, CELL_SZ - 1, COL_LIGHT);

        if (cell & F_FLAG) {
            /* Красный флажок на флагштоке */
            gfx_fill(sx + 3, sy + 2, 1, 6, GFX_WHITE);
            gfx_fill(sx + 4, sy + 2, 3, 3, GFX_RED);
        }
    }

    /* Рамка активного курсора */
    if (x == cx && y == cy) {
        gfx_fill(sx, sy, CELL_SZ - 1, 1, COL_CURSOR);
        gfx_fill(sx, sy + CELL_SZ - 2, CELL_SZ - 1, 1, COL_CURSOR);
        gfx_fill(sx, sy, 1, CELL_SZ - 1, COL_CURSOR);
        gfx_fill(sx + CELL_SZ - 2, sy, 1, CELL_SZ - 1, COL_CURSOR);
    }
}

/* Расстановка мин после первого хода (гарантия безопасного старта) */
static void place_mines(int8_t safe_x, int8_t safe_y)
{
    int placed = 0;
    while (placed < MINES_CNT) {
        int8_t mx = (int8_t)(fast_rand() % GRID_W);
        int8_t my = (int8_t)(fast_rand() % GRID_H);

        /* Не ставим мину в стартовую зону 3x3 вокруг курсора */
        if (mx >= safe_x - 1 && mx <= safe_x + 1 &&
            my >= safe_y - 1 && my <= safe_y + 1) continue;

        if (!(grid[my][mx] & F_MINE)) {
            grid[my][mx] |= F_MINE;
            placed++;
        }
    }

    /* Подсчет мин вокруг каждой клетки */
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            if (grid[y][x] & F_MINE) continue;
            uint8_t cnt = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = x + dx, ny = y + dy;
                    if (nx >= 0 && nx < GRID_W && ny >= 0 && ny < GRID_H) {
                        if (grid[ny][nx] & F_MINE) cnt++;
                    }
                }
            }
            grid[y][x] |= cnt;
        }
    }
}

/* Раскрытие пустых областей (волновой алгоритм). Стеки вынесены в static,
 * чтобы не давить на маленький стек задачи TD-H9. */
static uint8_t rev_stack[2 * GRID_W * GRID_H];
static void reveal_cell(int8_t start_x, int8_t start_y)
{
    uint8_t *stack_x = rev_stack;
    uint8_t *stack_y = rev_stack + GRID_W * GRID_H;
    int sp = 0;

    stack_x[sp] = (uint8_t)start_x;
    stack_y[sp] = (uint8_t)start_y;
    sp++;

    while (sp > 0) {
        sp--;
        int8_t x = (int8_t)stack_x[sp];
        int8_t y = (int8_t)stack_y[sp];

        if (grid[y][x] & (F_OPEN | F_FLAG)) continue;

        grid[y][x] |= F_OPEN;
        opened_cnt++;
        draw_cell(x, y);

        /* Если вокруг 0 мин — добавляем всех закрытых соседей в очередь */
        if ((grid[y][x] & MASK_NUM) == 0 && !(grid[y][x] & F_MINE)) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    int8_t nx = x + (int8_t)dx;
                    int8_t ny = y + (int8_t)dy;
                    if (nx >= 0 && nx < GRID_W && ny >= 0 && ny < GRID_H) {
                        if (!(grid[ny][nx] & (F_OPEN | F_FLAG)) && sp < (GRID_W * GRID_H)) {
                            stack_x[sp] = (uint8_t)nx;
                            stack_y[sp] = (uint8_t)ny;
                            sp++;
                        }
                    }
                }
            }
        }
    }
}

/* Инициализация новой партии */
static void new_game(void)
{
    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            grid[y][x] = 0;
        }
    }
    cx = GRID_W / 2;
    cy = GRID_H / 2;
    first_move = 1;
    game_over = 0;
    victory = 0;
    flags_left = MINES_CNT;
    opened_cnt = 0;

    for (int y = 0; y < GRID_H; y++) {
        for (int x = 0; x < GRID_W; x++) {
            draw_cell((int8_t)x, (int8_t)y);
        }
    }
    draw_hud();
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);

    /* Верхняя панель управления */
    gfx_text(2,   3, "MIN:",   4, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(60,  3, "0:OPN",  5, 1, 0x07FF,    GFX_BLACK);
    gfx_text(94,  3, "*:FLG",  5, 1, GFX_YELLOW,GFX_BLACK);
    gfx_text(122, 3, "#:NEW",  5, 1, 0x07E0,    GFX_BLACK);
    gfx_text(140, 3, "B:X",    3, 1, GFX_RED,   GFX_BLACK);

    rng_state = TICK ^ 0x5AFE1234u;
    if (!rng_state) rng_state = 999;
    key_last = key_seen = h9_key();

    new_game();

    for (;;) {
        unsigned k = poll_key();

        /* Выход в меню по Back */
        if (k == KEY_BACK) return 0;

        /* Перезапуск по # в любой момент */
        if (k == KEY_HASH) {
            gfx_fill(34, 56, 92, 16, GFX_BLACK);
            new_game();
            continue;
        }

        if (!game_over && !victory && k != 0) {
            int8_t old_x = cx, old_y = cy;

            /* Перемещение курсора */
            if (k == KEY_2 && cy > 0)                 cy--;
            else if ((k == KEY_8 || k == KEY_5) && cy < GRID_H - 1) cy++;
            else if (k == KEY_4 && cx > 0)            cx--;
            else if (k == KEY_6 && cx < GRID_W - 1)   cx++;

            if (cx != old_x || cy != old_y) {
                draw_cell(old_x, old_y);
                draw_cell(cx, cy);
            }

            /* Установка или снятие флажка по кнопке * */
            if (k == KEY_STAR && !(grid[cy][cx] & F_OPEN)) {
                if (grid[cy][cx] & F_FLAG) {
                    grid[cy][cx] &= (uint8_t)~F_FLAG;
                    flags_left++;
                } else {
                    grid[cy][cx] |= F_FLAG;
                    flags_left--;
                }
                draw_cell(cx, cy);
                draw_hud();
            }

            /* Открытие клетки по кнопке 0 */
            if (k == KEY_0 && !(grid[cy][cx] & (F_OPEN | F_FLAG))) {
                if (first_move) {
                    first_move = 0;
                    place_mines(cx, cy);
                }

                if (grid[cy][cx] & F_MINE) {
                    /* Подорвались на мине — показываем все мины */
                    game_over = 1;
                    for (int y = 0; y < GRID_H; y++) {
                        for (int x = 0; x < GRID_W; x++) {
                            if (grid[y][x] & F_MINE) {
                                grid[y][x] |= F_OPEN;
                                draw_cell((int8_t)x, (int8_t)y);
                            }
                        }
                    }
                    gfx_fill(36, 56, 88, 16, GFX_BLACK);
                    gfx_text(44, 60, "BOOM! #:NEW", 11, 1, GFX_RED, GFX_BLACK);
                } else {
                    reveal_cell(cx, cy);
                    /* Проверка победы: открыты все клетки без мин */
                    if (opened_cnt == (GRID_W * GRID_H - MINES_CNT)) {
                        victory = 1;
                        gfx_fill(36, 56, 88, 16, GFX_BLACK);
                        gfx_text(46, 60, "WIN! #:NEW", 10, 1, 0x07E0, GFX_BLACK);
                    }
                }
            }
        }

        h9_delay(10);
    }
}
