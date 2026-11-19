#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* --- RF / APRS-модем (проверено на железе) --- */
#define MYCALL        ((const volatile uint8_t *)0xC4A1)
#define RING          ((volatile uint8_t *)0x16634)
#define RING_WRITE    (*(volatile uint16_t *)0x16E34)
#define RING_READ     (*(volatile uint16_t *)0x16E36)
#define RING_MASK     2047u

#define RF_ASLEEP     (*(volatile uint8_t *)0x11B01)
#define RF_WAKE()     ((void (*)(void))0x01E5969Cu)()
#define AF_SELECT(m)  ((void (*)(unsigned))0x01E593E4u)(m)
#define RF_WRITE(r,v) ((void (*)(unsigned, unsigned))0x01E5070Au)((r), (v))
#define SPEAKER(on)   ((void (*)(unsigned))0x01E52328u)(on)
#define STOCK_KISS_TX ((void (*)(const uint8_t *, unsigned))0x01E5DBCCu)

#define SIDE_BYTE  (*(volatile uint8_t *)0x11B50)
#define ROUTE_BYTE (*(volatile uint8_t *)0x11AEC)
#define RF_SETUP(x) ((void (*)(unsigned))0x01E58E3Eu)(x)
#define RX_CH (*(const volatile uint8_t *)0xC532)
#define TX_CH (*(const volatile uint8_t *)0xC533)

/* Коды кнопок TD-H9  */
enum {
    KEY_1    = 3,
    KEY_2    = 4,
    KEY_4    = 6,
    KEY_5    = 7,
    KEY_6    = 8,
    KEY_8    = 10,
    KEY_9    = 11,
    KEY_MENU = 12,
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

/* --- APRS-состояние игры --- */
static uint8_t aprs_mode;       /* 1 = игра по радио, 0 = VS CPU */
static uint8_t my_turn;         /* 1 = наш ход, 0 = ждём входящий FIRE */
static uint8_t game_started;    /* первый FIRE получен/отправлен — порядок ходов */
static uint8_t waiting_reply;   /* ждём HIT/MISS/SUNK после своего FIRE */
static uint8_t pending_shot;    /* свой FIRE отправлен и ответ ещё не получен */
static uint8_t shot_timeout;    /* 1: исТёк таймаут ожидания ответа */
static int8_t last_fire_x, last_fire_y;
static uint32_t fire_tick;      /* TICK в момент отправки FIRE */
#define FIRE_TIMEOUT 12000u     /* тиков (100 Гц) = 120 с ~ 2 мин */
static char room_id[2] = { '0', '1' };

/* Буферы для AX.25 / KISS */
static char my_call_str[8];
static uint8_t rx_frame[128];
static uint8_t rx_len;
static uint8_t rx_esc;

/* История принятых пакетов (cmd,x,y) для защиты от повтора/эха */
static int8_t rx_hist[3][3];
static uint8_t rx_hist_idx;

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

/* --- APRS: RF-тракт --- */
static void aprs_quiet(void)
{
    if (RF_ASLEEP) RF_WAKE();
    AF_SELECT(1);
    RF_WRITE(0x30, 0xBFF5);
    SPEAKER(0);
}

static void aprs_side(unsigned side)
{
    SIDE_BYTE = (uint8_t)((SIDE_BYTE & 0x7F) | side << 7);
    ROUTE_BYTE = (uint8_t)side;
    RF_SETUP(0);
    aprs_quiet();
}

static unsigned aprs_rx_side(void) { return RX_CH == 1 ? 0 : RX_CH == 2 ? 1 : 2; }

/* Упаковка одного 7-байтового адреса AX.25. */
static void pack_addr(uint8_t *dst, const char *call, int last)
{
    int i = 0;
    for (; i < 6 && call[i] && call[i] != '-'; i++) {
        uint8_t c = (uint8_t)call[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i] = (uint8_t)(c << 1);
    }
    for (; i < 6; i++) dst[i] = (uint8_t)(' ' << 1);
    dst[6] = (uint8_t)(0x60 | (last ? 1 : 0));
}

/* Отправить игровой пакет: dst=APH9BS, src=mycall, payload ">BS<ROOM>:<CMD>,x,y[,len,h]".
 * НЕ трогаем RF-бокс сразу после передачи (проверено: срывает передачу). */
static void aprs_send_game_pkt(const char *cmd, int x, int y, int l, int h)
{
    static uint8_t frame[64], kiss[88];
    static char srccall[8];
    int p = 0, k = 0, n = 0;

    pack_addr(frame + p, "APH9BS", 0); p += 7;

    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) srccall[n] = (char)MYCALL[n];
    srccall[n] = 0;
    if (n == 0) { srccall[0] = 'N'; srccall[1] = '0'; srccall[2] = 'C';
                  srccall[3] = 'A'; srccall[4] = 'L'; srccall[5] = 'L'; srccall[6] = 0; }
    pack_addr(frame + p, srccall, 1); p += 7;

