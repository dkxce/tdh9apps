/* APRSPOOL: visible module. Local table; 2 hands off to .PL RADIO. */
#include "pl.h"

static void local_shot(void)
{
    struct pl *s = PL;
    pl_strike(s->aim, s->power);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    struct pl *s;
    (void)api;
    (void)self;
    s = pl_open();
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    s->room[0] = '0';
    s->room[1] = '1';

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(14, 8, "APRS 8-BALL POOL", 18, 1, 0x07FF, GFX_BLACK);
    gfx_text(14, 28, "1: PLAY LOCAL", 18, 1, 0x07E0, GFX_BLACK);
    gfx_text(14, 42, "2: APRS RADIO LINK", 18, 1, 0xF81F, GFX_BLACK);
    gfx_text(14, 58, "(c) dkxce 2026", 18, 1, 0x8410, GFX_BLACK);
    gfx_text(14, 76, "https://github.com/dkxce", 25, 1, 0x8410, GFX_BLACK);
    gfx_text(14, 88, "IF APRS FAILS RE-POWER", 25, 1, 0x8410, GFX_BLACK);
    gfx_text(14, 108, "BACK: EXIT", 12, 1, GFX_RED, GFX_BLACK);
    s->klast = s->kseen = (uint8_t)h9_key();
    for (;;) {
        unsigned k = pl_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_1) { s->aprs = 0; break; }
        if (k == KEY_2) {
            volatile struct h9_request *rq = H9_REQUEST;
            unsigned i = 0;
            const char *name = ".PL RADIO";
            for (; i < H9_NAME_BYTES && name[i]; i++) rq->name[i] = name[i];
            for (; i < H9_NAME_BYTES; i++) rq->name[i] = ' ';
            rq->magic = H9_REQUEST_MAGIC;
            return 0;
        }
        h9_delay(10);
    }
    pl_reset();
    pl_play(local_shot, 0, 0);
    return 0;
}
