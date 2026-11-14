/* .TRK POINTS: recorded points, each with the distance and time from the
 * start of tracking; Up / Down wrap. Menu on a point opens POINT (number,
 * distance, elapsed, clock time, distance from here, SHOW on the map,
 * NAVIGATE to it). Back returns to the Radar / Map it was opened from with
 * key 5, otherwise to TRACKER. */
#include "trk.h"

static void point_row(unsigned i, char *o)
{
    o = km_text(o, trk_point_dist(i));
    o = put(o, "KM ");
    time_text(o, TRK->p[i].t * 2u);
}

static char *clock_text(char *o, int32_t t)
{
    if (t < 0) return put(o, "--:--:--");
    o = num(o, (unsigned)(t / 3600), 2);
    *o++ = ':';
    o = num(o, (unsigned)(t / 60 % 60), 2);
    *o++ = ':';
    return num(o, (unsigned)(t % 60), 2);
}

static void point_item(unsigned i, char *o)
{
    struct trk *s = TRK;
    unsigned p = (unsigned)s->cur;
    if (i == 0) { o = put(o, "NUMBER "); num(o, p + 1, 1); }
    else if (i == 1) { o = put(o, "DIST "); o = km_text(o, trk_point_dist(p)); put(o, "KM"); }
    else if (i == 2) { o = put(o, "ELAPSED "); time_text(o, s->p[p].t * 2u); }
    else if (i == 3) { o = put(o, "TIME "); clock_text(o, trk_point_clock(p)); }
    else if (i == 4) {
        o = put(o, "EST DIST ");
        if (s->have_fix) {
            int32_t ll[2];
            trk_point(p, ll);
            o = km_text(o, dist_between(s->lat, s->lon, ll[0], ll[1]));
            put(o, "KM");
        } else put(o, "---");
    }
    else put(o, i == 5 ? "SHOW" : "NAVIGATE");
}

static void leave(const struct trk *s)
{
    trk_run(s->from == FROM_RADAR ? ".TRK RADAR" : s->from == FROM_MAP ? ".TRK MAP" : "TRACKER");
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    if (!s->count) {
        trk_msg("<NO POINTS>", 0, 0, 100);
        leave(s);
        return 0;
    }
    if (s->cur < 0 || s->cur >= (int)s->count) s->cur = 0;
    for (;;) {
        int r = menu_run("POINTS", s->count, (unsigned)s->cur, point_row);
        if (r < 0) { leave(s); return 0; }
        s->cur = (int16_t)r;
        unsigned sel = 0;
        for (;;) {
            int c = menu_run("POINT", 7, sel, point_item);
            if (c < 0) break;
            sel = (unsigned)c;
            if (c == 5) { s->show = s->cur; trk_run(".TRK MAP"); return 0; }
            if (c == 6) {
                int32_t ll[2];
                trk_point((unsigned)s->cur, ll);
                s->tlat = ll[0];
                s->tlon = ll[1];
                s->have_target = 1;
                trk_run(".TRK RADAR");
                return 0;
            }
        }
    }
}