    frame[p++] = 0x03;   /* UI */
    frame[p++] = 0xF0;   /* no L3 */
    frame[p++] = '>';
    frame[p++] = 'B';
    frame[p++] = 'S';
    frame[p++] = (uint8_t)room_id[0];
    frame[p++] = (uint8_t)room_id[1];
    frame[p++] = ':';
    while (*cmd) frame[p++] = (uint8_t)*cmd++;
    if (x >= 0 && y >= 0) {
        frame[p++] = ',';
        frame[p++] = (uint8_t)('0' + x);
        frame[p++] = ',';
        frame[p++] = (uint8_t)('0' + y);
        if (l >= 0) {
            frame[p++] = ',';
            frame[p++] = (uint8_t)('0' + l);
            frame[p++] = ',';
            frame[p++] = (uint8_t)('0' + h);
        }
    }

    /* KISS: C0 00 <escaped frame> C0 */
    kiss[k++] = 0xC0; kiss[k++] = 0x00;
    for (int i = 0; i < p; i++) {
        if (frame[i] == 0xC0) { kiss[k++] = 0xDB; kiss[k++] = 0xDC; }
        else if (frame[i] == 0xDB) { kiss[k++] = 0xDB; kiss[k++] = 0xDD; }
        else kiss[k++] = frame[i];
    }
    kiss[k++] = 0xC0;

    STOCK_KISS_TX(kiss, (unsigned)k);
}

