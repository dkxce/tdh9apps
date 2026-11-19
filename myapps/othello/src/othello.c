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

/* Доска 8x8 клеток по 14x14 пикселей (112x112 px) */
#define N       8
#define CELL    14
#define OFF_X   4
#define OFF_Y   8

/* Состояние клетки (как в OtForm.pas: 0-пусто, 1-черные/CPU, 2-белые/Игрок) */
enum { C_EMPTY = 0, C_CPU = 1, C_PLAYER = 2 };

/* Цвета RGB565 */
#define COL_BOARD  0x03E0 /* Темно-зеленое сукно */
#define COL_LIGHT  0x05E0 /* Светло-зеленая клетка */
#define COL_BORDER 0x8410 /* Серая рамка */
#define COL_CUR    0xFFE0 /* Желтый курсор */
#define COL_HINT   0x07FF /* Голубая точка возможного хода */

/* 8 направлений проверки (заменяют 8 блоков из Test/TestPC в OtForm.pas) */
static const int8_t DX[8] = {  0,  1,  1,  1,  0, -1, -1, -1 };
static const int8_t DY[8] = { -1, -1,  0,  1,  1,  1,  0, -1 };

/* Веса клеток для умного ИИ (углы — самые ценные) */
static const int8_t WEIGHT[N][N] = {
    { 50, -10, 10,  5,  5, 10, -10, 50 },
    {-10, -20,  1,  1,  1,  1, -20,-10 },
    { 10,   1,  5,  2,  2,  5,   1, 10 },
    {  5,   1,  2,  1,  1,  2,   1,  5 },
    {  5,   1,  2,  1,  1,  2,   1,  5 },
    { 10,   1,  5,  2,  2,  5,   1, 10 },
    {-10, -20,  1,  1,  1,  1, -20,-10 },
    { 50, -10, 10,  5,  5, 10, -10, 50 }
};

static uint8_t mx[N][N];
static int8_t cx, cy;
static uint8_t game_over;
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

static void num_to_str(char *d, uint32_t v)
{
    char t[10];
    unsigned n = 0;
    do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10);
    while (n) *d++ = t[--n];
    *d = 0;
}

/* Проверка и выполнение хода (аналог Test / TestPC из OtForm.pas).
   do_flip = 0: только проверка (сколько фишек перевернется),
   do_flip = 1: реальный ход с переворотом фишек. */
static int try_move(int xa, int ya, uint8_t side, int do_flip)
{
    if (xa < 0 || xa >= N || ya < 0 || ya >= N) return 0;
    if (mx[ya][xa] != C_EMPTY) return 0;

    uint8_t opp = (side == C_PLAYER) ? C_CPU : C_PLAYER;
    int total_flipped = 0;

    for (int d = 0; d < 8; d++) {
        int x = xa + DX[d];
        int y = ya + DY[d];
        int count = 0;

        while (x >= 0 && x < N && y >= 0 && y < N && mx[y][x] == opp) {
            x += DX[d];
            y += DY[d];
            count++;
        }

        if (count > 0 && x >= 0 && x < N && y >= 0 && y < N && mx[y][x] == side) {
            total_flipped += count;
            if (do_flip) {
                int fx = xa + DX[d];
                int fy = ya + DY[d];
                for (int i = 0; i < count; i++) {
                    mx[fy][fx] = side;
                    fx += DX[d];
                    fy += DY[d];
                }
            }
        }
    }

    if (total_flipped > 0 && do_flip) {
        mx[ya][xa] = side;
    }
    return total_flipped;
}

/* Есть ли хоть один доступный ход у стороны side */
static int has_any_move(uint8_t side)
{
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            if (try_move(x, y, side, 0) > 0) return 1;
        }
    }
    return 0;
}

/* Отрисовка фишки (круг из прямоугольников) */
static void draw_disc(int px, int py, uint16_t color, uint16_t rim)
{
    gfx_fill(px + 3, py + 2, 7, 9, rim);
    gfx_fill(px + 2, py + 3, 9, 7, rim);
    gfx_fill(px + 3, py + 3, 7, 7, color);
}

/* Отрисовка одной клетки доски */
static void draw_cell(int x, int y)
{
    int px = OFF_X + x * CELL;
    int py = OFF_Y + y * CELL;
    uint16_t bg = ((x + y) & 1) ? COL_BOARD : COL_LIGHT;

    gfx_fill(px, py, CELL - 1, CELL - 1, bg);

    uint8_t v = mx[y][x];
    if (v == C_PLAYER) {
        draw_disc(px, py, GFX_WHITE, 0xC618);
    } else if (v == C_CPU) {
        draw_disc(px, py, 0x2104, GFX_BLACK);
    } else if (!game_over && try_move(x, y, C_PLAYER, 0) > 0) {
        /* Подсказка доступного хода */
        gfx_fill(px + 5, py + 5, 3, 3, COL_HINT);
    }

    /* Курсор игрока */
    if (x == cx && y == cy) {
        gfx_fill(px, py, CELL - 1, 1, COL_CUR);
        gfx_fill(px, py + CELL - 2, CELL - 1, 1, COL_CUR);
        gfx_fill(px, py, 1, CELL - 1, COL_CUR);
        gfx_fill(px + CELL - 2, py, 1, CELL - 1, COL_CUR);
    }
}

