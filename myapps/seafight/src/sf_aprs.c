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
        if (c >= 'a' && c <= 'z') c -= 32;
        d[i] = (uint8_t)(c << 1);
    }
    for (; i < 6; i++) d[i] = (uint8_t)(' ' << 1);
    d[6] = (uint8_t)(0x60 | (last ? 1 : 0));
}

void sf_load_call(void)
{
    struct sf *s = SF;
    int n = 0;
    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) s->call[n] = (char)MYCALL[n];
    if (!n) {
        s->call[0] = 'N'; s->call[1] = '0'; s->call[2] = 'C';
        s->call[3] = 'A'; s->call[4] = 'L'; s->call[5] = 'L';
        n = 6;
    }
    s->call[n] = 0;
}

/* dst=APH9BS, src=call, ">BS<room>:<cmd>,x,y[,len,h]". Do not touch the RF box after TX. */
static void send_pkt(const char *cmd, int x, int y, int l, int h)
{
    struct sf *s = SF;
    uint8_t kiss[88];
    uint8_t frame[64];
    int p = 0, k = 0, i;
    pack_addr(frame + p, "APH9BS", 0); p += 7;
    pack_addr(frame + p, s->call, 1); p += 7;
    frame[p++] = 0x03;
    frame[p++] = 0xF0;
    frame[p++] = '>';
    frame[p++] = 'B';
    frame[p++] = 'S';
    frame[p++] = (uint8_t)s->room[0];
    frame[p++] = (uint8_t)s->room[1];
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
    kiss[k++] = 0xC0;
    kiss[k++] = 0x00;
    for (i = 0; i < p; i++) {
        if (frame[i] == 0xC0) { kiss[k++] = 0xDB; kiss[k++] = 0xDC; }
        else if (frame[i] == 0xDB) { kiss[k++] = 0xDB; kiss[k++] = 0xDD; }
        else kiss[k++] = frame[i];
    }
    kiss[k++] = 0xC0;
    STOCK_KISS_TX(kiss, (unsigned)k);
}