/* 1, если принятый KISS-кадр — наше собственное эхо (src == наш позывной). */
static int is_self_rx(void)
{
    for (int i = 0; i < 6; i++) {
        uint8_t c = (uint8_t)my_call_str[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (my_call_str[i] == 0 || my_call_str[i] == '-') c = ' ';
        if (rx_frame[8 + i] != (uint8_t)(c << 1)) return 0;
    }
    return 1;
}

/* 1, если пакет (cmd,x,y) уже обрабатывался (защита от повтора). */
static int is_dup_pkt(char cmd, int x, int y)
{
    for (int i = 0; i < 3; i++) {
        if (rx_hist[i][0] == (int8_t)cmd &&
            (x < 0 || (rx_hist[i][1] == (int8_t)x && rx_hist[i][2] == (int8_t)y)))
            return 1;
    }
    return 0;
}

static void remember_pkt(char cmd, int x, int y)
{
    rx_hist[rx_hist_idx][0] = (int8_t)cmd;
    rx_hist[rx_hist_idx][1] = (int8_t)x;
    rx_hist[rx_hist_idx][2] = (int8_t)y;
    rx_hist_idx = (rx_hist_idx + 1) % 3;
}

/* Разбор принятого AX.25-фрейма с проверкой комнаты (>BS<RR>:).
 * out_cmd — команда; v[0..3] — до 4 чисел; *nv — их число. Возвращает 1, если
 * принят валидный не-свой и не-повторный пакет. */
static int poll_aprs_rx(char *out_cmd, int *v, int *nv)
{
    unsigned write = RING_WRITE & RING_MASK;
    unsigned read  = RING_READ & RING_MASK;
    int got = 0;

    while (read != write) {
        uint8_t b = RING[read];
        read = (read + 1u) & RING_MASK;

        if (b == 0xC0) {
            if (rx_len > 20 && rx_frame[0] == 0x00) {
                if (!is_self_rx()) {
                    /* KISS: [0] port, [1..7] dst, [8..14] src, [15] ctrl, [16] pid, payload с 17 */
                    unsigned at = 17;
                    if (at + 8 <= rx_len &&
                        rx_frame[at] == '>' && rx_frame[at + 1] == 'B' && rx_frame[at + 2] == 'S' &&
                        rx_frame[at + 3] == (uint8_t)room_id[0] &&
                        rx_frame[at + 4] == (uint8_t)room_id[1] &&
                        rx_frame[at + 5] == ':') {
                        unsigned p = at + 6;
                        int k = 0;
                        while (p < rx_len && rx_frame[p] != ',' && k < 7) {
                            out_cmd[k++] = (char)rx_frame[p++];
                        }
                        out_cmd[k] = 0;
                        v[0] = v[1] = v[2] = v[3] = -1;
                        *nv = 0;
                        while (p < rx_len && *nv < 4) {
                            if (rx_frame[p] != ',') break;
                            p++;
                            if (p < rx_len && rx_frame[p] >= '0' && rx_frame[p] <= '9') {
                                v[(*nv)++] = rx_frame[p] - '0';
                            } else {
                                v[(*nv)++] = -1;
                            }
                            p++;
                        }
                        if (k > 0 && !is_dup_pkt(out_cmd[0], v[0], v[1])) {
                            remember_pkt(out_cmd[0], v[0], v[1]);
                            got = 1;
                        }
                    }
                }
            }
            rx_len = 0;
            rx_esc = 0;
            if (got) break;
            continue;
        }
        if (rx_esc) {
            b = (b == 0xDC) ? 0xC0 : (b == 0xDD) ? 0xDB : b;
            rx_esc = 0;
        } else if (b == 0xDB) {
            rx_esc = 1;
            continue;
        }
        if (rx_len < sizeof rx_frame) rx_frame[rx_len++] = b;
    }
    RING_READ = (uint16_t)read;
    return got;
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

/* Потоплен ли корабль игрока (все клетки связной группы C_SHIP стали C_HIT)? */
static int sunk_my_ship(int x, int y)
{
    static uint8_t vis[N][N];
    static int8_t qx[32], qy[32];
    int head = 0, tail = 0, i, j;
    if (x < 0 || x >= N || y < 0 || y >= N) return 0;
    if (my_grid[y][x] != C_HIT) return 0;
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
            if (my_grid[ny][nx] != C_WATER && !vis[ny][nx]) {
                /* сосед—часть корабля, если он C_SHIP или C_HIT ещё живого корабля */
                vis[ny][nx] = 1;
                if (tail < 32) { qx[tail] = (int8_t)nx; qy[tail] = (int8_t)ny; tail++; }
            }
        }
    }
    /* если в группе осталась хотя бы одна C_SHIP — корабль ещё не потоплен */
    for (int q = 0; q < tail; q++)
        if (my_grid[qy[q]][qx[q]] == C_SHIP) return 0;
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

/* Залить периметр потопленного корабля игрока (по my_grid) — чтобы CPU после
 * потопления не стрелял в смежные (диагональные/ортогональные) клетки. */
static void sink_my_area(int x, int y)
{
    static uint8_t vis[N][N];
    static int8_t qx[32], qy[32];
    int head = 0, tail = 0, i, j;
    for (j = 0; j < N; j++) for (i = 0; i < N; i++) vis[j][i] = 0;
    if (x < 0 || x >= N || y < 0 || y >= N) return;
    if (my_grid[y][x] != C_HIT) return;
    qx[tail] = (int8_t)x; qy[tail] = (int8_t)y; tail++;
    vis[y][x] = 1;
    while (head < tail && tail < 32) {
        int cxx = qx[head], cyy = qy[head]; head++;
        static const int8_t DX[4] = { 0, 1, 0, -1 };
        static const int8_t DY[4] = { -1, 0, 1, 0 };
        for (int d = 0; d < 4; d++) {
            int nx = cxx + DX[d], ny = cyy + DY[d];
            if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
            if (my_grid[ny][nx] == C_HIT && !vis[ny][nx]) {
                vis[ny][nx] = 1;
                if (tail < 32) { qx[tail] = (int8_t)nx; qy[tail] = (int8_t)ny; tail++; }
            }
        }
    }
    for (int q = 0; q < tail; q++) {
        int gx = qx[q], gy = qy[q];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = gx + dx, ny = gy + dy;
                if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
                if (my_grid[ny][nx] == C_WATER) {
                    my_grid[ny][nx] = C_MISS;
                    draw_cell(MY_X, (int8_t)nx, (int8_t)ny, C_MISS, 0);
                }
            }
        }
    }
}

