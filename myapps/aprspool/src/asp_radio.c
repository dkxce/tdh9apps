/* .PL RADIO: hidden module, room + APRS pool game.
 * Protocol: >PL<ROOM>:SHOT <angle> <power>  and  >PL<ROOM>:SYNC <40hex>.
 * Radio plumbing (pack_addr, KISS over STOCK_KISS_TX, ring drain, RF setup,
 * room select) mirrors seafight's sf_aprs.c, verified on hardware. */
#include "asp.h"

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

static uint8_t pkiss[160];
static uint8_t pcmd, pn, pv[4];
/* For SYNC: pointer into a->rx at the first hex char and length of the hex run. */
static uint8_t *psync;
static uint8_t psync_n;

static void draw_hud_pwr(void);

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

static int hexval(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static unsigned p_num(uint8_t **pp, uint8_t *end)
{
    unsigned v = 0;
    while (*pp < end && **pp >= '0' && **pp <= '9') {
        v = v * 10u + (uint8_t)(**pp - '0');
        (*pp)++;
    }
    return v;
}

/* Parse one received KISS frame (addressed, room match): sets cmd ('S'/'H'),
 * array pv and returns number of values, or -1 if it should be ignored. */
static int take_frame(void)
{
    struct asp *a = ASP;
    uint8_t *f = a->rx;
    unsigned len = a->rxn;
    if (len < 17) return -1;                       /* header + payload */
    if (f[0] != 0x00) return -1;                   /* KISS port must be 0 */
    /* dst = f[1..6], Ctrl f[15], PID f[16] (UI frame). Payload at 17. */
    unsigned at = 17;
    if (at + 7 <= len && f[at] == '>' && f[at + 1] == 'P' && f[at + 2] == 'L' &&
        f[at + 3] == a->room[0] && f[at + 4] == a->room[1] &&
        f[at + 5] == ':') {
uint8_t *p = &f[at + 6];
    uint8_t *end = &f[len];
    /* Ignore our own echo: src call is f[8..13], compare to MYCALL chars. */
    {
        int eq = 1, i;
        for (i = 0; i < 6; i++) {
            uint8_t s = (uint8_t)(f[8 + i] >> 1);
            uint8_t c = i < 6 && MYCALL[i] > ' ' && MYCALL[i] <= '~' ? (uint8_t)MYCALL[i] : ' ';
            if (s != c) { eq = 0; break; }
        }
        if (eq) return -1;
    }
    pcmd = (uint8_t)*p;
    while (p < end && *p >= 'A' && *p <= 'Z') p++;   /* skip command word */
    while (p < end && *p == ' ') p++;
    /* SYNC payload: run of hex chars; store pointer for apply_sync_hex. */
    if (pcmd == 'Y') {
        psync = p;
        while (p < end && ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F')
                           || (*p >= 'a' && *p <= 'f'))) p++;
        psync_n = (uint8_t)(p - psync);
        pn = 1;                      /* it returned something */
        return 1;
    }
    pn = 0;
    while (pn < 4 && p < end) {
        while (p < end && *p == ' ') p++;
        if (p >= end) break;
        if (*p >= '0' && *p <= '9') {
            pv[pn] = (uint8_t)p_num(&p, end);
            pn++;
        } else break;
    }
    return (int)pn;
    }
    return -1;
}

static void drain_ring(void)
{
    struct asp *a = ASP;
    unsigned write = RING_WRITE & RING_MASK, read = RING_READ & RING_MASK;
    while (read != write) {
        uint8_t b = RING[read];
        read = (read + 1u) & RING_MASK;
        if (b == 0xC0) {
            if (take_frame() >= 0) { a->rxn = 0; RING_READ = (uint16_t)read; return; }
            a->rxn = 0;
            a->esc = 0;
        } else if (a->esc) {
            a->rxn = 0;                              /* unescaped byte -> drop frame */
            a->esc = 0;
        } else if (b == 0xDB) {
            a->esc = 1;
        } else {
            if (a->rxn < sizeof a->rx) a->rx[a->rxn++] = b;
        }
    }
    RING_READ = (uint16_t)read;
}

/* Send one game packet: dst=DKXCE, src=mycall, payload ">PL<ROOM>:CMD ...".
 * a0/a1 are written as full decimal (angle can be 0..359), extra appended as-is. */
