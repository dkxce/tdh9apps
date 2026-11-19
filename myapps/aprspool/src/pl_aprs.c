/* APRS link. Linked only into .PL RADIO. */
#include "pl.h"

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

static uint8_t pl_kiss[88];
static uint8_t pl_rx[96];
static uint8_t pl_rxn, got_shot, got_sync, skip_sync, hold_sync[20];
static int hold_ang, hold_pwr;

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

void pl_load_call(void)
{
    struct pl *s = PL;
    int n = 0;
    const char *p = "N0CALL";
    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) s->call[n] = (char)MYCALL[n];
    if (!n) for (; n < 6; n++) s->call[n] = p[n];
    s->call[n] = 0;
}

static int hexv(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int parse_dec(unsigned *p)
{
    int v = 0, n = 0;
    while (*p < pl_rxn && (pl_rx[*p] == ' ' || pl_rx[*p] == ',')) (*p)++;
    while (*p < pl_rxn && pl_rx[*p] >= '0' && pl_rx[*p] <= '9' && n < 4) {
        v = v * 10 + (pl_rx[*p] - '0');
        (*p)++;
        n++;
    }
    return n ? v : -1;
}

static void take_frame(void)
{
    struct pl *s = PL;
    unsigned at, p;
    int i, hi, lo;
    if (pl_rxn < 10) return;
    for (at = 0; at + 10 <= pl_rxn; at++)
        if (pl_rx[at] == '>' && pl_rx[at + 1] == 'P' && pl_rx[at + 2] == 'L' &&
            pl_rx[at + 3] == (uint8_t)s->room[0] && pl_rx[at + 4] == (uint8_t)s->room[1] &&
            pl_rx[at + 5] == ':')
            break;
    if (at + 10 > pl_rxn) return;
    p = at + 6;
    if (p + 4 <= pl_rxn && pl_rx[p] == 'S' && pl_rx[p + 1] == 'H' &&
        pl_rx[p + 2] == 'O' && pl_rx[p + 3] == 'T') {
        p += 4;
        hold_ang = parse_dec(&p);
        hold_pwr = parse_dec(&p);
        if (hold_ang < 0 || hold_ang > 359 || hold_pwr < 2 || hold_pwr > 11) return;
        if (s->we_shot && s->aim == hold_ang && s->power == hold_pwr) return;
        got_shot = 1;
        return;
    }
    if (p + 4 <= pl_rxn && pl_rx[p] == 'S' && pl_rx[p + 1] == 'Y' &&
        pl_rx[p + 2] == 'N' && pl_rx[p + 3] == 'C') {
        p += 4;
        while (p < pl_rxn && pl_rx[p] == ' ') p++;
        if (skip_sync) { skip_sync = 0; return; }
        for (i = 0; i < 20; i++) {
            if (p + 1 >= pl_rxn) return;
            hi = hexv(pl_rx[p++]);
            lo = hexv(pl_rx[p++]);
            if (hi < 0 || lo < 0) return;
            hold_sync[i] = (uint8_t)((hi << 4) | lo);
        }
        got_sync = 1;
    }
}

static void drain_ring(void)
{
    unsigned write = RING_WRITE & RING_MASK, read = RING_READ & RING_MASK;
    while (read != write) {
        uint8_t b = RING[read];
        read = (read + 1u) & RING_MASK;
        if (b == 0xC0) {
            take_frame();
            pl_rxn = 0;
        } else if (pl_rxn < sizeof pl_rx)
            pl_rx[pl_rxn++] = b;
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

static void put_dec(uint8_t *f, int *p, int v)
{
    char t[4];
    int n = 0;
    if (v < 0) v = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < 4);
    while (n) f[(*p)++] = (uint8_t)t[--n];
}

static char hexn(unsigned v)
{
    return (char)(v < 10 ? '0' + v : 'A' + v - 10);
}

static void send_raw(const uint8_t *pay, int n)
{
    struct pl *s = PL;
    uint8_t *f = pl_kiss + 2;
    int p = 0, i;
    pack_addr(f + p, "DKXCE", 0); p += 7;
    pack_addr(f + p, s->call, 1); p += 7;
    f[p++] = 0x03;
    f[p++] = 0xF0;
    for (i = 0; i < n; i++) f[p++] = pay[i];
    pl_kiss[0] = 0xC0;
    pl_kiss[1] = 0x00;
    pl_kiss[2 + p] = 0xC0;
    STOCK_KISS_TX(pl_kiss, (unsigned)(p + 3));
    h9_delay(200);
    aprs_quiet();
    drain_ring();
}

static void send_shot_pkt(int angle, int power)
{
    struct pl *s = PL;
    uint8_t pay[24];
    int p = 0;
    pay[p++] = '>';
    pay[p++] = 'P';
    pay[p++] = 'L';
    pay[p++] = (uint8_t)s->room[0];
    pay[p++] = (uint8_t)s->room[1];
    pay[p++] = ':';
    pay[p++] = 'S';
    pay[p++] = 'H';
    pay[p++] = 'O';
    pay[p++] = 'T';
    pay[p++] = ' ';
    put_dec(pay, &p, angle);
    pay[p++] = ' ';
    put_dec(pay, &p, power);
    send_raw(pay, p);
}

static void send_sync_pkt(void)
{
    struct pl *s = PL;
    uint8_t snap[20], pay[56];
    int p = 0, i;
    pl_pack_sync(snap);
    pay[p++] = '>';
    pay[p++] = 'P';
    pay[p++] = 'L';
    pay[p++] = (uint8_t)s->room[0];
    pay[p++] = (uint8_t)s->room[1];
    pay[p++] = ':';
    pay[p++] = 'S';
    pay[p++] = 'Y';
    pay[p++] = 'N';
    pay[p++] = 'C';
    pay[p++] = ' ';
    for (i = 0; i < 20; i++) {
        pay[p++] = (uint8_t)hexn(snap[i] >> 4);
        pay[p++] = (uint8_t)hexn(snap[i] & 15);
    }
    wait_ms(2000);
    skip_sync = 1;
    send_raw(pay, p);
}

int pl_select_room(void)
{
    struct pl *s = PL;
    int pos = 0;
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
        gfx_text(64, 62, r, 4, 2, COL_AIM, GFX_BLACK);
        gfx_fill(64 + (pos > 1 ? 1 : pos) * 12, 78, 10, 2, GFX_RED);
        k = pl_key();
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

void pl_radio_setup(void)
{
    unsigned side = RX_CH == 1 ? 0 : RX_CH == 2 ? 1 : 2;
    SIDE_BYTE = (uint8_t)((SIDE_BYTE & 0x7F) | side << 7);
    ROUTE_BYTE = (uint8_t)side;
    RF_SETUP(0);
    aprs_quiet();
}

void pl_radio_shot(void)
{
    struct pl *s = PL;
    int a = s->aim, pwr = s->power;
    got_sync = 0;
    s->we_shot = 1;
    s->turn = 0;
    send_shot_pkt(a, pwr);
    wait_ms(500);
    pl_strike(a, pwr);
}

void pl_radio_tick(void)
{
    struct pl *s = PL;
    drain_ring();
    if (got_shot && s->state == ST_AIM && !s->we_shot) {
        int a = hold_ang, pwr = hold_pwr;
        got_shot = 0;
        skip_sync = 0;
        s->turn = 0;
        wait_ms(500);
        pl_strike(a, pwr);
        s->we_shot = 0;
    }
    if (got_sync && s->state != ST_ROLL && !s->we_shot) {
        got_sync = 0;
        wait_ms(200);
        pl_apply_sync(hold_sync);
        s->turn = 1;
        if (pl_ended()) pl_show_end();
        else { pl_hud(); pl_render(); }
    }
}

void pl_radio_rest(void)
{
    struct pl *s = PL;
    if (pl_ended()) {
        if (s->we_shot) send_sync_pkt();
        s->we_shot = 0;
        pl_show_end();
        return;
    }
    if (s->we_shot) {
        send_sync_pkt();
        s->we_shot = 0;
        s->turn = 0;
        s->state = ST_AIM;
        pl_hud();
        pl_render();
        return;
    }
    s->state = ST_AIM;
    pl_hud();
    pl_render();
}