/* Описать корабль на my_grid, содержащий (x,y): первая клетка, длина,
 * ориентация. Вызывается для потопленного (все клетки C_HIT) корабля. */
static void ship_span(int x, int y, int *sx, int *sy, int *len, int *horiz)
{
    int x0 = x, y0 = y, l = 1;
    int h = (x > 0 && my_grid[y][x - 1] == C_HIT) ||
            (x < N - 1 && my_grid[y][x + 1] == C_HIT);
    if (h) {
        while (x0 > 0 && my_grid[y][x0 - 1] == C_HIT) x0--;
        while (x0 + l < N && my_grid[y][x0 + l] == C_HIT) l++;
        *sx = x0; *sy = y;
    } else {
        while (y0 > 0 && my_grid[y0 - 1][x] == C_HIT) y0--;
        while (y0 + l < N && my_grid[y0 + l][x] == C_HIT) l++;
        *sx = x; *sy = y0;
    }
    *len = l; *horiz = h;
}

/* Приём SUNK: корабль задан первой клеткой (sx,sy), длиной len и ориентацией
 * horiz. Помечаем все клетки корабля C_HIT (идемпотентно) и заливаем периметр. */
static void sink_span_en(int sx, int sy, int len, int horiz)
{
    for (int i = 0; i < len; i++) {
        int gx = sx + (horiz ? i : 0);
        int gy = sy + (horiz ? 0 : i);
        if (en_grid[gy][gx] != C_HIT) {
            en_grid[gy][gx] = C_HIT;
            draw_cell(EN_X, (int8_t)gx, (int8_t)gy, C_HIT, gx == cx && gy == cy);
        }
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                int nx = gx + dx, ny = gy + dy;
                if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
                if (en_grid[ny][nx] == C_WATER) {
                    en_grid[ny][nx] = C_MISS;
                    draw_cell(EN_X, (int8_t)nx, (int8_t)ny, C_MISS, nx == cx && ny == cy);
                }
            }
        }
    }
}

/* Ход CPU: стреляет по нашей сетке, возвращает 1 если попал.
 * Логика:
 *  - если уже есть подбитая клетка (C_HIT), добиваем корабль по прямой вдоль
 *    найденной линии (горизонтальной или вертикальной), стреляя в следующий
 *    свободный C_WATER/C_SHIP по той же оси — исключаем побочные (смежные)
 *    клетки;
 *  - потопленный корабль заливается периметром (C_MISS), поэтому в него и его
 *    смежные CPU больше не целится;
 *  - в случайном режиме не стреляем в C_MISS (уже исключённые). */
