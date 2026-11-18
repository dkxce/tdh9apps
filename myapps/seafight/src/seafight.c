#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* Коды кнопок TD-H9  */
enum {
    KEY_1    = 3,
    KEY_2    = 4,
    KEY_4    = 6,
    KEY_5    = 7,
    KEY_6    = 8,
    KEY_8    = 10,
    KEY_BACK = 15,
    KEY_HASH = 0x14
};

/* Сетка 10x10 клеток по 7x7 пикселей. Два поля: моё слева, врага справа. */
#define N       10
#define CELL    7
#define MY_X    6
#define EN_X    84
#define GRID_Y  24

enum { C_WATER = 0, C_SHIP = 1, C_MISS = 2, C_HIT = 3 };

#define COL_SEA    0x0015
#define COL_SHIP   0x07E0
#define COL_MISS   0xC618
#define COL_HIT    0xF800
#define COL_CUR    0xFFE0

static uint8_t my_grid[N][N];
static uint8_t en_grid[N][N];
static uint8_t en_real[N][N];

static int8_t cx, cy;
static uint8_t my_hp;
static uint8_t en_hp;
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

/* Отрисовка одной клетки поля (базовое поле + маркеры HIT/MISS + курсор) */
static void draw_cell(int base_x, int8_t x, int8_t y, uint8_t state, int cursor)
{
    int sx = base_x + x * CELL;
    int sy = GRID_Y + y * CELL;

    gfx_fill(sx, sy, CELL - 1, CELL - 1, (state == C_SHIP) ? COL_SHIP : COL_SEA);

    if (state == C_MISS) {
        gfx_fill(sx + 2, sy + 2, 2, 2, COL_MISS);
    } else if (state == C_HIT) {
        gfx_fill(sx + 1, sy + 1, CELL - 3, CELL - 3, COL_HIT);
        gfx_fill(sx + 2, sy + 2, 2, 2, GFX_YELLOW);
    }

    if (cursor) {
        gfx_fill(sx, sy, CELL - 1, 1, COL_CUR);
        gfx_fill(sx, sy + CELL - 2, CELL - 1, 1, COL_CUR);
        gfx_fill(sx, sy, 1, CELL - 1, COL_CUR);
        gfx_fill(sx + CELL - 2, sy, 1, CELL - 1, COL_CUR);
    }
}

/* Полная перерисовка обоих полей */
static void redraw_all(void)
{
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            draw_cell(MY_X, (int8_t)x, (int8_t)y, my_grid[y][x], 0);
            draw_cell(EN_X, (int8_t)x, (int8_t)y, en_grid[y][x], (x == cx && y == cy));
        }
    }
}

static void set_status(const char *msg, uint16_t col)
{
    gfx_fill(0, 100, H9_WIDTH, 13, GFX_BLACK);
    gfx_text(4, 102, msg, 25, 1, col, GFX_BLACK);
}

/* Проверка: можно ли поставить корабль длиной len с левым-верхним углом (x,y) */
static int can_place(uint8_t g[N][N], int x, int y, int len, int horiz)
{
    for (int i = 0; i < len; i++) {
        int nx = x + (horiz ? i : 0);
        int ny = y + (horiz ? 0 : i);
        if (nx >= N || ny >= N) return 0;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int cx2 = nx + dx, cy2 = ny + dy;
                if (cx2 >= 0 && cx2 < N && cy2 >= 0 && cy2 < N) {
                    if (g[cy2][cx2] != C_WATER) return 0;
                }
            }
        }
    }
    return 1;
}

/* Случайная расстановка стандартного флота */
static void place_fleet(uint8_t g[N][N])
{
    static const uint8_t SHIPS[10] = { 4, 3, 3, 2, 2, 2, 1, 1, 1, 1 };
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) g[y][x] = C_WATER;

    for (int s = 0; s < 10; s++) {
        int placed = 0;
        while (!placed) {
            int x = (int)(fast_rand() % N);
            int y = (int)(fast_rand() % N);
            int h = (int)(fast_rand() & 1u);
            if (can_place(g, x, y, SHIPS[s], h)) {
                for (int i = 0; i < SHIPS[s]; i++) {
                    g[y + (h ? 0 : i)][x + (h ? i : 0)] = C_SHIP;
                }
                placed = 1;
            }
        }
    }
}