/* Перерисовка всей доски и счета (аналог Pnt из OtForm.pas) */
static void render_board(void)
{
    uint32_t p_cnt = 0, c_cnt = 0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            draw_cell(x, y);
            if (mx[y][x] == C_PLAYER) p_cnt++;
            else if (mx[y][x] == C_CPU) c_cnt++;
        }
    }

    char buf[12];
    gfx_fill(120, 22, 38, 10, GFX_BLACK);
    num_to_str(buf, p_cnt);
    gfx_text(122, 22, buf, 4, 1, GFX_WHITE, GFX_BLACK);

    gfx_fill(120, 50, 38, 10, GFX_BLACK);
    num_to_str(buf, c_cnt);
    gfx_text(122, 50, buf, 4, 1, 0x07FF, GFX_BLACK);
}

static void set_msg(const char *s, uint16_t col)
{
    gfx_fill(118, 70, 42, 24, GFX_BLACK);
    gfx_text(120, 72, s, 6, 1, col, GFX_BLACK);
}

/* Ход компьютера (аналог PCA из OtForm.pas, но с оценкой позиции) */
static void cpu_move(void)
{
    int best_x = -1, best_y = -1;
    int best_score = -9999;

    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int flipped = try_move(x, y, C_CPU, 0);
            if (flipped > 0) {
                int sc = WEIGHT[y][x] + flipped * 2 + (int)(fast_rand() & 3u);
                if (sc > best_score) {
                    best_score = sc;
                    best_x = x;
                    best_y = y;
                }
            }
        }
    }

    if (best_x >= 0) {
        try_move(best_x, best_y, C_CPU, 1);
    }
}

/* Сброс партии (аналог FormCreate / RzToolbarButton2Click из OtForm.pas) */
static void reset_game(void)
{
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) mx[y][x] = C_EMPTY;

    mx[3][3] = C_PLAYER;
    mx[4][4] = C_PLAYER;
    mx[3][4] = C_CPU;
    mx[4][3] = C_CPU;

    cx = 4; cy = 2;
    game_over = 0;
    set_msg("YOUR", 0x07E0);
    render_board();
}

static void check_end(void)
{
    uint32_t p_cnt = 0, c_cnt = 0;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            if (mx[y][x] == C_PLAYER) p_cnt++;
            else if (mx[y][x] == C_CPU) c_cnt++;
        }
    }
    game_over = 1;
    render_board();
    if (p_cnt > c_cnt)      set_msg("WIN!",  0x07E0);
    else if (c_cnt > p_cnt) set_msg("LOSE",  GFX_RED);
    else                    set_msg("DRAW",  COL_CUR);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);

    /* Рамка доски */
    gfx_fill(OFF_X - 2, OFF_Y - 2, N * CELL + 3, 2, COL_BORDER);
    gfx_fill(OFF_X - 2, OFF_Y + N * CELL - 1, N * CELL + 3, 2, COL_BORDER);
    gfx_fill(OFF_X - 2, OFF_Y, 2, N * CELL, COL_BORDER);
    gfx_fill(OFF_X + N * CELL - 1, OFF_Y, 2, N * CELL, COL_BORDER);

    /* Правая панель счета (белые vs черные) */
    gfx_text(120, 10, "YOU",  4, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(120, 38, "CPU",  4, 1, 0x07FF,    GFX_BLACK);
    gfx_text(120, 98, "#:NEW",5, 1, COL_CUR,   GFX_BLACK);
    gfx_text(120, 110,"0:PUT",5, 1, GFX_RED,   GFX_BLACK);

    rng_state = TICK ^ 0x07E11026u;
    if (!rng_state) rng_state = 777;
    key_last = key_seen = h9_key();

    reset_game();

    for (;;) {
        unsigned k = poll_key();

        if (k == KEY_BACK) return 0;
        if (k == KEY_HASH) { reset_game(); continue; }

        if (!game_over && k != 0) {
            int8_t ox = cx, oy = cy;
            if (k == KEY_2 && cy > 0)          cy--;
            else if (k == KEY_8 && cy < N - 1) cy++;
            else if (k == KEY_5 && cy < N - 1) cy++;
            else if (k == KEY_4 && cx > 0)     cx--;
            else if (k == KEY_6 && cx < N - 1) cx++;

            if (cx != ox || cy != oy) {
                draw_cell(ox, oy);
                draw_cell(cx, cy);
            }

            /* Ход игрока по кнопке 0 */
            if (k == KEY_0) {
                if (try_move(cx, cy, C_PLAYER, 1) > 0) {
                    render_board();

                    /* Цикл ходов компьютера (если у игрока нет ходов, компьютер ходит снова) */
                    for (;;) {
                        if (!has_any_move(C_CPU)) {
                            if (!has_any_move(C_PLAYER)) {
                                check_end();
                            } else {
                                set_msg("PASS", COL_CUR);
                            }
                            break;
                        }

                        set_msg("CPU..", 0x07FF);
                        h9_delay(350);
                        cpu_move();
                        render_board();

                        if (has_any_move(C_PLAYER)) {
                            set_msg("YOUR", 0x07E0);
                            break;
                        }
                        if (!has_any_move(C_CPU)) {
                            check_end();
                            break;
                        }
                        set_msg("SKIP", GFX_RED);
                        h9_delay(600);
                    }
                } else {
                    set_msg("BAD!", GFX_RED);
                }
            }
        }

        h9_delay(10);
    }
}
