/* .TRK POINTS: recorded points, each with the distance and time from the
 * start of tracking; Up / Down wrap. Menu on a point opens POINT (number,
 * distance, elapsed, clock time, distance from here, SHOW on the map,
 * NAVIGATE to it). Back returns to the Radar / Map it was opened from with
 * key 5, otherwise to TRACKER. */
#include "trk.h"

static void point_row(unsigned i, char *o)
{
    struct trk *s = TRK;
    if (s->count > 1 && i == 0) { put(o, "START POINT"); return; }
    unsigned p = s->count > 1 ? i - 1u : i;
    o = km_text(o, trk_point_dist(p));
    o = put(o, "KM ");
    time_text(o, s->p_t[p] * 2u);
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
    int is_start = s->count > 1 && s->cur == 0;
    int p = s->count > 1 ? s->cur - 1 : s->cur;
    int32_t plat = is_start ? s->slat : s->p_lat[p];
    int32_t plon = is_start ? s->slon : s->p_lon[p];
    if (i == 0) { o = put(o, is_start ? "START POINT" : "NUMBER "); if (!is_start) num(o, (unsigned)p + 1, 1); }
    else if (i == 1) {
        o = put(o, "DIST ");
        o = km_text(o, is_start ? 0u : trk_point_dist((unsigned)p));
        put(o, "KM");
    }
    else if (i == 2) { o = put(o, "ELAPSED "); time_text(o, is_start ? 0u : s->p_t[p] * 2u); }
    else if (i == 3) { o = put(o, "TIME "); clock_text(o, trk_point_clock(is_start ? -1 : p)); }
    else if (i == 4) {
        o = put(o, "EST DIST ");
        if (s->have_fix) {
            int32_t dx, dy;
            nav_delta(s->lat, s->lon, plat, plon, &dx, &dy);
            o = km_text(o, nav_distance(dx, dy));
            put(o, "KM");
        } else put(o, "---");
    }
    else if (i == 5) { if (is_start) put(o, "SHOW ---"); else put(o, "SHOW"); }
    else put(o, "NAVIGATE");
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
        unsigned list_n = s->count + (s->count > 1 ? 1u : 0u);
        if ((unsigned)s->cur >= list_n) s->cur = 0;
        int r = menu_run("POINTS", (int)list_n, (unsigned)s->cur, point_row);
        if (r < 0) { leave(s); return 0; }
        s->cur = (int16_t)r;
        unsigned sel = 0;
        for (;;) {
            int c = menu_run("POINT", 7, sel, point_item);
            if (c < 0) break;
            sel = (unsigned)c;
            int is_start = s->count > 1 && s->cur == 0;
            int p = s->count > 1 ? s->cur - 1 : s->cur;
            if (c == 5 && !is_start) { s->show = (int16_t)p; trk_run(".TRK MAP"); return 0; }
            if (c == 6) {
                s->tlat = is_start ? s->slat : s->p_lat[p];
                s->tlon = is_start ? s->slon : s->p_lon[p];
                s->have_target = 1;
                trk_run(".TRK RADAR");
                return 0;
            }
        }
    }
}