/* Потоплен ли корабль противника (по исходной расстановке en_real)? */
static int sunk_cpu(int x, int y)
{
    static uint8_t vis[N][N];
    static int8_t qx[32], qy[32];
    int head = 0, tail = 0, i, j;
    if (x < 0 || x >= N || y < 0 || y >= N) return 0;
    if (en_real[y][x] != C_SHIP) return 0;
    for (j = 0; j < N; j++) for (i = 0; i < N; i++) vis[j][i] = 0;
    qx[tail] = (int8_t)x; qy[tail] = (int8_t)y; tail++;
    vis[y][x] = 1;
    while (head < tail && tail < 32) {
        int cxx = qx[head], cyy = qy[head]; head++;
        static const int8_t DX[4] = { 0, 1, 0, -1 };
        static const int8_t DY[4] = { -1, 0, 1, 0 };
        for (int d = 0; d < 4; d++) {
            int nx = cxx + DX[d], ny = cyy + DY[d];
            if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
            if (en_real[ny][nx] == C_SHIP && !vis[ny][nx]) {
                vis[ny][nx] = 1;
                if (tail < 32) { qx[tail] = (int8_t)nx; qy[tail] = (int8_t)ny; tail++; }
            }
        }
    }
    for (int q = 0; q < tail; q++)
        if (en_grid[qy[q]][qx[q]] != C_HIT) return 0;
    return 1;
}

/* Заливка потопленного корабля противника морем (периметр) */
static void sink_area_en(int x, int y)
{
    static int8_t qx[64], qy[64];
    static uint8_t vis[N][N];
    int head = 0, tail = 0, i, j;
    for (j = 0; j < N; j++) for (i = 0; i < N; i++) vis[j][i] = 0;
    if (x < 0 || x >= N || y < 0 || y >= N) return;
    if (en_grid[y][x] != C_HIT) return;
    qx[tail] = (int8_t)x; qy[tail] = (int8_t)y; tail++;
    vis[y][x] = 1;
    while (head < tail && tail < 64) {
        int cxx = qx[head], cyy = qy[head]; head++;
        static const int8_t DX[4] = { 0, 1, 0, -1 };
        static const int8_t DY[4] = { -1, 0, 1, 0 };
        for (int d = 0; d < 4; d++) {
            int nx = cxx + DX[d], ny = cyy + DY[d];
            if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
            if (en_grid[ny][nx] == C_HIT && !vis[ny][nx]) {
                vis[ny][nx] = 1;
                if (tail < 64) { qx[tail] = (int8_t)nx; qy[tail] = (int8_t)ny; tail++; }
            }
        }
    }
    for (int q = 0; q < tail; q++) {
        int gx = qx[q], gy = qy[q];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = gx + dx, ny = gy + dy;
                if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
                if (en_grid[ny][nx] == C_WATER) {
                    en_grid[ny][nx] = C_MISS;
                    draw_cell(EN_X, (int8_t)nx, (int8_t)ny,
                              (nx == cx && ny == cy) ? C_MISS : en_grid[ny][nx],
                              nx == cx && ny == cy);
                }
            }
        }
    }
}

