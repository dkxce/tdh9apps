/* SEAFIGHT APRS link. Linked only into .SF RADIO. */
#include "sf.h"

#define MYCALL ((const volatile uint8_t *)0xC4A1)
#define RING ((volatile uint8_t *)0x16634)
#define RING_WRITE (*(volatile uint16_t *)0x16E34)
#define RING_READ (*(volatile uint16_t *)0x16E36)
#define RING_MASK 2047u
#define RF_ASLEEP (*(volatile uint8_t *)0x11B01)
#define RF_WAKE() ((void (*)(void))0x01E5969Cu)()
#define AF_SELECT(m) ((void (*)(unsigned))0x01E593E4u)(m)
#define RF_WRITE(r, v) ((void (*)(unsigned, unsigned))0x01E5070Au)((r), (v))
#define SPEAKER(on) ((void (*)(unsigned))0x01E52328u)(on)
#define STOCK_KISS_TX ((void (*)(const uint8_t *, unsigned))0x01E5DBCCu)
#define SIDE_BYTE (*(volatile uint8_t *)0x11B50)
#define ROUTE_BYTE (*(volatile uint8_t *)0x11AEC)
#define RF_SETUP(x) ((void (*)(unsigned))0x01E58E3Eu)(x)
#define RX_CH (*(const volatile uint8_t *)0xC532)

static uint8_t sf_kiss[40];
static uint8_t sf_rx[80];
static uint8_t sf_rxn, pcmd[2], pnv[2], pgot[2];
static int8_t pv[2][4];

static void aprs_quiet(void)
{
    if (RF_ASLEEP) RF_WAKE();
    AF_SELECT(1);
    RF_WRITE(0x30, 0xBFF5);
    SPEAKER(0);
}

static void pack_addr(uint8_t *d, const char *call, int last)
{
    int i = 0;
    for (; i < 6 && call[i] && call[i] != '-'; i++) {
        uint8_t c = (uint8_t)call[i];
        if (c >= 'a') c -= 32;
        d[i] = (uint8_t)(c << 1);
    }
    for (; i < 6; i++) d[i] = (uint8_t)(' ' << 1);
    d[6] = (uint8_t)(0x60 | last);
}

void sf_load_call(void)
{
    struct sf *s = SF;
    int n = 0;
    const char *p = "N0CALL";
    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) s->call[n] = (char)MYCALL[n];
    if (!n) for (; n < 6; n++) s->call[n] = p[n];
    s->call[n] = 0;
}

static void take_frame(void)
{
    struct sf *s = SF;
    unsigned i, p;
    int slot, n;
    char c;
    if (sf_rxn < 8) return;
    for (i = 0; i + 8 <= sf_rxn; i++)
        if (sf_rx[i] == '>' && sf_rx[i + 1] == 'B' && sf_rx[i + 2] == 'S' &&
            sf_rx[i + 3] == (uint8_t)s->room[0] && sf_rx[i + 4] == (uint8_t)s->room[1] &&
            sf_rx[i + 5] == ':')
            break;
    if (i + 8 > sf_rxn) return;
    p = i + 6;
    c = (char)sf_rx[p];
    while (p < sf_rxn && sf_rx[p] >= 'A' && sf_rx[p] <= 'Z') p++;
    if (c == 'F') {
        slot = 0;
        if (s->pending && p + 3 < sf_rxn) {
            int x = sf_rx[p + 1] - '0', y = sf_rx[p + 3] - '0';
            if (x == s->fx && y == s->fy) return;
        }
    } else if (c == 'H' || c == 'M' || c == 'S') slot = 1;
    else return;
    pcmd[slot] = (uint8_t)c;
    for (n = 0; n < 4; n++) pv[slot][n] = -1;
    n = 0;
    while (p < sf_rxn && n < 4 && (sf_rx[p] == ' ' || sf_rx[p] == ',')) {
        p++;
        pv[slot][n++] = (p < sf_rxn && sf_rx[p] >= '0' && sf_rx[p] <= '9') ? (int8_t)(sf_rx[p] - '0') : -1;
        p++;
    }
    pnv[slot] = (uint8_t)n;
    pgot[slot] = 1;
}

static void drain_ring(void)
{
    unsigned write = RING_WRITE & RING_MASK, read = RING_READ & RING_MASK;
    while (read != write) {
        uint8_t b = RING[read];
        read = (read + 1u) & RING_MASK;
        if (b == 0xC0) {
            take_frame();
            sf_rxn = 0;
        } else if (sf_rxn < sizeof sf_rx)
            sf_rx[sf_rxn++] = b;
    }
    RING_READ = (uint16_t)read;
}

