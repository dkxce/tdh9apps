/* .TRK MAP: full-height north-up map left of the value panel, shown with or
 * without a fix. Start green, target red, track cyan, own position white,
 * the point picked in POINTS (SHOW) fuchsia. Top left: north pointer and
 * NAV (arrow to the target). * / # scale 100M..5000KM per 40 px bar;
 * Up = Radar, Down = Points, Back = TRACKER. */
#include "trk.h"

enum { MX = VIEW_W / 2, MY = H9_HEIGHT / 2, BAR = 40, FAR = 600, ZOOMS = 15 };

static const uint32_t ZOOM_M[ZOOMS] = { 100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000,
                                        250000, 500000, 1000000, 2500000, 5000000 };

struct view { int32_t lat, lon, scale; };

/* Screen position of a point; 0 when it is too far off the view to draw a line to. */
static int to_xy(const struct view *v, int32_t lat, int32_t lon, int *x, int *y)
{
    int32_t dx, dy;
    nav_delta(v->lat, v->lon, lat, lon, &dx, &dy);
    if (dx > 20000000 || dx < -20000000 || dy > 20000000 || dy < -20000000) return 0;
    *x = MX + (int)(dx * BAR / v->scale);
    *y = MY - (int)(dy * BAR / v->scale);
    return *x > -FAR && *x < FAR && *y > -FAR && *y < FAR;
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
    char b[12];
    v.scale = (int32_t)ZOOM_M[s->zoom];
    if (s->show >= 0 && s->show < (int)s->count) { v.lat = s->p_lat[s->show]; v.lon = s->p_lon[s->show]; }
    else if (s->have_fix) { v.lat = s->lat; v.lon = s->lon; }
    else if (s->has_start) { v.lat = s->slat; v.lon = s->slon; }
    else { v.lat = s->tlat; v.lon = s->tlon; }

    gfx_fill(0, 0, VIEW_W, H9_HEIGHT, GFX_BLACK);
    for (unsigned i = 0; i < s->count; i++) {
        int ok = to_xy(&v, s->p_lat[i], s->p_lon[i], &x, &y);
        if (ok && prev) view_line(px, py, x, y, C_CYAN);
        else if (ok) view_px(x, y, C_CYAN);
        px = x; py = y; prev = ok;
    }
    if (s->has_start) mark(&v, s->slat, s->slon, 3, GFX_GREEN);
    if (s->have_target) mark(&v, s->tlat, s->tlon, 3, GFX_RED);
    if (s->show >= 0 && s->show < (int)s->count) mark(&v, s->p_lat[s->show], s->p_lon[s->show], 5, C_FUCHSIA);
    if (s->have_fix && to_xy(&v, s->lat, s->lon, &x, &y)) {
        view_line(x - 3, y, x + 3, y, GFX_WHITE);
        view_line(x, y - 3, x, y + 3, GFX_WHITE);
    }

    /* north pointer and NAV arrow */
    view_line(4, 14, 4, 2, GFX_WHITE);
    view_line(4, 2, 1, 6, GFX_WHITE);
    view_line(4, 2, 7, 6, GFX_WHITE);
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
    dist_label(b, ZOOM_M[s->zoom]);
    gfx_text(4, 108, b, 6, 1, GFX_WHITE, GFX_BLACK);
}

static void draw_panel(const struct trk *s)
{
    char b[12];
    int live = trk_live();
    int32_t dx = 0, dy = 0;
    int nav = s->have_fix && s->have_target;
    if (nav) nav_delta(s->lat, s->lon, s->tlat, s->tlon, &dx, &dy);
    view_value(0, "FIX", live ? "OK" : "BAD", live ? GFX_GREEN : GFX_RED);
    if (nav) km_text(b, nav_distance(dx, dy)); else put(b, "---");
    view_value(1, "DST KM", b, GFX_WHITE);
    if (s->have_fix) num(b, s->course, 3); else put(b, "---");
    view_value(2, "HDG", b, GFX_WHITE);
    if (nav) num(b, nav_bearing(dx, dy), 3); else put(b, "---");
    view_value(3, "BRG", b, GFX_YELLOW);
    if (s->have_fix) fixed(b, (int32_t)(s->spd10 * 1852u / 1000u), 1); else put(b, "---");
    view_value(4, "SPD KMH", b, GFX_WHITE);
    num(b, s->count, 1);
    view_value(5, "PTS", b, C_CYAN);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    uint32_t last = TICK - 100, drawn_fix = s->fix_tick - 1;
    unsigned drawn_count = 0xFFFF, dirty = 1;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    for (;;) {
        trk_service();
        unsigned k = trk_key();
        if (k == H9_KEY_BACK || k == H9_KEY_UP || k == H9_KEY_DOWN) {
            s->show = -1;
            trk_run(k == H9_KEY_BACK ? "TRACKER" : k == H9_KEY_UP ? ".TRK RADAR" : ".TRK POINTS");
            return 0;
        }
        if (k == KEY_STAR && s->zoom) { s->zoom--; dirty = 1; }
        if (k == KEY_HASH && s->zoom + 1 < ZOOMS) { s->zoom++; dirty = 1; }

        uint32_t now = TICK;
        if (s->fix_tick != drawn_fix || s->count != drawn_count) dirty = 1;
        if (dirty) {
            draw_map(s);
            drawn_fix = s->fix_tick;
            drawn_count = s->count;
            dirty = 0;
            last = now - 100;
        }
        if (now - last >= 50) { draw_panel(s); last = now; }
        h9_delay(10);
    }
}
