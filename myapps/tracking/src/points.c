/* .TRK POINTS: recorded points, each with the distance and time from the
 * start of tracking. Menu on a point opens POINT (number, distance, time,
 * SHOW on the map, NAVIGATE to it). Up on the first point = Map, Back =
 * TRACKER. */
#include "trk.h"

static void point_row(unsigned i, char *o)
{
    o = km_text(o, trk_point_dist(i));
    o = put(o, "KM ");
    time_text(o, TRK->p_t[i] * 2u);
}

static void point_item(unsigned i, char *o)
{
    struct trk *s = TRK;
    unsigned p = (unsigned)s->cur;
    if (i == 0) { o = put(o, "NUMBER "); num(o, p + 1, 1); }
    else if (i == 1) { o = put(o, "DIST "); o = km_text(o, trk_point_dist(p)); put(o, "KM"); }
    else if (i == 2) { o = put(o, "TIME "); time_text(o, s->p_t[p] * 2u); }
    else put(o, i == 3 ? "SHOW" : "NAVIGATE");
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    if (!s->count) {
        trk_msg("<NO POINTS>", 0, 0, 100);
        trk_run("TRACKER");
        return 0;
    }
    for (;;) {
        int r = menu_run("POINTS", s->count, (unsigned)s->cur, point_row, 1);
        if (r == MENU_TOP) { trk_run(".TRK MAP"); return 0; }
        if (r < 0) { trk_run("TRACKER"); return 0; }
        s->cur = (int16_t)r;
        unsigned sel = 0;
        for (;;) {
            int c = menu_run("POINT", 5, sel, point_item, 0);
            if (c < 0) break;
            sel = (unsigned)c;
            if (c == 3) { s->show = s->cur; trk_run(".TRK MAP"); return 0; }
            if (c == 4) {
                s->tlat = s->p_lat[s->cur];
                s->tlon = s->p_lon[s->cur];
                s->have_target = 1;
                trk_run(".TRK RADAR");
                return 0;
            }
        }
    }
}
