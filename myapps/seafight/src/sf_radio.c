/* .SF RADIO: hidden module, room + APRS game. */
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
    s->aprs = 1;
    sf_load_call();
    s->klast = s->kseen = (uint8_t)h9_key();
    if (!sf_select_room()) return 0;
    sf_radio_setup();
    sf_init();
    sf_board();
    sf_status("YOUR TURN! FIRE", COL_CUR);
    sf_play(sf_radio_fire, sf_radio_tick);
    return 0;
}
