/* SEAFIGHT: visible module. Menu, vs CPU; 2 hands off to .SF RADIO. */
#include "sf.h"

unsigned module_main(const struct h9_api *api, unsigned self)
{
    struct sf *s;
    (void)api;
    (void)self;
    s = sf_open();
    if (!s) return 0;
    memset(s, 0, sizeof *s);
    s->room[0] = '0';
    s->room[1] = '1';

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    gfx_text(22, 14, "BATTLESHIP", 16, 1, 0x07FF, GFX_BLACK);
    gfx_text(14, 54, "1: PLAY VS CPU", 18, 1, COL_SHIP, GFX_BLACK);
    gfx_text(14, 78, "2: APRS RADIO LINK", 18, 1, 0xF81F, GFX_BLACK);
    gfx_text(14, 102, "BACK: EXIT", 12, 1, GFX_RED, GFX_BLACK);
    s->klast = s->kseen = (uint8_t)h9_key();
    for (;;) {
        unsigned k = sf_key();
        if (k == KEY_BACK) return 0;
        if (k == KEY_1) { s->aprs = 0; break; }
        if (k == KEY_2) {
            sf_run(".SF RADIO");
            return 0;
        }
        h9_delay(10);
    }

    sf_init();
    sf_board();
    sf_status("YOUR TURN! FIRE", COL_CUR);
    sf_play(sf_cpu_fire, 0);
    return 0;
}
