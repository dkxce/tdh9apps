#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

/* Адреса APRS-модема и RF-тракта из aprs_core.c и aprs_send.c */
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

/* Переключение бокса приёма/передачи (как в aprs_core.c aprs_side): передача
 * идёт только на TX-боксе. */
#define SIDE_BYTE  (*(volatile uint8_t *)0x11B50)     /* bit 7: logical side B */
#define ROUTE_BYTE (*(volatile uint8_t *)0x11AEC)     /* Dual Watch route side */
#define RF_SETUP(x) ((void (*)(unsigned))0x01E58E3Eu)(x)
#define RX_CH (*(const volatile uint8_t *)0xC532)     /* 0 off, 1 CH-A, 2 CH-B */
#define TX_CH (*(const volatile uint8_t *)0xC533)     /* 0/1 lock, 2/3 auto, 4 A+B */
#define TX_FREQ (*(const volatile uint32_t *)0x11C9C)
#define TX_LOW  14400000u
#define TX_HIGH 14800000u

/* Коды кнопок TD-H9 */
enum {
    KEY_0    = 2,
    KEY_1    = 3,
    KEY_2    = 4,
    KEY_4    = 6,
    KEY_5    = 7,
    KEY_6    = 8,
    KEY_8    = 10,
    KEY_9    = 11,
    KEY_MENU = 12,
    KEY_BACK = 15,
    KEY_STAR = 0x13,
    KEY_HASH = 0x14
};

/* Сетка 10x10 клеток по 7x7 пикселей */
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
static uint8_t aprs_mode;
static uint8_t my_turn; /* 1 = наш ход (можно стрелять), 0 = ждём входящий FIRE */
static uint8_t waiting_reply; /* APRS: 1 пока ждём HIT/MISS после своего FIRE */
static uint8_t pending_shot;  /* APRS: 1 после своего FIRE, пока не получен/обработан ответ */
static uint8_t shot_timeout;  /* APRS: 1, если истёк таймаут ожидания ответа */
static int8_t last_fire_x, last_fire_y; /* клетка, в которую отправлен FIRE (ждёт ответа) */
static uint32_t fire_tick;    /* время отправки FIRE (для таймаута) */
#define FIRE_TIMEOUT 6000u     /* тиков (100 Гц) = 60 секунд ожидания ответа */
static char room_id[2] = { '0', '1' }; /* Двухзначный номер комнаты (00..99) */
static uint32_t rng_state;

/* Буферы для AX.25 / KISS */
static char my_call_str[8];
static uint8_t rx_frame[124];
static uint8_t rx_len;
static uint8_t rx_esc;

/* История последних обработанных APRS-пакетов (cmd, x, y) для защиты от
 * повторной доставки (ретранслятор/эфир может прислать один пакет дважды). */
static int8_t rx_hist[3][3];
static uint8_t rx_hist_idx;

/* Открыть приём APRS (шумодав открыт, динамик выключен) — как aprs_quiet(). */
static void aprs_quiet(void)
{
    if (RF_ASLEEP) RF_WAKE();
    AF_SELECT(1);
    RF_WRITE(0x30, 0xBFF5);   /* audio DAC on: squelch open */
    SPEAKER(0);
}

/* Принудительно переинициализировать RF-бокс на нужную сторону независимо
 * от состояния, сохранённого прошлым запуском модуля (BSS не обнуляется
 * между вызовами). Всегда открывает приём. */
static void aprs_side(unsigned side)
{
    SIDE_BYTE = (uint8_t)((SIDE_BYTE & 0x7F) | side << 7);
    ROUTE_BYTE = (uint8_t)side;
    RF_SETUP(0);
    aprs_quiet();
}

static unsigned aprs_rx_side(void) { return RX_CH == 1 ? 0 : RX_CH == 2 ? 1 : 2; }

/* Упаковка 7-байтового адреса AX.25 (позывной сдвинут влево на 1 бит).
 * flags bit 0 = last addr, bit 1<<7 = command. */