static void wait_ms(unsigned ms)
{
    while (ms) {
        unsigned d = ms > 20 ? 20 : ms;
        drain_ring();
        h9_delay(d);
        ms -= d;
    }
}

static void send_pkt(int cmd, int x, int y, int l, int h)
{
    struct sf *s = SF;
    uint8_t *f = sf_kiss + 2;
    int p = 0, i, n, a[4];
    if (cmd != 'F') wait_ms(2000);
    pack_addr(f + p, "DKXCE", 0); p += 7;
    pack_addr(f + p, s->call, 1); p += 7;
    f[p++] = 0x03;
    f[p++] = 0xF0;
    f[p++] = '>';
    f[p++] = 'B';
    f[p++] = 'S';
    f[p++] = (uint8_t)s->room[0];
    f[p++] = (uint8_t)s->room[1];
    f[p++] = ':';
    f[p++] = (uint8_t)cmd;
    n = 0;
    if (x >= 0) {
        a[n++] = x;
        a[n++] = y;
        if (l >= 0) {
            a[n++] = l;
            a[n++] = h;
        }
    }
    for (i = 0; i < n; i++) {
        f[p++] = ' ';
        f[p++] = (uint8_t)('0' + a[i]);
    }
    sf_kiss[0] = 0xC0;
    sf_kiss[1] = 0x00;
    sf_kiss[2 + p] = 0xC0;
    STOCK_KISS_TX(sf_kiss, (unsigned)(p + 3));
    h9_delay(200);
    aprs_quiet();
    drain_ring();
    sf_board();
}

int sf_select_room(void)
{
    struct sf *s = SF;
    int pos = 2;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(30, 30, "ROOM (0-99):", 12, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(4, 102, "0-9 DIGIT *:DEL MENU OK", 24, 1, GFX_GREY, GFX_BLACK);
    for (;;) {
        char r[3];
        unsigned k;
        r[0] = s->room[0];
        r[1] = s->room[1];
        r[2] = 0;
        gfx_fill(52, 56, 56, 30, GFX_BLACK);
        gfx_text(64, 62, r, 4, 2, COL_CUR, GFX_BLACK);
        k = sf_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_MENU) return 1;
        if (k == KEY_STAR && pos) {
            pos--;
            s->room[pos] = '0';
        } else if (k >= KEY_0 && k <= KEY_9) {
            char d = (char)('0' + (k - 2));
            if (pos > 1) pos = 1;
            s->room[pos] = d;
            pos++;
        }
        h9_delay(10);
    }
}

void sf_radio_setup(void)
{
    unsigned side = RX_CH == 1 ? 0 : RX_CH == 2 ? 1 : 2;
    SIDE_BYTE = (uint8_t)((SIDE_BYTE & 0x7F) | side << 7);
    ROUTE_BYTE = (uint8_t)side;
    RF_SETUP(0);
    aprs_quiet();
}

static void ship_span(int x, int y, int *sx, int *sy, int *len, int *horiz)
{
    struct sf *s = SF;
    int x0 = x, y0 = y, l = 1;
    int h = (x > 0 && AT(s->my, x - 1, y) == C_HIT) || (x < N - 1 && AT(s->my, x + 1, y) == C_HIT);
    if (h) {
        while (x0 > 0 && AT(s->my, x0 - 1, y) == C_HIT) x0--;
        while (x0 + l < N && AT(s->my, x0 + l, y) == C_HIT) l++;
        *sx = x0;
        *sy = y;
    } else {
        while (y0 > 0 && AT(s->my, x, y0 - 1) == C_HIT) y0--;
        while (y0 + l < N && AT(s->my, x, y0 + l) == C_HIT) l++;
        *sx = x;
        *sy = y0;
    }
    *len = l;
    *horiz = h;
}

static void clear_shot(void)
{
    struct sf *s = SF;
    if (pgot[0] && pv[0][0] == s->fx && pv[0][1] == s->fy) pgot[0] = 0;
    s->pending = s->timeout = 0;
    s->fx = s->fy = -1;
}

