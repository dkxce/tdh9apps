/* .TRK MAP: north-up map, shown with or without a fix; full screen or split
 * (map left, values right, divided). Start green, target red, track cyan,
 * own position white, the point picked in POINTS (SHOW) fuchsia. Top left:
 * north pointer and NAV (arrow to the target); recording status top right.
 * Keys: Up / Down = Radar, 5 = Points, 0 = full screen / split,
 * * / # = scale 50M..5000KM per 40 px bar, Menu = start / pause / resume
 * recording, Back = TRACKER. */
#include "trk.h"

enum { MY = H9_HEIGHT / 2, BAR = 40, FAR = 600, ZOOMS = 16 };

/* Bar length in metres: 50, 100, 250, 500, 1000, ... 5000000 */
static uint32_t zoom_m(unsigned i)
{
    static const uint8_t M[3] = { 1, 2, 5 };
    uint32_t v = 50u * M[i % 3];
    for (i /= 3; i; i--) v *= 10;
    return v;
}

struct view { int32_t lat, lon, scale; int mx; };

/* Screen position of a point; 0 when it is too far off the view to draw a line to. */
static int to_xy(const struct view *v, int32_t lat, int32_t lon, int *x, int *y)
{
    int32_t dx, dy;
    nav_delta(v->lat, v->lon, lat, lon, &dx, &dy);
    if ((uint32_t)dx + 20000000u > 40000000u || (uint32_t)dy + 20000000u > 40000000u) return 0;
    *x = v->mx + (int)(dx * BAR / v->scale);
    *y = MY - (int)(dy * BAR / v->scale);
    return (unsigned)*x + FAR < 2u * FAR && (unsigned)*y + FAR < 2u * FAR;
}

static void mark(const struct view *v, int32_t lat, int32_t lon, int r, uint16_t c)
{
    int x, y;
    if (to_xy(v, lat, lon, &x, &y)) view_circle(x, y, r, c);
}

static void draw_map(const struct trk *s)
{
    struct view v;
    int x = 0, y = 0, px = 0, py = 0, prev = 0;
    int show = s->show >= 0 && s->show < (int)s->count;
    char b[12];
    view_frame(s->map_full);
    v.mx = s->view_w / 2;
    v.scale = (int32_t)zoom_m(s->zoom);
    if (show) { v.lat = s->p_lat[s->show]; v.lon = s->p_lon[s->show]; }
    else if (s->have_fix) { v.lat = s->lat; v.lon = s->lon; }
    else if (s->has_start) { v.lat = s->slat; v.lon = s->slon; }
    else { v.lat = s->tlat; v.lon = s->tlon; }

    for (unsigned i = 0; i < s->count; i++) {
        int ok = to_xy(&v, s->p_lat[i], s->p_lon[i], &x, &y);
        if (ok && prev) view_line(px, py, x, y, C_CYAN);
        else if (ok) view_px(x, y, C_CYAN);
        px = x; py = y; prev = ok;
    }
    if (s->has_start) mark(&v, s->slat, s->slon, 3, GFX_GREEN);
    if (s->have_target) mark(&v, s->tlat, s->tlon, 3, GFX_RED);
    if (show) mark(&v, s->p_lat[s->show], s->p_lon[s->show], 5, C_FUCHSIA);
    if (s->have_fix && to_xy(&v, s->lat, s->lon, &x, &y)) {
        view_line(x - 3, y, x + 3, y, GFX_WHITE);
        view_line(x, y - 3, x, y + 3, GFX_WHITE);
    }

    /* north pointer and NAV arrow */
    view_arrow(4, 14, 0, 12, GFX_WHITE);
    gfx_text(9, 2, "N", 1, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(2, 18, "NAV", 3, 1, GFX_WHITE, GFX_BLACK);
    if (s->have_fix && s->have_target) {
        int32_t dx, dy;
        nav_delta(s->lat, s->lon, s->tlat, s->tlon, &dx, &dy);
        view_arrow(28, 21, nav_bearing(dx, dy), 7, trk_live() ? GFX_GREEN : GFX_RED);
    }

    /* scale bar */
    gfx_fill(4, 122, BAR + 1, 1, GFX_WHITE);
    gfx_fill(4, 118, 1, 4, GFX_WHITE);
    gfx_fill(4 + BAR, 118, 1, 4, GFX_WHITE);
    dist_label(b, (uint32_t)v.scale);
    gfx_text(4, 108, b, 6, 1, GFX_WHITE, GFX_BLACK);
}

static void draw_panel(const struct trk *s)
{
    char b[12], *d;
    int live = trk_live();
    int32_t dx = 0, dy = 0;
    int nav = s->have_fix && s->have_target;
    if (nav) nav_delta(s->lat, s->lon, s->tlat, s->tlon, &dx, &dy);
    d = put(b, live ? "OK:" : "BAD:");
    num(d, s->sats, 1);
    view_value(0, "FIX", b, live ? GFX_GREEN : GFX_RED);
    view_value(1, "DST KM", nav ? (km_text(b, nav_distance(dx, dy)), b) : 0, GFX_WHITE);
    view_value(2, "HDG", trk_heading_ok() ? (num(b, s->course, 3), b) : 0, GFX_WHITE);
    view_value(3, "BRG", nav ? (num(b, nav_bearing(dx, dy), 3), b) : 0, GFX_YELLOW);
    view_value(4, "ALT M", s->have_fix ? (fixed(b, s->alt10 / 10, 0), b) : 0, GFX_WHITE);
    view_value(5, "SPD KMH", s->have_fix ? (fixed(b, (int32_t)(s->spd10 * 1852u / 1000u), 1), b) : 0, GFX_WHITE);
    num(b, s->count, 1);
    view_value(6, "PTS", b, C_CYAN);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    uint32_t last = TICK - 100, drawn_fix = s->fix_tick - 1;
    unsigned drawn_count = 0xFFFF, dirty = 1, shown_rec = 9;
    for (;;) {
        trk_service();
        unsigned k = trk_key();
        if (k == H9_KEY_BACK || k == H9_KEY_UP || k == H9_KEY_DOWN || k == KEY_5) {
            s->show = -1;
            if (k == KEY_5) s->from = FROM_MAP;
            trk_run(k == H9_KEY_BACK ? "TRACKER" : k == KEY_5 ? ".TRK POINTS" : ".TRK RADAR");
            return 0;
        }
        if (k == KEY_STAR && s->zoom) { s->zoom--; dirty = 1; }
        if (k == KEY_HASH && s->zoom + 1 < ZOOMS) { s->zoom++; dirty = 1; }
        if (k == KEY_0) { s->map_full = (uint8_t)!s->map_full; dirty = 1; }
        if (k == H9_KEY_MENU) view_rec_key();

        uint32_t now = TICK;
        if (s->fix_tick != drawn_fix || s->count != drawn_count) dirty = 1;
        if (dirty) {
            draw_map(s);
            drawn_fix = s->fix_tick;
            drawn_count = s->count;
            dirty = 0;
            shown_rec = 9;
            last = now - 100;
        }
        if (now - last >= 50) {
            if (!s->map_full) { draw_panel(s); shown_rec = 9; }
            last = now;
        }
        if (s->rec != shown_rec) { view_status(H9_WIDTH - 11, 1); shown_rec = s->rec; }
        h9_delay(10);
    }
}