static void pack_ax25_addr(uint8_t *dst, const char *call, uint8_t ssid, int last)
{
    int i = 0;
    for (; i < 6 && call[i] && call[i] != '-'; i++) {
        uint8_t c = (uint8_t)call[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        dst[i] = (uint8_t)(c << 1);
    }
    for (; i < 6; i++) dst[i] = (uint8_t)(' ' << 1);
    /* 0x60 = HDLC address extension + reserved; SSID; last bit */
    dst[6] = (uint8_t)(0x60 | ((ssid & 0x0F) << 1) | (last ? 1 : 0));
}

/* Отправка APRS-пакета: собрать AX.25 UI frame, затем обернуть в KISS
 * (C0 00 ... C0, с escape C0/DB), как требует стоковый STOCK_KISS_TX. */
static void aprs_send_game_pkt(const char *cmd, int x, int y)
{
    uint8_t frame[64], kiss[80];
    int p = 0, k = 0;

    if (TX_FREQ < TX_LOW || TX_FREQ > TX_HIGH) return;   /* передача только на 2м */

    /* dst: наш сервер-адресат (не command для простоты, last=1) */
    pack_ax25_addr(frame + p, "APH9BS", 0, 1); p += 7;

    char raw_call[8];
    int n = 0;
    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) raw_call[n] = (char)MYCALL[n];
    raw_call[n] = 0;
    if (n == 0) {
        raw_call[0] = 'N'; raw_call[1] = '0'; raw_call[2] = 'C';
        raw_call[3] = 'A'; raw_call[4] = 'L'; raw_call[5] = 'L'; raw_call[6] = 0;
    }
    pack_ax25_addr(frame + p, raw_call, MYCALL[6], 1); p += 7;

    frame[p++] = 0x03;  /* UI */
    frame[p++] = 0xF0;  /* no layer 3 */

    /* Полезная нагрузка: >BS<ROOM>:<CMD>,<X>,<Y> */
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
    }

    /* KISS-кадр: C0 00 <frame с escape> C0 */
    kiss[k++] = 0xC0;
    kiss[k++] = 0x00;
    for (int i = 0; i < p; i++) {
        if (frame[i] == 0xC0) { kiss[k++] = 0xDB; kiss[k++] = 0xDC; }
        else if (frame[i] == 0xDB) { kiss[k++] = 0xDB; kiss[k++] = 0xDD; }
        else kiss[k++] = frame[i];
    }
    kiss[k++] = 0xC0;

    /* Отправка. НЕ переключаем бокс здесь: приём уже стоит на RX-боксе
     * (см. инициализацию), а мгновенный RF_SETUP/смена SIDE сразу после
     * STOCK_KISS_TX срывал передачу на полуслове (уходил только аргумент
     * и часть поля) и ронял RF-тракт в открытый шумодав. */
    STOCK_KISS_TX(kiss, (unsigned)k);
    aprs_quiet();   /* открыть приём на том же боксе */
}

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

/* 1, если корабль на своей сетке `g`, содержащий клетку (x,y), потоплен
 * (все клетки корабля стали C_HIT). BFS по горизонтали/вертикали: корабль —
 * связная группа клеток, изначально бывших C_SHIP. Если в группе ещё остался
 * C_SHIP — корабль не потоплен. */
static int ship_sunk(int8_t x, int8_t y)
{
    /* Ищем длину связной группы, в которую входит (x,y), и есть ли в ней C_SHIP.
     * BFS с маленьким стеком (корабль max длиной 4). */
    static int8_t qx[32], qy[32];
    int head = 0, tail = 0;
    int has_ship = 0;
    switch (my_grid[y][x]) {
        case C_HIT: break;
        case C_SHIP: has_ship = 1; break;
        default: return 0;
    }
    qx[tail] = x; qy[tail] = y; tail++;
    /* помечаем пройденные как C_HIT временно? нельзя портить. Используем
     * отдельную посещённость через запись в qx/qy и проверку — избежим
     * повторного добавления простым условием направления. Для корабля длиной
     * ≤4 достаточно однопроходного обхода без детальной защиты. */
    while (head < tail && tail < 32) {
        int cxx = qx[head], cyy = qy[head]; head++;
        static const int8_t DX[4] = { 0, 1, 0, -1 };
        static const int8_t DY[4] = { -1, 0, 1, 0 };
        for (int d = 0; d < 4; d++) {
            int nx = cxx + DX[d], ny = cyy + DY[d];
            if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
            uint8_t t = my_grid[ny][nx];
            if (t == C_SHIP) { has_ship = 1; }
            if (t == C_HIT || t == C_SHIP) {
                /* добавляем соседа, если он ещё не был добавлен */
                int dup = 0;
                for (int q = 0; q < tail; q++)
                    if (qx[q] == nx && qy[q] == ny) { dup = 1; break; }
                if (!dup && tail < 32) { qx[tail] = nx; qy[tail] = ny; tail++; }
            }
        }
    }
    return !has_ship;
}

