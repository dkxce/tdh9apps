/* .TRK RADAR: north-up dial, shown with or without a fix. Red north
 * triangle above the dial, recording status top left, SATS / PTS bottom
 * left. Arrow to the target green with a fix, red without; pale grey arrow
 * for the radio's own course (with the 2 km/h filter on, only while moving).
 * Keys: Up / Down = Map, 5 = Points, 0 = full screen / split with values,
 * * = dial labels (N E S W / 12 03 06 09 / 00 09 18 27), Back = TRACKER. */
#include "trk.h"

enum { CY = 64, R = 50, NONE = 999 };

static void dial(const struct trk *s, int cx, unsigned target, uint16_t tcol, unsigned course)
{
    static const char *const L[3][4] = {
        { "N", "E", "S", "W" }, { "12", "03", "06", "09" }, { "00", "09", "18", "27" } };
    view_frame(s->radar_full);
    view_circle(cx, CY, R, GFX_GREY);
    gfx_fill((unsigned)(cx - R), CY, 2 * R + 1, 1, C_DGREY);
    gfx_fill((unsigned)cx, CY - R, 1, 2 * R + 1, C_DGREY);
    for (unsigned i = 0; i < 6; i++)   /* north triangle, apex up */
        gfx_fill((unsigned)cx - i, CY - R - 11 + i, 2 * i + 1, 1, GFX_RED);
    for (unsigned i = 0; i < 4; i++) {
        int x, y;
        const char *t = L[s->radar_mode][i];
        view_spoke(cx, CY, i * 90, R - 8, &x, &y);
        gfx_text((unsigned)(x - (int)text_len(t) * 3), (unsigned)(y - 4), t, 2, 1, GFX_WHITE, GFX_BLACK);
    }
    if (course != NONE) view_arrow(cx, CY, course, R - 14, C_LGREY);
    if (target != NONE) view_arrow(cx, CY, target, R - 12, tcol);
}

static void panel(const struct trk *s, int nav, int32_t dx, int32_t dy, int live)
{
    char b[12];
    if (nav) km_text(b, nav_distance(dx, dy)); else put(b, "---");
    view_value(0, "DST KM", b, GFX_WHITE);
    if (trk_heading_ok()) num(b, s->course, 3); else put(b, "---");
    view_value(1, "HDG", b, GFX_WHITE);
    if (nav) num(b, nav_bearing(dx, dy), 3); else put(b, "---");
    view_value(2, "BRG", b, GFX_YELLOW);
    if (s->have_fix) fixed(b, (int32_t)(s->spd10 * 1852u / 1000u), 1); else put(b, "---");
    view_value(3, "SPD KMH", b, GFX_WHITE);
    if (s->have_fix) deg_text(b, s->lat); else put(b, "---");
    view_value(4, "LAT", b, live ? GFX_WHITE : GFX_GREY);
    if (s->have_fix) deg_text(b, s->lon); else put(b, "---");
    view_value(5, "LON", b, live ? GFX_WHITE : GFX_GREY);
    if (s->have_fix) fixed(b, s->alt10 / 10, 0); else put(b, "---");
    view_value(6, "ALT M", b, GFX_WHITE);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    unsigned shown_t = NONE + 1, shown_c = NONE + 1, shown_rec = 9, dirty = 1;
    uint16_t shown_col = 0;
    uint32_t last = TICK - 100;
    for (;;) {
        trk_service();
        unsigned k = trk_key();
        if (k == H9_KEY_BACK) { trk_run("TRACKER"); return 0; }
        if (k == H9_KEY_UP || k == H9_KEY_DOWN) { trk_run(".TRK MAP"); return 0; }
        if (k == KEY_5) { s->from = FROM_RADAR; trk_run(".TRK POINTS"); return 0; }
        if (k == KEY_STAR) { s->radar_mode = (uint8_t)((s->radar_mode + 1) % 3); dirty = 1; }
        if (k == KEY_0) { s->radar_full = (uint8_t)!s->radar_full; dirty = 1; }

        uint32_t now = TICK;
        if (dirty || now - last >= 50) {
            char b[24], *d;
            int live = trk_live();
            int cx = s->radar_full ? H9_WIDTH / 2 : 52;
            int32_t dx = 0, dy = 0;
            int nav = s->have_fix && s->have_target;
            if (nav) nav_delta(s->lat, s->lon, s->tlat, s->tlon, &dx, &dy);
            unsigned brg = nav ? nav_bearing(dx, dy) : NONE;
            unsigned course = trk_heading_ok() ? s->course : NONE;
            uint16_t col = live ? GFX_GREEN : GFX_RED;
            if (dirty || brg != shown_t || course != shown_c || col != shown_col) {
                dial(s, cx, brg, col, course);
                shown_t = brg; shown_c = course; shown_col = col;
                shown_rec = 9;
            }
            if (s->rec != shown_rec) { view_status(2, 2); shown_rec = s->rec; }
            d = put(b, "SATS:");
            d = num(d, s->sats, 1);
            d = put(d, " PTS:");
            d = num(d, s->count, 1);
            put(d, "  ");
            gfx_text(2, 119, b, 17, 1, GFX_WHITE, GFX_BLACK);
            if (!s->radar_full) panel(s, nav, dx, dy, live);
            last = now;
            dirty = 0;
        }
        h9_delay(10);
    }
}