static void send_cmd(const char *cmd, int a0, int a1, const char *extra)
{
    struct asp *as = ASP;
    uint8_t *f = pkiss + 2;
    int p = 0;
    char nb[8], *q;
    pack_addr(f + p, "DKXCE", 0); p += 7;      /* dst */
    pack_addr(f + p, as->call, 1); p += 7;     /* src */
    f[p++] = 0x03;                             /* UI */
    f[p++] = 0xF0;                             /* no L3 */
    f[p++] = '>'; f[p++] = 'P'; f[p++] = 'L';
    f[p++] = (uint8_t)as->room[0];
    f[p++] = (uint8_t)as->room[1];
    f[p++] = ':';
    while (*cmd) f[p++] = (uint8_t)*cmd++;
    if (a0 >= 0) {
        f[p++] = ' ';
        q = nb + sizeof nb;
        *--q = 0;
        do { *--q = (char)('0' + a0 % 10); a0 /= 10; } while (a0 && q > nb);
        while (*q) f[p++] = (uint8_t)*q++;
    }
    if (a1 >= 0) {
        f[p++] = ' ';
        q = nb + sizeof nb;
        *--q = 0;
        do { *--q = (char)('0' + a1 % 10); a1 /= 10; } while (a1 && q > nb);
        while (*q) f[p++] = (uint8_t)*q++;
    }
    if (extra) while (*extra) f[p++] = (uint8_t)*extra++;
    pkiss[0] = 0xC0;
    pkiss[1] = 0x00;
    f[p] = 0xC0;
    STOCK_KISS_TX(pkiss, (unsigned)(p + 1));
    h9_delay(200);
    aprs_quiet();
    drain_ring();
}

/* Serialize ball coordinates+act into 40 hex chars (2 bytes/ball: xhi,xlo). */
static void sync_hex(char *out)
{
    struct asp *a = ASP;
    for (int i = 0; i < MAX_BALLS; i++) {
        unsigned x = a->act[i] ? (unsigned)TO_PX(a->bx[i] + 128) : 0x7F;
        unsigned y = a->act[i] ? (unsigned)TO_PX(a->by[i] + 128) : 0x7F;
        if (x > 0xFF) x = 0xFF;
        if (y > 0xFF) y = 0xFF;
        static const char H[] = "0123456789ABCDEF";
        *out++ = H[x >> 4]; *out++ = H[x & 15];
        *out++ = H[y >> 4]; *out++ = H[y & 15];
    }
    *out = 0;
}

int asp_select_room(void)
{
    struct asp *a = ASP;
    int pos = 0;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(30, 30, "ROOM (0-99):", 12, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(4, 102, "0-9 DIGIT *:DEL MENU OK", 24, 1, GFX_GREY, GFX_BLACK);
    for (;;) {
        char r[4];
        unsigned k;
        r[0] = a->room[0];
        r[1] = a->room[1];
        r[2] = 0;
        gfx_fill(52, 56, 56, 30, GFX_BLACK);
        gfx_text(64, 62, r, 4, 2, COL_AIM, GFX_BLACK);
        gfx_fill(64 + (pos > 1 ? 1 : pos) * 12, 78, 10, 2, GFX_RED);
        k = asp_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_MENU) return 1;
        if (k == KEY_STAR && pos) { pos--; a->room[pos] = '0'; }
        else if (k >= KEY_0 && k <= KEY_9) {
            char d = (char)('0' + (k - 2));
            if (pos > 1) pos = 1;
            a->room[pos] = d;
            pos++;
        }
        h9_delay(10);
    }
}

void asp_radio_setup(void)
{
    unsigned side = RX_CH == 1 ? 0 : RX_CH == 2 ? 1 : 2;
    SIDE_BYTE = (uint8_t)((SIDE_BYTE & 0x7F) | side << 7);
    ROUTE_BYTE = (uint8_t)side;
    RF_SETUP(0);
    aprs_quiet();
}

void asp_load_call(void)
{
    struct asp *a = ASP;
    int n = 0;
    for (; n < 6 && MYCALL[n] > ' ' && MYCALL[n] <= '~'; n++) a->call[n] = (char)MYCALL[n];
    if (!n) {
        a->call[0] = 'N'; a->call[1] = 'O'; a->call[2] = 'C';
        a->call[3] = 'A'; a->call[4] = 'L'; a->call[5] = 'L'; n = 6;
    }
    a->call[n] = 0;
}

/* Fire our shot: broadcast SHOT, then play the deterministic animation. */
void asp_radio_fire(void)
{
    struct asp *a = ASP;
    a->started = 1;
    a->myturn = 0;                       /* we shot, now opponent's turn */
    a->pending = 1;
    a->timeout = 0;
    a->ftick = TICK;
    send_cmd("SHOT", a->aim, a->power, 0);
    int32_t spd = (int32_t)a->power * 170;
    a->vx[0] = (spd * (asp_cos(a->aim))) >> 10;
    a->vy[0] = (spd * (asp_sin(a->aim))) >> 10;
    a->state = ST_MOVING;
    a->shots++;
    asp_status("SHOT SENT... WAIT", COL_CUR);
}