/* 1, если принятый AX.25-фрейм имеет наш собственный src-адрес (своё эхо).
 * rx_frame = KISS-кадр: [0] port(0x00), [1..7] dst, [8..14] src, [15..16] ctrl/pid. */
static int is_self_rx(void)
{
    int i;
    for (i = 0; i < 6; i++) {
        uint8_t c = (uint8_t)my_call_str[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (my_call_str[i] == 0 || my_call_str[i] == '-') c = ' ';
        if (rx_frame[8 + i] != (uint8_t)(c << 1)) return 0;
    }
    return 1;
}

/* 1, если пакет (cmd,x,y) повторял уже обработанный (защита от повтора). */
static int is_dup_pkt(char cmd, int x, int y)
{
    for (int i = 0; i < 3; i++) {
        if (rx_hist[i][0] == (int8_t)cmd &&
            (x < 0 || (rx_hist[i][1] == (int8_t)x && rx_hist[i][2] == (int8_t)y)))
            return 1;
    }
    return 0;
}

/* Запомнить обработанный пакет в историю. */
static void remember_pkt(char cmd, int x, int y)
{
    rx_hist[rx_hist_idx][0] = (int8_t)cmd;
    rx_hist[rx_hist_idx][1] = (int8_t)x;
    rx_hist[rx_hist_idx][2] = (int8_t)y;
    rx_hist_idx = (rx_hist_idx + 1) % 3;
}

/* Заполнить морем клетки потопленного корабля на сетке противника (en_grid) и
 * весь его периметр (соседние клетки вокруг корабля). BFS по C_HIT-группе. */
static void sink_area_en(int x, int y)
{
    static int8_t qx[64], qy[64];
    static uint8_t vis[N][N];
    int head = 0, tail = 0, i, j;

    for (j = 0; j < N; j++)
        for (i = 0; i < N; i++) vis[j][i] = 0;

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

    /* Заливаем периметр (без диагоналей — 8 соседей) вокруг найденной группы */
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

/* 1, если корабль противника (в CPU-режиме, en_real), содержащий (x,y), потоплен:
 * все его клетки уже стали C_HIT на en_grid. BFS по en_real (оригинальные корабли). */
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
    /* все клетки группы должны быть C_HIT на en_grid */
    for (int q = 0; q < tail; q++)
        if (en_grid[qy[q]][qx[q]] != C_HIT) return 0;
    return 1;
}

/* Разбор принятого AX.25-фрейма с проверкой номера комнаты (>BS<RR>:) */
static int poll_aprs_rx(char *out_cmd, int *ox, int *oy)
{
    unsigned write = RING_WRITE & RING_MASK;
    unsigned read  = RING_READ & RING_MASK;
    int got = 0;

    while (read != write) {
        uint8_t b = RING[read];
        read = (read + 1u) & RING_MASK;

        if (b == 0xC0) {
            if (rx_len > 20 && rx_frame[0] == 0x00) {
                /* Игнорируем собственное эхо (сообщение от себя самого) */
                if (!is_self_rx()) {
                    /* KISS: [0] port, [1..7] dst, [8..14] src, [15] ctrl, [16] pid,
                     * payload с 17 (только 2 адреса, без digipeater). */
                    unsigned at = 17;

                    /* Проверяем сигнатуру >BS и совпадение 2 цифр номера комнаты */
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
                    if (p + 3 < rx_len && rx_frame[p] == ',' && rx_frame[p + 2] == ',') {
                        *ox = rx_frame[p + 1] - '0';
                        *oy = rx_frame[p + 3] - '0';
                    } else {
                        *ox = *oy = -1;
                    }
                    /* Пропускаем повторно доставленный пакет */
                    if (!is_dup_pkt(out_cmd[0], *ox, *oy)) {
                        remember_pkt(out_cmd[0], *ox, *oy);
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
        if (rx_len < 124) rx_frame[rx_len++] = b;
    }
    RING_READ = (uint16_t)read;
    return got;
}

static void cpu_turn(void)
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
    /* Случайный поиск свободной клетки — с гарантированным лимитом, чтобы
     * не уйти в бесконечный цикл, если поле почти всё помечено. */
    if (tx < 0) {
        int guard = 400;
        while (tx < 0 && guard-- > 0) {
            int rx = (int)(fast_rand() % N);
            int ry = (int)(fast_rand() % N);
            if (my_grid[ry][rx] == C_WATER || my_grid[ry][rx] == C_SHIP) {
                tx = rx; ty = ry;
            }
        }
    }
    /* Если свободных клеток нет вовсе — выходим, ничего не стреляем */
    if (tx < 0) return;

    if (my_grid[ty][tx] == C_SHIP) {
        my_grid[ty][tx] = C_HIT;
        if (my_hp > 0) my_hp--;
        draw_cell(MY_X, (int8_t)tx, (int8_t)ty, C_HIT, 0);
        set_status("ENEMY HIT YOUR SHIP!", GFX_RED);
    } else {
        my_grid[ty][tx] = C_MISS;
        draw_cell(MY_X, (int8_t)tx, (int8_t)ty, C_MISS, 0);
        set_status("ENEMY MISSED! YOUR TURN", 0x07E0);
    }
}

/* Экран ввода 2-значного номера комнаты (00..99). По умолчанию 01. */
static int select_room(void)
{
    int pos = 0;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);   /* чистый экран */
    gfx_text(14, 30, "ENTER ROOM:", 12, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(30, 52, "(00-99)", 8, 1, 0x8410, GFX_BLACK);

    for (;;) {
        char r_str[4] = { room_id[0], room_id[1], 0, 0 };
        /* Чистим область цифр И курсора (y 70..96), чтобы не оставался мусор */
        gfx_fill(56, 70, 48, 28, GFX_BLACK);
        gfx_text(64, 74, r_str, 4, 2, COL_CUR, GFX_BLACK);
        gfx_fill(64 + pos * 12, 96, 10, 2, GFX_RED);
        gfx_fill(64 + (1 - pos) * 12, 96, 10, 2, GFX_BLACK);
        gfx_text(14, 110, "0-9:SET MENU:OK BACK:EXIT", 22, 1, 0x8410, GFX_BLACK);

        unsigned k = poll_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_MENU) return 1;
        if (k >= KEY_0 && k <= KEY_9) {
            room_id[pos] = (char)('0' + (k - KEY_0));
            if (pos == 0) {
                pos = 1;
            }
            /* После ввода второй цифры просто показываем её; старт — по MENU */
        }
        h9_delay(10);
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api;
    (void)self;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(22, 14, "BATTLESHIP APRS", 16, 1, 0x07FF, GFX_BLACK);

    unsigned n = 0;
    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) my_call_str[n] = (char)MYCALL[n];
    my_call_str[n] = 0;

    gfx_text(22, 32, "MY CALL:", 8, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(74, 32, n ? my_call_str : "NONE", 8, 1, COL_CUR, GFX_BLACK);

    gfx_text(14, 54, "1: PLAY VS CPU", 18, 1, 0x07E0, GFX_BLACK);
    gfx_text(14, 70, "2: APRS RADIO LINK", 18, 1, 0xF81F, GFX_BLACK);
    gfx_text(14, 96, "BACK: EXIT", 12, 1, GFX_RED, GFX_BLACK);

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
        aprs_side(aprs_rx_side());   /* сбросить RF и слушать на RX-боксе */
        aprs_quiet();
    }

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(MY_X + 10, 12, "MY FLEET", 8, 1, 0x07E0, GFX_BLACK);

    if (aprs_mode) {
        char hdr[8] = { 'R', 'O', 'O', 'M', ' ', room_id[0], room_id[1], 0 };
        gfx_text(EN_X + 10, 12, hdr, 8, 1, 0xF81F, GFX_BLACK);
    } else {
        gfx_text(EN_X + 12, 12, "ENEMY", 8, 1, 0x07FF, GFX_BLACK);
    }

    gfx_text(4, 117, "2468:AIM 5:FIRE #:NEW *:PAU", 26, 1, 0x8410, GFX_BLACK);

    rng_state = TICK ^ 0x5EA12345u;
    if (!rng_state) rng_state = 101;
    my_turn = (uint8_t)(fast_rand() & 1u);   /* рандомный первый ход */

    place_fleet(my_grid);
    if (!aprs_mode) place_fleet(en_real);
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) en_grid[y][x] = C_WATER;

    cx = 4; cy = 4;
    my_hp = 20;
    en_hp = 20;
    waiting_reply = 0;
    pending_shot = 0;
    shot_timeout = 0;
    last_fire_x = last_fire_y = -1;
    fire_tick = 0;
    rx_len = 0; rx_esc = 0;
    for (int h = 0; h < 3; h++) { rx_hist[h][0] = -1; rx_hist[h][1] = -1; rx_hist[h][2] = -1; }
    rx_hist_idx = 0;

    redraw_all();
    set_status(aprs_mode ? (my_turn ? "YOUR TURN: AIM & FIRE!" : "WAIT FOR ENEMY FIRE...")
                         : "YOUR TURN: AIM & FIRE", COL_CUR);

    for (;;) {
        unsigned k = poll_key();

        /* Таймаут ожидания ответа: истёк — предлагаем повторить FIRE или отменить.
         * Обрабатывается ДО общего выхода по Back, чтобы Back в этом состоянии
         * означал SKIP, а не выход из игры. */
        if (aprs_mode && pending_shot && !shot_timeout &&
            (uint32_t)(TICK - fire_tick) >= FIRE_TIMEOUT) {
            shot_timeout = 1;
            set_status("NO REPLY! 5:RETRY BACK:SKIP", GFX_RED);
        }
        if (aprs_mode && shot_timeout) {
            if (k == KEY_5) {
                /* Повторная отправка выстрела в ту же клетку */
                aprs_send_game_pkt("FIRE", last_fire_x, last_fire_y);
                fire_tick = TICK;
                shot_timeout = 0;
                set_status("SENT AGAIN! WAITING REPLY...", COL_CUR);
                continue;
            } else if (k == KEY_BACK) {
                /* Отмена: снять ожидание, вернуть ход (НЕ выход из игры) */
                waiting_reply = 0;
                pending_shot = 0;
                shot_timeout = 0;
                last_fire_x = last_fire_y = -1;
                set_status("SHOT DROPPED. NEW TURN", 0xC618);
                continue;
            }
        }

        /* Выход по Back. Но в APRS-режиме, когда выстрел ожидает ответа (pending_shot),
         * первое Back отменяет ход (возврат к прицелу), второе — выходит из игры. */
        if (k == KEY_BACK) {
            if (aprs_mode && (pending_shot || shot_timeout)) {
                waiting_reply = 0;
                pending_shot = 0;
                shot_timeout = 0;
                last_fire_x = last_fire_y = -1;
                set_status("SHOT CANCELLED. AIM AGAIN", 0xC618);
                continue;
            }
            if (aprs_mode) SPEAKER(1);
            return 0;
        }

        /* Пауза по * */
        if (k == KEY_STAR) {
            set_status("PAUSED *:RESUME", COL_CUR);
            gfx_wait_release();
            for (;;) {
                unsigned pk = poll_key();
                if (pk == KEY_STAR) break;
                if (pk == KEY_BACK) { if (aprs_mode) SPEAKER(1); return 0; }
                h9_delay(10);
            }
            gfx_wait_release();
            redraw_all();
            set_status("RESUMED", 0x07E0);
            continue;
        }

        /* Перемешивание расстановки кораблей по # до начала боя */
        if (k == KEY_HASH && my_hp == 20 && en_hp == 20) {
            place_fleet(my_grid);
            if (!aprs_mode) place_fleet(en_real);
            redraw_all();
            set_status("FLEET REARRANGED", 0x07FF);
            continue;
        }

        /* Обработка реальных входящих AX.25 пакетов своей комнаты */
        if (aprs_mode) {
            char cmd[8];
            int rx_x, rx_y;
            if (poll_aprs_rx(cmd, &rx_x, &rx_y)) {
                if (cmd[0] == 'F' && rx_x >= 0 && rx_x < N && rx_y >= 0 && rx_y < N) {
                    if (my_grid[rx_y][rx_x] == C_SHIP) {
                        my_grid[rx_y][rx_x] = C_HIT;
                        if (my_hp > 0) my_hp--;
                        draw_cell(MY_X, (int8_t)rx_x, (int8_t)rx_y, C_HIT, 0);
                        if (ship_sunk(rx_x, rx_y)) {
                            set_status("RX FIRE -> SHIP SUNK!", GFX_RED);
                            aprs_send_game_pkt("HIT", rx_x, rx_y);
                            aprs_send_game_pkt("SUNK", rx_x, rx_y);
                            my_turn = 1;
                        } else {
                            set_status("RX FIRE -> TX HIT!", GFX_RED);
                            aprs_send_game_pkt("HIT", rx_x, rx_y);
                            my_turn = 1;
                        }
                    } else {
                        if (my_grid[rx_y][rx_x] == C_WATER) my_grid[rx_y][rx_x] = C_MISS;
                        draw_cell(MY_X, (int8_t)rx_x, (int8_t)rx_y, C_MISS, 0);
                        set_status("RX FIRE -> TX MISS!", 0x07E0);
                        aprs_send_game_pkt("MISS", rx_x, rx_y);
                        my_turn = 1;
                    }
                } else if (cmd[0] == 'H' && rx_x >= 0 && rx_x < N && rx_y >= 0 && rx_y < N) {
                    en_grid[rx_y][rx_x] = C_HIT;
                    if (en_hp > 0) en_hp--;
                    waiting_reply = 0;
                    pending_shot = 0;
                    shot_timeout = 0;
                    draw_cell(EN_X, (int8_t)rx_x, (int8_t)rx_y, C_HIT, (rx_x == cx && rx_y == cy));
                    set_status("APRS RX: DIRECT HIT!", COL_CUR);
                } else if (cmd[0] == 'S' && rx_x >= 0 && rx_x < N && rx_y >= 0 && rx_y < N) {
                    /* Наш выстрел потопил корабль противника: заливаем его морем. */
                    en_grid[rx_y][rx_x] = C_HIT;
                    sink_area_en(rx_x, rx_y);
                    waiting_reply = 0;
                    pending_shot = 0;
                    shot_timeout = 0;
                    set_status("APRS RX: ENEMY SHIP SUNK!", 0x07E0);
                } else if (cmd[0] == 'M' && rx_x >= 0 && rx_x < N && rx_y >= 0 && rx_y < N) {
                    en_grid[rx_y][rx_x] = C_MISS;
                    waiting_reply = 0;
                    pending_shot = 0;
                    shot_timeout = 0;
                    draw_cell(EN_X, (int8_t)rx_x, (int8_t)rx_y, C_MISS, (rx_x == cx && rx_y == cy));
                    set_status("APRS RX: MISS!", 0xC618);
                }
            }
        }

        /* Прицеливание и выстрел (в APRS только свой ход) */
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
                    if (waiting_reply && !shot_timeout) {
                        set_status("WAIT FOR REPLY...", 0xC618);
                    } else if (pending_shot && cx == last_fire_x && cy == last_fire_y) {
                        /* Уже послали FIRE в эту клетку и ещё нет ответа */
                        set_status("ALREADY SHOT THIS CELL!", 0xC618);
                    } else {
                        set_status("TX APRS FIRE...", 0x07FF);
                        aprs_send_game_pkt("FIRE", cx, cy);
                        waiting_reply = 1;
                        pending_shot = 1;
                        shot_timeout = 0;
                        last_fire_x = cx; last_fire_y = cy;
                        fire_tick = TICK;
                        my_turn = 0;   /* ход переходит противнику */
                        set_status("SENT! WAITING REPLY...", COL_CUR);
                    }
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
                        h9_delay(500);
                        cpu_turn();
                    }
                }
            }
        }

        if (my_hp == 0 || en_hp == 0) {
            set_status(en_hp == 0 ? "VICTORY! FLEET SUNK!" : "DEFEAT! YOUR FLEET SUNK",
                       en_hp == 0 ? 0x07E0 : GFX_RED);
            if (aprs_mode) SPEAKER(1);
            /* Ждём, пока всё отпустят, затем первую новую клавишу. В отличие от
             * gfx_wait_key не застревает, если палец ещё на клавише выстрела. */
            while (h9_key() != H9_KEY_NONE) h9_delay(3);
            while (h9_key() == H9_KEY_NONE) h9_delay(3);
            return 0;
        }

        h9_delay(10);
    }
}