static void on_fire(int x, int y, int apply)
{
    struct sf *s = SF;
    uint8_t t = AT(s->my, x, y);
    s->lx = (int8_t)x;
    s->ly = (int8_t)y;
    if (t == C_SHIP && apply) {
        AT(s->my, x, y) = C_HIT;
        if (s->myhp) s->myhp--;
        sf_cell(MY_X, x, y, C_HIT, 2);
        t = C_HIT;
    }
    if (t == C_HIT) {
        if (sf_sunk(s->my, s->my, x, y)) {
            int sx, sy, len, h;
            if (apply) sf_sink(s->my, MY_X, x, y);
            ship_span(x, y, &sx, &sy, &len, &h);
            send_pkt('S', sx, sy, len, h);
            if (apply) sf_status("YOUR SHIP SUNK! ENEMY TURN", GFX_RED);
        } else {
            send_pkt('H', x, y, -1, -1);
            if (apply) sf_status("HIT! ENEMY SHOTS", GFX_RED);
        }
        if (apply) s->turn = 0;
        return;
    }
    if (t == C_WATER && apply) {
        AT(s->my, x, y) = C_MISS;
        s->turn = 1;
    }
    sf_cell(MY_X, x, y, C_MISS, 2);
    send_pkt('M', x, y, -1, -1);
    if (apply && t == C_WATER) sf_status("ENEMY MISS! YOUR TURN", COL_SHIP);
}

void sf_radio_fire(void)
{
    struct sf *s = SF;
    int retry = s->timeout;
    if (!retry) {
        s->fx = s->ox = s->cx;
        s->fy = s->oy = s->cy;
        s->started = 1;
        s->turn = 0;
    }
    s->pending = 1;
    s->timeout = 0;
    s->ftick = TICK;
    pgot[1] = 0;
    send_pkt('F', s->fx, s->fy, -1, -1);
    sf_status(retry ? "RETRY SENT..." : "SENT! WAITING...", COL_CUR);
}

void sf_radio_tick(void)
{
    struct sf *s = SF;
    int slot, x, y, n, v2, v3;
    char c;
    drain_ring();
    slot = s->pending ? 1 : 0;
    if (!pgot[slot]) return;
    c = (char)pcmd[slot];
    x = pv[slot][0];
    y = pv[slot][1];
    v2 = pv[slot][2];
    v3 = pv[slot][3];
    n = pnv[slot];
    pgot[slot] = 0;
    wait_ms(2000);
    if (!slot) {
        uint8_t t;
        if (c != 'F' || n < 2 || (unsigned)x >= N || (unsigned)y >= N) return;
        t = AT(s->my, x, y);
        if (s->started && s->turn && (t == C_SHIP || t == C_WATER)) return;
        s->started = 1;
        on_fire(x, y, t == C_SHIP || t == C_WATER);
        return;
    }
    if (c == 'H' && n >= 2 && (unsigned)x < N && (unsigned)y < N) {
        if (AT(s->en, x, y) != C_HIT) {
            AT(s->en, x, y) = C_HIT;
            if (s->enhp) s->enhp--;
            sf_cell(EN_X, x, y, C_HIT, x == s->cx && y == s->cy);
        }
        clear_shot();
        s->turn = 1;
        sf_status("DIRECT HIT! AGAIN", COL_CUR);
    } else if (c == 'S' && n >= 4 && v2 >= 1 && v2 <= 4 && (unsigned)v3 <= 1 &&
               (unsigned)x < N && (unsigned)y < N &&
               x + (v3 ? v2 - 1 : 0) < N && y + (v3 ? 0 : v2 - 1) < N &&
               s->fx >= x && s->fx <= x + (v3 ? v2 - 1 : 0) &&
               s->fy >= y && s->fy <= y + (v3 ? 0 : v2 - 1)) {
        int i, gx, gy;
        if (s->enhp) s->enhp--;
        for (i = 0; i < v2; i++) {
            gx = x + (v3 ? i : 0);
            gy = y + (v3 ? 0 : i);
            AT(s->en, gx, gy) = C_HIT;
            sf_cell(EN_X, gx, gy, C_HIT, gx == s->cx && gy == s->cy);
        }
        sf_sink(s->en, EN_X, x, y);
        clear_shot();
        s->turn = 1;
        sf_status("ENEMY SHIP SUNK! YOUR TURN", COL_SHIP);
    } else if (c == 'M' && n >= 2 && (unsigned)x < N && (unsigned)y < N) {
        AT(s->en, x, y) = C_MISS;
        sf_cell(EN_X, x, y, C_MISS, x == s->cx && y == s->cy);
        clear_shot();
        s->turn = 0;
        sf_status("MISS! WAIT ENEMY", COL_MISS);
    }
}