static int cpu_turn(void)
{
    int tx = -1, ty = -1;

    /* 1) Найдём первую HIT-клетку и определим направление добивания. */
    static const int8_t DX[4] = { 0, 1, 0, -1 };
    static const int8_t DY[4] = { -1, 0, 1, 0 };
    int hx = -1, hy = -1;
    for (int y = 0; y < N && ty < 0; y++) {
        for (int x = 0; x < N; x++) {
            if (my_grid[y][x] != C_HIT) continue;
            if (hx < 0) { hx = x; hy = y; }
            /* Добивание: есть ли HIT правее/левее (горизонтальный корабль)? */
            if ((x > 0 && my_grid[y][x - 1] == C_HIT) ||
                (x < N - 1 && my_grid[y][x + 1] == C_HIT)) {
                if (x < N - 1 && my_grid[y][x + 1] == C_WATER) { tx = x + 1; ty = y; break; }
                if (x > 0 && my_grid[y][x - 1] == C_WATER)     { tx = x - 1; ty = y; break; }
            }
            /* есть HIT выше/ниже (вертикальный корабль)? */
            if ((y > 0 && my_grid[y - 1][x] == C_HIT) ||
                (y < N - 1 && my_grid[y + 1][x] == C_HIT)) {
                if (y < N - 1 && my_grid[y + 1][x] == C_WATER) { tx = x; ty = y + 1; break; }
                if (y > 0 && my_grid[y - 1][x] == C_WATER)     { tx = x; ty = y - 1; break; }
            }
        }
    }

    /* 2) Если корабль одинокий (одна HIT без линии), добиваем, стреляя в 4 соседа
     *    но НЕ в смежные отреки (уже C_MISS проверяем). */
    if (tx < 0 && hx >= 0) {
        for (int d = 0; d < 4 && tx < 0; d++) {
            int nx = hx + DX[d], ny = hy + DY[d];
            if (nx >= 0 && nx < N && ny >= 0 && ny < N &&
                (my_grid[ny][nx] == C_WATER || my_grid[ny][nx] == C_SHIP)) {
                tx = nx; ty = ny;
            }
        }
    }

    /* 3) Случайный выстрел по свободной клетке (не C_MISS, не C_HIT). */
    if (tx < 0) {
        int guard = 400;
        while (tx < 0 && guard-- > 0) {
            int rx = (int)(fast_rand() % N);
            int ry = (int)(fast_rand() % N);
            if (my_grid[ry][rx] == C_WATER || my_grid[ry][rx] == C_SHIP) {
                tx = rx; ty = ry;
            }
        }
        if (tx < 0) return 0;   /* нет свободных клеток */
    }

    if (my_grid[ty][tx] == C_SHIP) {
        my_grid[ty][tx] = C_HIT;
        if (my_hp > 0) my_hp--;
        draw_cell(MY_X, (int8_t)tx, (int8_t)ty, C_HIT, 0);
        /* если корабль игрока потоплен — заливаем периметр (исключить смежные
         * клетки для дальнейших выстрелов CPU) */
        if (sunk_my_ship(tx, ty)) {
            sink_my_area(tx, ty);
            set_status("ENEMY SANK YOUR SHIP!", GFX_RED);
        } else {
            set_status("ENEMY HIT YOUR SHIP!", GFX_RED);
        }
        return 1;
    } else {
        my_grid[ty][tx] = C_MISS;
        draw_cell(MY_X, (int8_t)tx, (int8_t)ty, C_MISS, 0);
        set_status("ENEMY MISSED! YOUR TURN", 0x07E0);
        return 0;
    }
}

/* Экран выбора комнаты (0..99). Возвращает 1, если готово начать игру. */
static int select_room(void)
{
    int pos = 0;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(30, 30, "ROOM (0-99):", 12, 1, GFX_WHITE, GFX_BLACK);

    char r_str[4];
    for (;;) {
        r_str[0] = room_id[0]; r_str[1] = room_id[1]; r_str[2] = 0;
        gfx_fill(52, 56, 56, 30, GFX_BLACK);
        gfx_text(64, 62, r_str, 4, 2, COL_CUR, GFX_BLACK);
        gfx_text(20, 102, "0-9:SET 5:OK BACK:EXIT", 22, 1, 0x8410, GFX_BLACK);

        unsigned k = poll_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_5) return 1;
        if (k >= KEY_1 && k <= KEY_9) {
            room_id[pos] = (char)('0' + (k - KEY_1 + 1));
            if (pos == 0) pos = 1;
        }
        if (k == KEY_BACK) return 0;
        h9_delay(10);
    }
}

static void init_game(void)
{
    rng_state = TICK ^ 0x5EA12345u;
    if (!rng_state) rng_state = 101;
    place_fleet(my_grid);
    if (!aprs_mode) place_fleet(en_real);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) en_grid[y][x] = C_WATER;
    cx = 4; cy = 4;
    my_hp = 20;
    en_hp = 20;
    my_turn = 1;
    game_started = 0;
    waiting_reply = 0;
    pending_shot = 0;
    shot_timeout = 0;
    last_fire_x = last_fire_y = -1;
    fire_tick = 0;
    rx_len = 0; rx_esc = 0;
    for (int h = 0; h < 3; h++) { rx_hist[h][0] = -1; rx_hist[h][1] = -1; rx_hist[h][2] = -1; }
    rx_hist_idx = 0;
}