/* Apply an incoming SHOT (opponent fired): start the same local animation. */
static void apply_incoming_shot(void)
{
    struct asp *a = ASP;
    int ang = pv[0], pwr = pv[1];
    if (ang < 0 || ang > 359 || pwr < 2 || pwr > 11) return;
    a->started = 1;
    a->myturn = 0;                       /* opponent shot; not my turn */
    a->pending = 0;                      /* I am not the shooter */
    int32_t spd = (int32_t)pwr * 170;
    a->vx[0] = (spd * (asp_cos(ang))) >> 10;
    a->vy[0] = (spd * (asp_sin(ang))) >> 10;
    a->state = ST_MOVING;
    asp_status("OPPONENT SHOT...", COL_CUR);
}

/* Parse 40 hex chars from rx (SYNC payload) and restore ball positions. */
static void apply_sync_hex(const uint8_t *h)
{
    struct asp *a = ASP;
    for (int i = 0; i < MAX_BALLS; i++) {
        int x = hexval(h[0]) << 4 | hexval(h[1]);
        int y = hexval(h[2]) << 4 | hexval(h[3]);
        h += 4;
        if (x == 0x7F && y == 0x7F) {
            a->act[i] = 0;
            a->vx[i] = a->vy[i] = 0;
        } else {
            a->act[i] = 1;
            a->bx[i] = TO_FP(x - 128);
            a->by[i] = TO_FP(y - 128);
            a->ox[i] = a->bx[i];
            a->oy[i] = a->by[i];
            a->vx[i] = a->vy[i] = 0;
        }
    }
    a->state = ST_AIM;
}

/* Update/redraw the power bar in the HUD (top strip). */
static void draw_hud_pwr(void)
{
    struct asp *a = ASP;
    gfx_fill(118, 2, 42, 10, GFX_BLACK);
    gfx_text(120, 3, "PWR:", 4, 1, GFX_WHITE, GFX_BLACK);
    gfx_fill(150, 4, 6 * a->power / 11 + 6, 6, (a->power > 8) ? GFX_RED : 0x07E0);
}

/* Solo (local) game against physics: no radio at all. */
static void asp_play_solo(void)
{
    struct asp *a = ASP;
    uint32_t ftick = TICK;
    for (;;) {
        unsigned k = asp_key();
        if (k == KEY_BACK) return;

        if (a->state == ST_MOVING) {
            if ((uint32_t)(TICK - ftick) >= 2u) {
                ftick = TICK;
                int moving = asp_step();
                for (int i = 0; i < MAX_BALLS; i++) asp_redraw_ball(i);
                if (!moving) {
                    uint8_t outcome = a->state;
                    a->state = ST_AIM;
                    a->myturn = 1;
                    if (outcome == ST_WIN || outcome == ST_LOSE) {
                        asp_render();
                        gfx_fill(34, 56, 92, 18, GFX_BLACK);
                        if (outcome == ST_WIN)
                            gfx_text(62, 60, "WIN!", 6, 2, COL_SHIP, GFX_BLACK);
                        else
                            gfx_text(56, 60, "LOSE", 6, 2, GFX_RED, GFX_BLACK);
                        gfx_text(48, 80, "BACK: EXIT", 10, 1, GFX_GREY, GFX_BLACK);
                        while (h9_key() != H9_KEY_NONE) h9_delay(3);
                        for (;;) { if (asp_key() == KEY_BACK) return; h9_delay(10); }
                    } else {
                        /* Balls stopped, cue ball possibly potted: redraw. */
                        if (!a->act[0]) {
                            a->bx[0] = TO_FP(TBL_X + 26);
                            a->by[0] = TO_FP(TBL_Y + TBL_H / 2);
                            a->vx[0] = a->vy[0] = 0;
                            a->act[0] = 1;
                        }
                        asp_render();
                        asp_status("YOUR TURN! AIM 0:FIRE", COL_CUR);
                    }
                }
            }
            h9_delay(10);
            continue;
        }

        /* Aiming: always my turn in solo. */
        if (a->state == ST_AIM && k != 0) {
            int need = 0;
            if (k == KEY_4) { a->aim = (uint8_t)((a->aim + 355) % 360); need = 1; }
            else if (k == KEY_6) { a->aim = (uint8_t)((a->aim + 5) % 360); need = 1; }
            else if (k == KEY_2 && a->power < 11) { a->power++; draw_hud_pwr(); }
            else if (k == KEY_8 && a->power > 2) { a->power--; draw_hud_pwr(); }
            else if (k == KEY_0 && a->act[0]) {
                int32_t spd = (int32_t)a->power * 170;
                a->vx[0] = (spd * (asp_cos(a->aim))) >> 10;
                a->vy[0] = (spd * (asp_sin(a->aim))) >> 10;
                a->started = 1;
                a->shots++;
                a->state = ST_MOVING;
                a->ftick = TICK;
                asp_draw_hud();
            }
            if (need) asp_render();
            h9_delay(40);
            continue;
        }

        h9_delay(10);
    }
}

