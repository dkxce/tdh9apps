/* APRSPOOL: visible module, menu. 1 runs local BALLPOOL, 2 hands off to the
 * .PL RADIO APRS game. Mirror of seafight.c's menu. */
#include "asp.h"

unsigned module_main(const struct h9_api *api, unsigned self)
{
    struct asp *a;
    (void)api;
    (void)self;
    a = asp_open();
    if (!a) return 0;
    memset(a, 0, sizeof *a);
    a->room[0] = '0';
    a->room[1] = '1';

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(14, 8, "-- APRS POOL --", 20, 1, 0x07FF, GFX_BLACK);
    gfx_text(14, 28, "1: PLAY BALLPOOL", 20, 1, COL_SHIP, GFX_BLACK);
    gfx_text(14, 42, "2: APRS RADIO LINK", 20, 1, 0xF81F, GFX_BLACK);
    gfx_text(8, 62, "(c) dkxce 2026", 18, 1, 0x8410, GFX_BLACK);
    gfx_text(8, 74, "https://github.com/dkxce", 25, 1, 0x8410, GFX_BLACK);
    gfx_text(8, 94, "IF APRS FAILS RE-POWER", 25, 1, 0x8410, GFX_BLACK);
    gfx_text(90, 112, "BACK: EXIT", 12, 1, GFX_RED, GFX_BLACK);
    a->klast = a->kseen = (uint8_t)h9_key();
    a->ktick = 0;

    for (;;) {
        unsigned k = asp_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_1 || k == KEY_2 || k == KEY_5) {
            a->mode = (k == KEY_2) ? 'R' : 'S';   /* 2 = APRS radio, else solo */
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
}