static void draw_board(void)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(10, 12, "MY", 8, 1, 0x07E0, GFX_BLACK);
    if (aprs_mode) {
        char hdr[8] = { 'R', 'O', 'O', 'M', ' ', room_id[0], room_id[1], 0 };
        gfx_text(EN_X + 6, 12, hdr, 8, 1, 0xF81F, GFX_BLACK);
    } else {
        gfx_text(EN_X + 6, 12, "EN", 8, 1, 0x07FF, GFX_BLACK);
    }
    gfx_text(4, 117, "2468:AIM 5:FIRE #:NEW", 26, 1, 0x8410, GFX_BLACK);
    redraw_all();
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    /* MENU */
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(22, 14, "BATTLESHIP", 16, 1, 0x07FF, GFX_BLACK);
    gfx_text(14, 54, "1: PLAY VS CPU", 18, 1, 0x07E0, GFX_BLACK);
    gfx_text(14, 78, "2: APRS RADIO LINK", 18, 1, 0xF81F, GFX_BLACK);
    gfx_text(14, 102, "BACK: EXIT", 12, 1, GFX_RED, GFX_BLACK);

    key_last = key_seen = h9_key();
    for (;;) {
        unsigned k = poll_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_1) { aprs_mode = 0; break; }
        if (k == KEY_2) {
            aprs_mode = 1;
            if (!select_room()) return 0;
            break;
        }
        h9_delay(10);
    }

    if (aprs_mode) {
        aprs_side(aprs_rx_side());
        aprs_quiet();
    }

    init_game();
    draw_board();
    set_status(aprs_mode ? (my_turn ? "YOUR TURN! FIRE TO START" : "WAIT ENEMY FIRST...")
                         : "YOUR TURN: AIM & FIRE", COL_CUR);

    for (;;) {
        unsigned k = poll_key();

        if (k == KEY_BACK) {
            /* в APRS: если ждём ответ, Back отменяет выстрел (не выход) */
            if (aprs_mode && pending_shot) {
                waiting_reply = 0;
                pending_shot = 0;
                shot_timeout = 0;
                last_fire_x = last_fire_y = -1;
                my_turn = 1;
                set_status("SHOT CANCELLED", 0xC618);
                continue;
            }
            return 0;
        }

        /* Таймаут ответа (APRS): 2 мин или Back — отмена хода */
        if (aprs_mode && pending_shot && !shot_timeout &&
            (uint32_t)(TICK - fire_tick) >= FIRE_TIMEOUT) {
            shot_timeout = 1;
            set_status("NO REPLY! 5:RETRY BACK:SKIP", GFX_RED);
        }
        if (aprs_mode && shot_timeout) {
            if (k == KEY_5) {
                aprs_send_game_pkt("FIRE", last_fire_x, last_fire_y, -1, -1);
                fire_tick = TICK;
                shot_timeout = 0;
                set_status("RETRY SENT...", COL_CUR);
                continue;
            } else if (k == KEY_BACK) {
                waiting_reply = 0;
                pending_shot = 0;
                shot_timeout = 0;
                last_fire_x = last_fire_y = -1;
                my_turn = 1;
                set_status("SHOT DROPPED", 0xC618);
                continue;
            }
        }

        /* Перемешивание расстановки по # до начала боя */
        if (k == KEY_HASH && my_hp == 20 && en_hp == 20) {
            place_fleet(my_grid);
            if (!aprs_mode) place_fleet(en_real);
            redraw_all();
            set_status("FLEET REARRANGED", 0x07FF);
            continue;
        }

        /* --- Обработка входящих APRS-пакетов (п.3,4,9) --- */
        if (aprs_mode) {
            char cmd[8];
            int v[4];
            int nv = 0;
            if (poll_aprs_rx(cmd, v, &nv)) {
                int rx_x = v[0], rx_y = v[1];
                if (cmd[0] == 'F' && nv >= 2 && rx_x >= 0 && rx_x < N && rx_y >= 0 && rx_y < N) {
                    /* Первый FIRE фиксирует порядок ходов */
                    game_started = 1;
                    my_turn = 0;      /* соперник начал — он ходит */
                    if (my_grid[rx_y][rx_x] == C_SHIP) {
                        my_grid[rx_y][rx_x] = C_HIT;
                        if (my_hp > 0) my_hp--;
                        draw_cell(MY_X, (int8_t)rx_x, (int8_t)rx_y, C_HIT, 0);
                        if (sunk_my_ship(rx_x, rx_y)) {
                            sink_my_area(rx_x, rx_y);
                            int sx, sy, len, horiz;
                            ship_span(rx_x, rx_y, &sx, &sy, &len, &horiz);
                            aprs_send_game_pkt("SUNK", sx, sy, len, horiz);
                            set_status("YOUR SHIP SUNK!", GFX_RED);
                        } else {
                            aprs_send_game_pkt("HIT", rx_x, rx_y, -1, -1);
                            set_status("HIT! ENEMY SHOTS", GFX_RED);
                        }
                    } else {
                        if (my_grid[rx_y][rx_x] == C_WATER) my_grid[rx_y][rx_x] = C_MISS;
                        draw_cell(MY_X, (int8_t)rx_x, (int8_t)rx_y, C_MISS, 0);
                        aprs_send_game_pkt("MISS", rx_x, rx_y, -1, -1);
                        my_turn = 1;   /* промах — наш ход */
                        set_status("ENEMY MISS! YOUR TURN", 0x07E0);
                    }
                } else if (cmd[0] == 'H' && nv >= 2 && rx_x >= 0 && rx_x < N && rx_y >= 0 && rx_y < N) {
                    en_grid[rx_y][rx_x] = C_HIT;
                    if (en_hp > 0) en_hp--;
                    draw_cell(EN_X, (int8_t)rx_x, (int8_t)rx_y, C_HIT, (rx_x == cx && rx_y == cy));
                    waiting_reply = 0;
                    pending_shot = 0;
                    shot_timeout = 0;
                    my_turn = 1;   /* попали — стреляем ещё */
                    set_status("DIRECT HIT! AGAIN", COL_CUR);
                } else if (cmd[0] == 'S' && nv >= 4) {
                    int sx = v[0], sy = v[1], len = v[2], horiz = v[3];
                    if (len >= 1 && len <= 4 && horiz >= 0 && horiz <= 1 &&
                        sx >= 0 && sx < N && sy >= 0 && sy < N &&
                        (sx + (horiz ? len - 1 : 0)) < N && (sy + (horiz ? 0 : len - 1)) < N) {
                        int ex = sx + (horiz ? len - 1 : 0);
                        int ey = sy + (horiz ? 0 : len - 1);
                        if ((last_fire_x >= sx && last_fire_x <= ex &&
                             last_fire_y >= sy && last_fire_y <= ey)) {
                            sink_span_en(sx, sy, len, horiz);
                            waiting_reply = 0;
                            pending_shot = 0;
                            shot_timeout = 0;
                            my_turn = 1;
                            set_status("ENEMY SHIP SUNK!", 0x07E0);
                        }
                    }
                } else if (cmd[0] == 'M' && nv >= 2 && rx_x >= 0 && rx_x < N && rx_y >= 0 && rx_y < N) {
                    en_grid[rx_y][rx_x] = C_MISS;
                    draw_cell(EN_X, (int8_t)rx_x, (int8_t)rx_y, C_MISS, (rx_x == cx && rx_y == cy));
                    waiting_reply = 0;
                    pending_shot = 0;
                    shot_timeout = 0;
                    my_turn = 0;   /* промах — ждём выстрел соперника */
                    set_status("MISS! WAIT ENEMY", 0xC618);
                }
            }
        }

        /* --- Прицеливание и выстрел --- */
        if (my_hp > 0 && en_hp > 0 && k != 0 &&
            !(aprs_mode && pending_shot) &&
            !(aprs_mode && !my_turn)) {
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
                if (aprs_mode) {
                    aprs_send_game_pkt("FIRE", cx, cy, -1, -1);
                    game_started = 1;
                    waiting_reply = 1;
                    pending_shot = 1;
                    shot_timeout = 0;
                    last_fire_x = cx; last_fire_y = cy;
                    fire_tick = TICK;
                    my_turn = 0;
                    set_status("SENT! WAITING REPLY...", COL_CUR);
                } else {
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
                        h9_delay(400);
                        while (cpu_turn()) {
                            h9_delay(400);
                            if (my_hp == 0) break;
                        }
                    }
                }
            }
        }

        if (my_hp == 0 || en_hp == 0) {
            /* Отдельный экран конца игры: YOU WIN / YOU LOOSE. BACK — выход. */
            gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
            if (en_hp == 0) {
                gfx_text(42, 52, "YOU WIN", 8, 2, 0x07E0, GFX_BLACK);
            } else {
                gfx_text(38, 52, "YOU LOOSE", 8, 2, GFX_RED, GFX_BLACK);
            }
            gfx_text(48, 92, "BACK: EXIT", 10, 1, 0x8410, GFX_BLACK);
            while (h9_key() != H9_KEY_NONE) h9_delay(3);
            for (;;) {
                if (poll_key() == KEY_BACK) break;
                h9_delay(10);
            }
            return 0;
        }

        h9_delay(10);
    }
}