static int self_rx(void)
{
    struct sf *s = SF;
    int i;
    for (i = 0; i < 6; i++) {
        uint8_t c = (uint8_t)s->call[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (!s->call[i] || s->call[i] == '-') c = ' ';
        if (s->rx[8 + i] != (uint8_t)(c << 1)) return 0;
    }
    return 1;
}

/* 1: a game packet from someone else is in cmd / v[0..3]. */
static int poll_rx(char *cmd, int *v, int *nv)
{
    struct sf *s = SF;
    unsigned write = RING_WRITE & RING_MASK, read = RING_READ & RING_MASK;
    int got = 0;
    while (read != write) {
        uint8_t b = RING[read];
        read = (read + 1u) & RING_MASK;
        if (b == 0xC0) {
            if (s->rxn > 20 && s->rx[0] == 0 && !self_rx()) {
                unsigned at = 17, p;
                int n = 0;
                if (at + 8 <= s->rxn && s->rx[at] == '>' && s->rx[at + 1] == 'B' && s->rx[at + 2] == 'S' &&
                    s->rx[at + 3] == (uint8_t)s->room[0] && s->rx[at + 4] == (uint8_t)s->room[1] &&
                    s->rx[at + 5] == ':') {
                    p = at + 6;
                    while (p < s->rxn && s->rx[p] != ',' && n < 7) cmd[n++] = (char)s->rx[p++];
                    cmd[n] = 0;
                    v[0] = v[1] = v[2] = v[3] = -1;
                    *nv = 0;
                    while (p < s->rxn && *nv < 4 && s->rx[p] == ',') {
                        p++;
                        v[(*nv)++] = (p < s->rxn && s->rx[p] >= '0' && s->rx[p] <= '9') ? s->rx[p] - '0' : -1;
                        p++;
                    }
                    if (n) got = 1;
                }
            }
            s->rxn = s->esc = 0;
            if (got) break;
            continue;
        }
        if (s->esc) { b = b == 0xDC ? 0xC0 : b == 0xDD ? 0xDB : b; s->esc = 0; }
        else if (b == 0xDB) { s->esc = 1; continue; }
        if (s->rxn < sizeof s->rx) s->rx[s->rxn++] = b;
    }
    RING_READ = (uint16_t)read;
    return got;
}

/* MENU confirms; 0-9 type digits (5 is a digit, not OK). */
int sf_select_room(void)
{
    struct sf *s = SF;
    int pos = 0;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(30, 30, "ROOM (0-99):", 12, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(8, 102, "0-9 DIGIT  MENU OK", 20, 1, GFX_GREY, GFX_BLACK);
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
        if (k >= KEY_0 && k <= KEY_9) {
            char d = (char)('0' + (k - 2));
            if (!pos) { s->room[0] = d; pos = 1; }
            else s->room[1] = d;
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

static void sink_span_en(int sx, int sy, int len, int h)
{
    struct sf *s = SF;
    int i, gx, gy, dx, dy, nx, ny;
    for (i = 0; i < len; i++) {
        gx = sx + (h ? i : 0);
        gy = sy + (h ? 0 : i);
        if (AT(s->en, gx, gy) != C_HIT) {
            AT(s->en, gx, gy) = C_HIT;
            sf_cell(EN_X, gx, gy, C_HIT, gx == s->cx && gy == s->cy);
        }
        for (dy = -1; dy <= 1; dy++)
            for (dx = -1; dx <= 1; dx++) {
                nx = gx + dx;
                ny = gy + dy;
                if (nx < 0 || nx >= N || ny < 0 || ny >= N) continue;
                if (AT(s->en, nx, ny) == C_WATER) {
                    AT(s->en, nx, ny) = C_MISS;
                    sf_cell(EN_X, nx, ny, C_MISS, nx == s->cx && ny == s->cy);
                }
            }
    }
}

static void clear_shot(void)
{
    struct sf *s = SF;
    s->pending = s->timeout = 0;
    s->fx = s->fy = -1;
}

/* Incoming FIRE: apply=1 first time, 0 retry. HIT/SUNK keep their turn; MISS gives us the turn. */
static void on_fire(int x, int y, int apply)
{
    struct sf *s = SF;
    uint8_t t = AT(s->my, x, y);
    if (t == C_SHIP && apply) {
        AT(s->my, x, y) = C_HIT;
        if (s->myhp) s->myhp--;
        sf_cell(MY_X, x, y, C_HIT, 0);
        if (sf_sunk(s->my, s->my, x, y)) {
            int sx, sy, len, h;
            sf_sink(s->my, MY_X, x, y);
            ship_span(x, y, &sx, &sy, &len, &h);
            send_pkt("SUNK", sx, sy, len, h);
            sf_status("YOUR SHIP SUNK!", GFX_RED);
        } else {
            send_pkt("HIT", x, y, -1, -1);
            sf_status("HIT! ENEMY SHOTS", GFX_RED);
        }
        s->turn = 0;
        return;
    }
    if (t == C_HIT) {
        if (sf_sunk(s->my, s->my, x, y)) {
            int sx, sy, len, h;
            ship_span(x, y, &sx, &sy, &len, &h);
            send_pkt("SUNK", sx, sy, len, h);
        } else send_pkt("HIT", x, y, -1, -1);
        return;
    }
    if (t == C_WATER && apply) {
        AT(s->my, x, y) = C_MISS;
        s->turn = 1;
    }
    sf_cell(MY_X, x, y, C_MISS, 0);
    send_pkt("MISS", x, y, -1, -1);
    if (apply && t == C_WATER) sf_status("ENEMY MISS! TURN", COL_SHIP);
}

void sf_radio_fire(void)
{
    struct sf *s = SF;
    if (s->timeout) {
        send_pkt("FIRE", s->fx, s->fy, -1, -1);
        s->ftick = TICK;
        s->timeout = 0;
        sf_status("RETRY SENT...", COL_CUR);
        return;
    }
    send_pkt("FIRE", s->cx, s->cy, -1, -1);
    s->started = s->pending = 1;
    s->timeout = 0;
    s->fx = s->cx;
    s->fy = s->cy;
    s->ftick = TICK;
    s->turn = 0;
    sf_status("SENT! WAITING...", COL_CUR);
}

void sf_radio_tick(void)
{
    struct sf *s = SF;
    char cmd[8];
    int v[4], nv = 0, x, y;
    if (!poll_rx(cmd, v, &nv)) return;
    x = v[0];
    y = v[1];
    if (cmd[0] == 'F' && nv >= 2 && x >= 0 && x < N && y >= 0 && y < N) {
        int first = AT(s->my, x, y) == C_SHIP || AT(s->my, x, y) == C_WATER;
        if (s->started && s->turn && !s->pending && first) {
            /* our turn: ignore a stray first shot */
        } else {
            s->started = 1;
            on_fire(x, y, first);
        }
    } else if (cmd[0] == 'H' && nv >= 2 && x >= 0 && x < N && y >= 0 && y < N) {
        if (AT(s->en, x, y) != C_HIT) {
            AT(s->en, x, y) = C_HIT;
            if (s->enhp) s->enhp--;
            sf_cell(EN_X, x, y, C_HIT, x == s->cx && y == s->cy);
        }
        clear_shot();
        s->turn = 1;
        sf_status("DIRECT HIT! AGAIN", COL_CUR);
    } else if (cmd[0] == 'S' && nv >= 4) {
        int sx = v[0], sy = v[1], len = v[2], h = v[3];
        if (len >= 1 && len <= 4 && h >= 0 && h <= 1 && sx >= 0 && sx < N && sy >= 0 && sy < N &&
            sx + (h ? len - 1 : 0) < N && sy + (h ? 0 : len - 1) < N &&
            s->fx >= sx && s->fx <= sx + (h ? len - 1 : 0) &&
            s->fy >= sy && s->fy <= sy + (h ? 0 : len - 1)) {
            if (s->enhp) s->enhp--;
            sink_span_en(sx, sy, len, h);
            clear_shot();
            s->turn = 1;
            sf_status("ENEMY SHIP SUNK!", COL_SHIP);
        }
    } else if (cmd[0] == 'M' && nv >= 2 && x >= 0 && x < N && y >= 0 && y < N) {
        AT(s->en, x, y) = C_MISS;
        sf_cell(EN_X, x, y, C_MISS, x == s->cx && y == s->cy);
        clear_shot();
        s->turn = 0;
        sf_status("MISS! WAIT ENEMY", COL_MISS);
    }
}