/* Play the game loop: aim/shoot, run animation, send/receive SYNC. */
static void asp_play(void)
{
    struct asp *a = ASP;
    uint32_t ftick = TICK;
    for (;;) {
        unsigned k = asp_key();
        drain_ring();

        /* Handle an incoming SHOT while we are not animating (i.e. my turn or
         * idle). starts/plays identical local animation. */
        if ((!a->pending || !a->myturn) && pcmd == 'S' && pn >= 2 && a->state != ST_MOVING) {
            apply_incoming_shot();
            pcmd = 0;
        }
        /* Handle incoming SYNC snapshot (turn passes to us). */
        if (pcmd == 'Y' && pn >= 1 && psync_n >= 40) {
            apply_sync_hex(psync);
            a->myturn = 1;
            a->pending = 0;
            a->timeout = 0;
            a->ftick = TICK;
            pcmd = 0;
            asp_render();
            asp_status("YOUR TURN! AIM 0:FIRE", COL_CUR);
        }

        if (k == KEY_BACK) { if (a->pending) { a->pending = 0; a->myturn = 1; asp_status("SHOT CANCELLED", COL_MISS); k = 0; } else return; }

        if (a->state == ST_MOVING) {
            if ((uint32_t)(TICK - ftick) >= 2u) {
                ftick = TICK;
                int moving = asp_step();
                for (int i = 0; i < MAX_BALLS; i++) asp_redraw_ball(i);
                if (!moving) {
                    uint8_t outcome = a->state;   /* ST_WIN/ST_LOSE from asp_step */
                    a->state = ST_AIM;
                    if (a->pending) {
                        /* We shot: send authoritative SYNC, turn to opponent. */
                        char hex[42];
                        sync_hex(hex);
                        send_cmd("SYNC", -1, -1, hex);
                        a->pending = 0;
                        a->myturn = 0;
                        asp_status("WAITING OPPONENT SHOT", COL_MISS);
                    } else {
                        /* Receiver: opponent shot; await their SYNC. */
                        a->myturn = 0;
                        asp_status("AWAITING SYNC...", COL_MISS);
                    }
                    asp_render();
                    if (outcome == ST_WIN || outcome == ST_LOSE) {
                        gfx_fill(34, 56, 92, 18, GFX_BLACK);
                        if (outcome == ST_WIN)
                            gfx_text(62, 60, "WIN!", 6, 2, COL_SHIP, GFX_BLACK);
                        else
                            gfx_text(56, 60, "LOSE", 6, 2, GFX_RED, GFX_BLACK);
                        gfx_text(48, 80, "BACK: EXIT", 10, 1, GFX_GREY, GFX_BLACK);
                        while (h9_key() != H9_KEY_NONE) h9_delay(3);
                        for (;;) { if (asp_key() == KEY_BACK) return; h9_delay(10); }
                    }
                }
            }
            h9_delay(10);
            continue;
        }

        /* Aiming: only when it's my turn. */
        if (a->state == ST_AIM && a->myturn && k != 0) {
            int need = 0;
            if (k == KEY_4) { a->aim = (uint8_t)((a->aim + 355) % 360); need = 1; }
            else if (k == KEY_6) { a->aim = (uint8_t)((a->aim + 5) % 360); need = 1; }
            else if (k == KEY_2 && a->power < 11) { a->power++; draw_hud_pwr(); }
            else if (k == KEY_8 && a->power > 2) { a->power--; draw_hud_pwr(); }
            else if (k == KEY_0 && a->act[0]) { asp_radio_fire(); }
            if (need) asp_render();
            h9_delay(40);
            continue;
        }

        h9_delay(10);
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    struct asp *a;
    (void)api;
    (void)self;
    a = ASP;
    {
        /* Keep the mode chosen in the menu across the module hand-off. */
        uint8_t keep_mode = a->mode;
        memset(a, 0, sizeof *a);
        a->mode = keep_mode;
    }
    a->room[0] = '0';
    a->room[1] = '1';
    asp_load_call();
    a->klast = a->kseen = (uint8_t)h9_key();
    a->ktick = 0;
    if (!asp_select_room()) return 0;
    if (a->mode == 'R') asp_radio_setup();
    asp_reset_rack();
    a->myturn = 1;
    a->started = 0;
    a->pending = 0;
    a->aim = 0;
    a->power = 7;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    asp_render();
    asp_status("YOUR TURN! AIM 0:FIRE", COL_CUR);
    if (a->mode == 'S') asp_play_solo();
    else asp_play();
    return 0;
}