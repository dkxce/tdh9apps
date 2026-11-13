/* TRACKER: the app's visible module. Menus TRACKER and NAV TO (target,
 * track recording), the CURRENT screen, HELP and ABOUT; Settings, Fixed, Radar,
 * Map and Points are the hidden .TRK modules. */
#include "trk.h"

static void main_label(unsigned i, char *o)
{
    static const char *const T[7] = { "NAVTO", "RADAR", "MAP", "POINTS", "SETTINGS", "HELP", "ABOUT" };
    put(o, T[i]);
}

static void navto_label(unsigned i, char *o)
{
    static const char *const T[5] = { "FIXED", "CURRENT", "START", "PAUSE", "STOP" };
    put(o, i == 3 && TRK->rec == REC_PAUSED ? "RESUME" : T[i]);
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

/* Lines starting with a space are white, the rest yellow headings. */
static void text_page(const char *title, const char *const *t, unsigned n)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    menu_title(title);
    for (unsigned i = 0; i < n; i++)
        gfx_text(4, 12 + i * 9, t[i], 26, 1, t[i][0] == ' ' ? GFX_WHITE : GFX_YELLOW, GFX_BLACK);
    while (!trk_key()) { trk_service(); h9_delay(10); }
}

static void help(void)
{
    static const char *const H[] = {
        "RADAR", " UP/DN   MAP", " 5       POINTS", " 0       FULL / SPLIT", " *       N-E-S-W/12/36",
        "MAP", " UP/DN   RADAR", " 5       POINTS", " 0       FULL / SPLIT", " * / #   SCALE -/+",
        "BOTH", " MENU    REC/PAUSE/RESUME", " BACK    TRACKER MENU" };
    text_page("HELP", H, sizeof H / sizeof H[0]);
}

static void about(void)
{
    static const char *const A[] = {
        " VERSION 1.0.0", " ",
        "AUTHOR: DKXCE", " HTTPS://GITHUB.COM/DKXCE", " ",
        "PROJECT:", " HTTPS://GITHUB.COM/DKXCE/", " TDH9APPS", " ",
        " (C) DKXCE 2026" };
    text_page("ABOUT", A, sizeof A / sizeof A[0]);
}

/* 1 when another module was requested */
static int navto(struct trk *s)
{
    unsigned sel = 0;
    for (;;) {
        int r = menu_run("NAV TO", 5, sel, navto_label);
        if (r < 0) return 0;
        sel = (unsigned)r;
        if (r == 0) { s->screen = SCR_NAVTO; trk_run(".TRK FIXED"); return 1; }
        if (r == 1) current(s);
        else if (r == 2) {
            trk_start();
            trk_msg("<STARTED>", 0, 0, 80);
            trk_run(".TRK RADAR");
            return 1;
        } else if (r == 3) {
            if (s->rec == REC_ON) { s->rec = REC_PAUSED; trk_msg("<PAUSED>", 0, 0, 80); }
            else if (s->rec == REC_PAUSED) {
                s->rec = REC_ON;
                trk_msg("<RESUMED>", 0, 0, 80);
                trk_run(".TRK RADAR");
                return 1;
            } else trk_msg("<STOPPED>", 0, 0, 80);
        } else { s->rec = REC_OFF; trk_msg("<STOPPED>", 0, 0, 80); }
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    s->from = FROM_MENU;
    if (s->screen == SCR_NAVTO) {
        s->screen = SCR_MAIN;
        if (navto(s)) return 0;
    }
    for (;;) {
        int r = menu_run("TRACKER", 7, s->main_sel, main_label);
        if (r < 0) return 0;
        s->main_sel = (uint8_t)r;
        if (r == 0) { if (navto(s)) return 0; }
        else if (r == 5) help();
        else if (r == 6) about();
        else {
            static const char *const NEXT[4] = { ".TRK RADAR", ".TRK MAP", ".TRK POINTS", ".TRK SETUP" };
            trk_run(NEXT[r - 1]);
            return 0;
        }
    }
}
