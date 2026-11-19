/* .PL RADIO: hidden module, room + APRS table. */
#include "pl.h"

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
    s->aprs = 1;
    s->turn = 1;
    pl_load_call();
    s->klast = s->kseen = (uint8_t)h9_key();
    if (!pl_select_room()) return 0;
    pl_radio_setup();
    pl_reset();
    s->turn = 1;
    pl_play(pl_radio_shot, pl_radio_tick, pl_radio_rest);
    return 0;
}