/* Ход CPU: стреляет по нашей сетке, возвращает 1 если попал */
static int cpu_turn(void)
{
    int tx = -1, ty = -1;
    for (int y = 0; y < N && tx < 0; y++) {
        for (int x = 0; x < N; x++) {
            if (my_grid[y][x] == C_HIT) {
                static const int8_t DX[4] = { 0, 1, 0, -1 };
                static const int8_t DY[4] = { -1, 0, 1, 0 };
                for (int d = 0; d < 4; d++) {
                    int nx = x + DX[d], ny = y + DY[d];
                    if (nx >= 0 && nx < N && ny >= 0 && ny < N) {
                        if (my_grid[ny][nx] == C_WATER || my_grid[ny][nx] == C_SHIP) {
                            tx = nx; ty = ny; break;
                        }
                    }
                }
            }
        }
    }
    while (tx < 0) {
        int rx = (int)(fast_rand() % N);
        int ry = (int)(fast_rand() % N);
        if (my_grid[ry][rx] == C_WATER || my_grid[ry][rx] == C_SHIP) {
            tx = rx; ty = ry;
        }
    }

    if (my_grid[ty][tx] == C_SHIP) {
        my_grid[ty][tx] = C_HIT;
        if (my_hp > 0) my_hp--;
        draw_cell(MY_X, (int8_t)tx, (int8_t)ty, C_HIT, 0);
        set_status("ENEMY HIT YOUR SHIP!", GFX_RED);
        return 1;
    } else {
        my_grid[ty][tx] = C_MISS;
        draw_cell(MY_X, (int8_t)tx, (int8_t)ty, C_MISS, 0);
        set_status("ENEMY MISSED! YOUR TURN", 0x07E0);
        return 0;
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(22, 14, "BATTLESHIP", 16, 1, 0x07FF, GFX_BLACK);
    gfx_text(14, 54, "1: PLAY VS CPU", 18, 1, 0x07E0, GFX_BLACK);
    gfx_text(14, 78, "BACK: EXIT", 12, 1, GFX_RED, GFX_BLACK);

    key_last = key_seen = h9_key();
    for (;;) {
        unsigned k = poll_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_1) break;
        h9_delay(10);
    }

    /* Игровое поле */
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(10, 12, "MY", 8, 1, 0x07E0, GFX_BLACK);
    gfx_text(EN_X + 6, 12, "EN", 8, 1, 0x07FF, GFX_BLACK);
    gfx_text(4, 117, "2468:AIM 5:FIRE #:NEW", 26, 1, 0x8410, GFX_BLACK);

    rng_state = TICK ^ 0x5EA12345u;
    if (!rng_state) rng_state = 101;

    place_fleet(my_grid);
    place_fleet(en_real);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) en_grid[y][x] = C_WATER;

    cx = 4; cy = 4;
    my_hp = 20;
    en_hp = 20;

    redraw_all();
    set_status("YOUR TURN: AIM & FIRE", COL_CUR);

    for (;;) {
        unsigned k = poll_key();

        if (k == KEY_BACK) return 0;

        /* Перемешивание расстановки по # до начала боя */
        if (k == KEY_HASH && my_hp == 20 && en_hp == 20) {
            place_fleet(my_grid);
            place_fleet(en_real);
            redraw_all();
            set_status("FLEET REARRANGED", 0x07FF);
            continue;
        }

        if (my_hp > 0 && en_hp > 0 && k != 0) {
            int8_t ox = cx, oy = cy;
            if (k == KEY_2 && cy > 0)          cy--;
            else if (k == KEY_8 && cy < N - 1) cy++;
            else if (k == KEY_4 && cx > 0)     cx--;
            else if (k == KEY_6 && cx < N - 1) cx++;

            if (cx != ox || cy != oy) {
                draw_cell(EN_X, ox, oy, en_grid[oy][ox], 0);
                draw_cell(EN_X, cx, cy, en_grid[cy][cx], 1);
            }

            if (k == KEY_5 && en_grid[cy][cx] == C_WATER) {
                if (en_real[cy][cx] == C_SHIP) {
                    en_grid[cy][cx] = C_HIT;
                    if (en_hp > 0) en_hp--;
                    draw_cell(EN_X, cx, cy, C_HIT, 1);
                    if (sunk_cpu(cx, cy)) {
                        sink_area_en(cx, cy);
                        set_status("ENEMY SHIP SUNK!", 0x07E0);
                    } else {
                        set_status("DIRECT HIT! SHOOT AGAIN", COL_CUR);
                    }
                } else {
                    en_grid[cy][cx] = C_MISS;
                    draw_cell(EN_X, cx, cy, C_MISS, 1);
                    set_status("MISS! ENEMY FIRING...", 0xC618);
                    h9_delay(500);
                    if (cpu_turn()) {
                        set_status("ENEMY HIT YOU, AGAIN", GFX_RED);
                    }
                }
            }
        }

        if (my_hp == 0 || en_hp == 0) {
            set_status(en_hp == 0 ? "VICTORY! FLEET SUNK!" : "DEFEAT! YOUR FLEET SUNK",
                       en_hp == 0 ? 0x07E0 : GFX_RED);
            while (h9_key() != H9_KEY_NONE) h9_delay(3);
            while (h9_key() == H9_KEY_NONE) h9_delay(3);
            return 0;
        }

        h9_delay(10);
    }
}