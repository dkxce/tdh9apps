/* .TRK SETUP: menu SETTINGS (store points, step distance, 2 km/h heading
 * filter, step time, step mode, clear log). Back returns to TRACKER. */
#include "trk.h"

static void settings_label(unsigned i, char *o)
{
    struct trk *s = TRK;
    if (i == 0) { o = put(o, "STORE PTS "); num(o, s->cap, 1); }
    else if (i == 1) { o = put(o, "STEP DIST "); dist_label(o, STEP_M[s->step]); }
    else if (i == 2) put(o, s->hdg_filter ? "FILTER 2KMH ON" : "FILTER 2KMH OFF");
    else if (i == 3) { o = put(o, "STEP TIME "); o = num(o, s->step_time, 1); put(o, "S"); }
    else if (i == 4) put(o, s->step_mode == STEP_TIME ? "STEP MODE TIME" : "STEP MODE DIST");
    else put(o, "CLEAR LOG");
}

static void cap_label(unsigned i, char *o)
{
    o = num(o, (i + 1) * 40, 1);
    put(o, TRK->cap == (i + 1) * 40 ? " PTS <" : " PTS");
}

static void step_label(unsigned i, char *o)
{
    o = dist_label(o, STEP_M[i]);
    if (TRK->step == i) put(o, " <");
}

static void mode_label(unsigned i, char *o)
{
    o = put(o, i ? "TIME" : "DIST");
    if (TRK->step_mode == i) put(o, " <");
}

/* Seconds 15..3600: digits type the number (the first one replaces it),
 * Up / Down +-5, Menu OK (out-of-range values are clamped), Back cancel. */
static void edit_time(struct trk *s)
{
    unsigned v = s->step_time, dirty = 1, typed = 0;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    menu_title("STEP TIME");
    gfx_text(4, 86, "15..3600 SECONDS", 16, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(4, 98, "0-9 TYPE  UP/DN +-5", 19, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(4, 110, "MENU OK  BACK CANCEL", 20, 1, GFX_WHITE, GFX_BLACK);
    for (;;) {
        trk_service();
        if (dirty) {
            char b[12], *d = num(b, v, 1);
            put(d, "S    ");
            gfx_text(30, 44, b, 6, 2, GFX_WHITE, GFX_BLACK);
            dirty = 0;
        }
        unsigned k = trk_key();
        if (k == H9_KEY_BACK) return;
        if (k == H9_KEY_MENU) {
            s->step_time = (uint16_t)(v < STEP_TIME_MIN ? STEP_TIME_MIN : v > STEP_TIME_MAX ? STEP_TIME_MAX : v);
            return;
        }
        if (k == H9_KEY_UP) { v = v + 5 > STEP_TIME_MAX ? STEP_TIME_MAX : v + 5; dirty = 1; }
        else if (k == H9_KEY_DOWN) { v = v >= STEP_TIME_MIN + 5 ? v - 5 : STEP_TIME_MIN; dirty = 1; }
        else if (k >= 2 && k <= 11) {
            v = typed ? v * 10 + (k - 2) : k - 2;
            if (v > STEP_TIME_MAX) v = k - 2;
            typed = dirty = 1;
        }
        h9_delay(10);
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    if (s->step_time < STEP_TIME_MIN) s->step_time = STEP_TIME_MIN;   /* saved under the old 5 s minimum */
    unsigned sel = 0;
    for (;;) {
        int r = menu_run("SETTINGS", 6, sel, settings_label);
        if (r < 0) { trk_run("TRACKER"); return 0; }
        sel = (unsigned)r;
        if (r == 0) {
            int c = menu_run("STORE PTS", 4, s->cap / 40u - 1u, cap_label);
            if (c >= 0) trk_set_cap(((unsigned)c + 1) * 40);
        } else if (r == 1) {
            int c = menu_run("STEP DIST", 16, s->step, step_label);
            if (c >= 0) s->step = (uint8_t)c;
        } else if (r == 2) {
            s->hdg_filter = (uint8_t)!s->hdg_filter;
        } else if (r == 3) {
            edit_time(s);
        } else if (r == 4) {
            int c = menu_run("STEP MODE", 2, s->step_mode, mode_label);
            if (c >= 0) s->step_mode = (uint8_t)c;
        } else {
            trk_clear();
            trk_msg("<CLEARED>", 0, 0, 80);
        }
    }
}
