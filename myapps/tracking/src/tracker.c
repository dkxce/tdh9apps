/* TRACKER: the app's visible module. Menus TRACKER, NAV TO (target, track
 * recording), CURRENT and SETTINGS; Fixed, Radar, Map and Points are the
 * hidden .TRK modules. */
#include "trk.h"

static void main_label(unsigned i, char *o)
{
    static const char *const T[5] = { "NAVTO", "RADAR", "MAP", "POINTS", "SETTINGS" };
    put(o, T[i]);
}

static void navto_label(unsigned i, char *o)
{
    static const char *const T[5] = { "FIXED", "CURRENT", "START", "PAUSE", "STOP" };
    put(o, i == 3 && TRK->rec == REC_PAUSED ? "RESUME" : T[i]);
}

static void settings_label(unsigned i, char *o)
{
    struct trk *s = TRK;
    if (i == 0) { o = put(o, "STORE PTS "); num(o, s->cap, 1); }
    else if (i == 1) { o = put(o, "STEP DIST "); dist_label(o, STEP_M[s->step]); }
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

/* "N 55.75123" */
static void hemi_text(char *d, int32_t micro, char pos, char neg)
{
    *d++ = micro < 0 ? neg : pos;
    *d++ = ' ';
    deg_text(d, micro < 0 ? -micro : micro);
}

static void current(struct trk *s)
{
    char a[16], b[16];
    if (trk_live()) {
        s->tlat = s->lat;
        s->tlon = s->lon;
        s->have_target = 1;
        hemi_text(a, s->lat, 'N', 'S');
        hemi_text(b, s->lon, 'E', 'W');
        trk_msg("<COPIED>", a, b, 0);
        return;
    }
    char *d = put(a, "SATS: ");
    num(d, s->sats, 1);
    d = put(b, "TIME: ");
    if (s->utc[0]) {
        for (unsigned i = 0; i < 6; i++) {
            if (i == 2 || i == 4) *d++ = ':';
            *d++ = s->utc[i];
        }
        *d = 0;
    } else put(d, "--:--:--");
    trk_msg("<NO FIX>", a, b, 0);
}

/* 1 when another module was requested */
static int navto(struct trk *s)
{
    unsigned sel = 0;
    for (;;) {
        int r = menu_run("NAV TO", 5, sel, navto_label, 0);
        if (r < 0) return 0;
        sel = (unsigned)r;
        if (r == 0) { s->screen = SCR_NAVTO; trk_run(".TRK FIXED"); return 1; }
        if (r == 1) current(s);
        else if (r == 2) { trk_start(); trk_msg("<STARTED>", 0, 0, 80); }
        else if (r == 3) {
            if (s->rec == REC_ON) { s->rec = REC_PAUSED; trk_msg("<PAUSED>", 0, 0, 80); }
            else if (s->rec == REC_PAUSED) { s->rec = REC_ON; trk_msg("<RESUMED>", 0, 0, 80); }
            else trk_msg("<STOPPED>", 0, 0, 80);
        } else { s->rec = REC_OFF; trk_msg("<STOPPED>", 0, 0, 80); }
    }
}

static void settings(struct trk *s)
{
    unsigned sel = 0;
    for (;;) {
        int r = menu_run("SETTINGS", 3, sel, settings_label, 0);
        if (r < 0) return;
        sel = (unsigned)r;
        if (r == 0) {
            int c = menu_run("STORE PTS", 4, s->cap / 40u - 1u, cap_label, 0);
            if (c >= 0) trk_set_cap(((unsigned)c + 1) * 40);
        } else if (r == 1) {
            int c = menu_run("STEP DIST", 16, s->step, step_label, 0);
            if (c >= 0) s->step = (uint8_t)c;
        } else {
            trk_clear();
            trk_msg("<CLEARED>", 0, 0, 80);
        }
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    unsigned sel = 0;
    if (s->screen == SCR_NAVTO) {
        s->screen = SCR_MAIN;
        if (navto(s)) return 0;
    }
    for (;;) {
        int r = menu_run("TRACKER", 5, sel, main_label, 0);
        if (r < 0) return 0;
        sel = (unsigned)r;
        if (r == 0) { if (navto(s)) return 0; }
        else if (r == 4) settings(s);
        else {
            trk_run(r == 1 ? ".TRK RADAR" : r == 2 ? ".TRK MAP" : ".TRK POINTS");
            return 0;
        }
    }
}
