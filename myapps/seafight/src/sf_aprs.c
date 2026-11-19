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

/* Same as tx_test: static buffers in the module, not 0x2B8E0. Payload ">BS01:F 4 4". */
static uint8_t sf_kiss[88];
static uint8_t sf_frame[64];
static uint8_t sf_rx[128];
static uint8_t sf_rxn;
static char fire_cmd[8], reply_cmd[8];
static int fire_v[4], reply_v[4], fire_nv, reply_nv;
static uint8_t got_fire, got_reply;

static void take_frame(void)
{
    struct sf *s = SF;
    unsigned at, p;
    int n = 0, nv = 0, v[4];
    char cmd[8];
    if (sf_rxn <= 2) return;
    for (at = 0; at + 8 <= sf_rxn; at++) {
        if (sf_rx[at] == '>' && sf_rx[at + 1] == 'B' && sf_rx[at + 2] == 'S' &&
            sf_rx[at + 3] == (uint8_t)s->room[0] && sf_rx[at + 4] == (uint8_t)s->room[1] &&
            sf_rx[at + 5] == ':')
            break;
    }
    if (at + 8 > sf_rxn) return;
    p = at + 6;
    while (p < sf_rxn && sf_rx[p] >= 'A' && sf_rx[p] <= 'Z' && n < 7)
        cmd[n++] = (char)sf_rx[p++];
    cmd[n] = 0;
    v[0] = v[1] = v[2] = v[3] = -1;
    while (p < sf_rxn && nv < 4 && (sf_rx[p] == ' ' || sf_rx[p] == ',')) {
        p++;
        v[nv++] = (p < sf_rxn && sf_rx[p] >= '0' && sf_rx[p] <= '9') ? sf_rx[p] - '0' : -1;
        p++;
    }
    if (!n) return;
    if (cmd[0] == 'F') {
        if (s->pending && nv >= 2 && v[0] == s->fx && v[1] == s->fy) return;
        for (p = 0; p < 8; p++) fire_cmd[p] = cmd[p];
        fire_v[0] = v[0]; fire_v[1] = v[1]; fire_v[2] = v[2]; fire_v[3] = v[3];
        fire_nv = nv;
        got_fire = 1;
        return;
    }
    if (cmd[0] != 'H' && cmd[0] != 'M' && cmd[0] != 'S') return;
    for (p = 0; p < 8; p++) reply_cmd[p] = cmd[p];
    reply_v[0] = v[0]; reply_v[1] = v[1]; reply_v[2] = v[2]; reply_v[3] = v[3];
    reply_nv = nv;
    got_reply = 1;
}

/* Always empty the modem ring (seafight.aprs.c). Never leave READ behind WRITE. */
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

static void send_pkt(const char *cmd, int x, int y, int l, int h)
{
    struct sf *s = SF;
    uint8_t *kiss = sf_kiss, *frame = sf_frame;
    int p = 0, k = 0, i;
    if (cmd[0] != 'F') {
        sf_status("WAIT REPLY TX...", COL_CUR);
        wait_ms(2000);
    }
    pack_addr(frame + p, "DKXCE", 0); p += 7;
    pack_addr(frame + p, s->call, 1); p += 7;
    frame[p++] = 0x03;
    frame[p++] = 0xF0;
    frame[p++] = '>';
    frame[p++] = 'B';
    frame[p++] = 'S';
    frame[p++] = (uint8_t)s->room[0];
    frame[p++] = (uint8_t)s->room[1];
    frame[p++] = ':';
    frame[p++] = (uint8_t)cmd[0];
    if (x >= 0 && y >= 0) {
        frame[p++] = ' ';
        frame[p++] = (uint8_t)('0' + x);
        frame[p++] = ' ';
        frame[p++] = (uint8_t)('0' + y);
        if (l >= 0) {
            frame[p++] = ' ';
            frame[p++] = (uint8_t)('0' + l);
            frame[p++] = ' ';
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
    wait_ms(200);
    sf_board();
}

static int take_slot(uint8_t *got, char *src, int *sv, int sn, char *cmd, int *v, int *nv)
{
    int i;
    if (!*got) return 0;
    for (i = 0; i < 8; i++) cmd[i] = src[i];
    v[0] = sv[0]; v[1] = sv[1]; v[2] = sv[2]; v[3] = sv[3];
    *nv = sn;
    *got = 0;
    return 1;
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
    if (got_fire && fire_v[0] == s->fx && fire_v[1] == s->fy) got_fire = 0;
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
        s->timeout = 0;
        s->pending = 1;
        s->ftick = TICK;
        got_reply = 0;
        send_pkt("FIRE", s->fx, s->fy, -1, -1);
        sf_status("RETRY SENT...", COL_CUR);
        return;
    }
    s->fx = s->cx;
    s->fy = s->cy;
    s->started = s->pending = 1;
    s->timeout = 0;
    s->ftick = TICK;
    s->turn = 0;
    got_reply = 0;
    send_pkt("FIRE", s->cx, s->cy, -1, -1);
    sf_status("SENT! WAITING...", COL_CUR);
}

void sf_radio_tick(void)
{
    struct sf *s = SF;
    char cmd[8];
    int v[4], nv = 0, x, y;
    drain_ring();
    if (s->pending) {
        if (!take_slot(&got_reply, reply_cmd, reply_v, reply_nv, cmd, v, &nv)) return;
    } else {
        if (!take_slot(&got_fire, fire_cmd, fire_v, fire_nv, cmd, v, &nv)) return;
        x = v[0];
        y = v[1];
        if (cmd[0] == 'F' && nv >= 2 && x >= 0 && x < N && y >= 0 && y < N) {
            int first = AT(s->my, x, y) == C_SHIP || AT(s->my, x, y) == C_WATER;
            if (s->started && s->turn && first) return;
            s->started = 1;
            on_fire(x, y, first);
        }
        return;
    }
    x = v[0];
    y = v[1];
    if (cmd[0] == 'H' && nv >= 2 && x >= 0 && x < N && y >= 0 && y < N) {
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
