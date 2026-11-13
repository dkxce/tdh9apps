/* .TRK RADAR: north-up dial, shown with or without a fix.
 * Arrow to the target green with a fix, red without; pale grey arrow for
 * the radio's own course while moving. * cycles the dial labels
 * (N E S W / 12 03 06 09 / 00 09 18 27); Up = Points, Down = Map,
 * Back = TRACKER. */
#include "trk.h"

enum { CX = 52, CY = 64, R = 50, NONE = 999 };

static void dial(const struct trk *s, unsigned target, uint16_t tcol, unsigned course)
{
    static const char *const L[3][4] = {
        { "N", "E", "S", "W" }, { "12", "03", "06", "09" }, { "00", "09", "18", "27" } };
    gfx_fill(0, 0, VIEW_W, H9_HEIGHT, GFX_BLACK);
    view_circle(CX, CY, R, GFX_GREY);
    gfx_fill(CX - R, CY, 2 * R + 1, 1, C_DGREY);
    gfx_fill(CX, CY - R, 1, 2 * R + 1, C_DGREY);
    for (unsigned i = 0; i < 4; i++) {
        int x, y;
        const char *t = L[s->radar_mode][i];
        view_spoke(CX, CY, i * 90, R - 8, &x, &y);
        gfx_text((unsigned)(x - (int)text_len(t) * 3), (unsigned)(y - 4), t, 2, 1, GFX_WHITE, GFX_BLACK);
    }
    if (course != NONE) view_arrow(CX, CY, course, R - 14, C_LGREY);
    if (target != NONE) view_arrow(CX, CY, target, R - 12, tcol);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    unsigned shown_t = NONE + 1, shown_c = NONE + 1, shown_mode = 9;
    uint16_t shown_col = 0;
    uint32_t last = TICK - 100;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    for (;;) {
        trk_service();
        unsigned k = trk_key();
        if (k == H9_KEY_BACK) { trk_run("TRACKER"); return 0; }
        if (k == H9_KEY_UP) { trk_run(".TRK POINTS"); return 0; }
        if (k == H9_KEY_DOWN) { trk_run(".TRK MAP"); return 0; }
        if (k == KEY_STAR) { s->radar_mode = (uint8_t)((s->radar_mode + 1) % 3); last = TICK - 100; }

        uint32_t now = TICK;
        if (now - last >= 50) {
            char b[12];
            int live = trk_live();
            int32_t dx = 0, dy = 0;
            int nav = s->have_fix && s->have_target;
            if (nav) nav_delta(s->lat, s->lon, s->tlat, s->tlon, &dx, &dy);
            unsigned brg = nav ? nav_bearing(dx, dy) : NONE;
            unsigned course = live && s->spd10 >= 11 ? s->course : NONE;   /* about 2 km/h */
            uint16_t col = live ? GFX_GREEN : GFX_RED;
            if (brg != shown_t || course != shown_c || col != shown_col || s->radar_mode != shown_mode) {
                dial(s, brg, col, course);
                shown_t = brg; shown_c = course; shown_col = col; shown_mode = s->radar_mode;
            }
            if (nav) km_text(b, nav_distance(dx, dy)); else put(b, "---");
            view_value(0, "DST KM", b, GFX_WHITE);
            if (s->have_fix) num(b, s->course, 3); else put(b, "---");
            view_value(1, "HDG", b, GFX_WHITE);
            if (nav) num(b, brg, 3); else put(b, "---");
            view_value(2, "BRG", b, GFX_YELLOW);
            if (s->have_fix) fixed(b, (int32_t)(s->spd10 * 1852u / 1000u), 1); else put(b, "---");
            view_value(3, "SPD KMH", b, GFX_WHITE);
            if (s->have_fix) deg_text(b, s->lat); else put(b, "---");
            view_value(4, "LAT", b, live ? GFX_WHITE : GFX_GREY);
            if (s->have_fix) deg_text(b, s->lon); else put(b, "---");
            view_value(5, "LON", b, live ? GFX_WHITE : GFX_GREY);
            if (s->have_fix) fixed(b, s->alt10 / 10, 0); else put(b, "---");
            view_value(6, "ALT M", b, GFX_WHITE);
            last = now;
        }
        h9_delay(10);
    }
}
