/* .TRK FIXED: the target typed in. Menu FIXED (Lat, Lon), one editor each:
 * 0-9 digit (on N/S, E/W any digit flips it), Up/Down move, Menu OK,
 * Back cancel. Back from FIXED returns to NAV TO. */
#include "trk.h"

static void fixed_label(unsigned i, char *o)
{
    struct trk *s = TRK;
    o = put(o, i ? "LON " : "LAT ");
    if (s->have_target) deg_text(o, i ? s->tlon : s->tlat);
    else put(o, "---");
}

/* *v in microdegrees, ideg integer digits (2 lat, 3 lon); 1 when accepted */
static int edit(const char *title, int32_t *v, unsigned ideg, char pos, char neg)
{
    uint8_t dg[8];
    unsigned n = ideg + 5, cur = 1, dirty = 1;
    int minus = *v < 0;
    uint32_t a = (uint32_t)(minus ? -*v : *v) / 10;
    for (unsigned i = n; i--;) { dg[i] = (uint8_t)(a % 10); a /= 10; }

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    menu_title(title);
    gfx_text(4, 96, "0-9 DIGIT  UP/DN MOVE", 21, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(4, 110, "MENU OK  BACK CANCEL", 20, 1, GFX_WHITE, GFX_BLACK);
    for (;;) {
        trk_service();
        if (dirty) {
            char b[12];
            unsigned j = 0, col;
            b[j++] = minus ? neg : pos;
            b[j++] = ' ';
            for (unsigned i = 0; i < n; i++) {
                if (i == ideg) b[j++] = '.';
                b[j++] = (char)('0' + dg[i]);
            }
            b[j] = 0;
            gfx_text(8, 44, b, 11, 2, GFX_WHITE, GFX_BLACK);
            gfx_fill(8, 61, 11 * 12, 2, GFX_BLACK);
            col = cur == 0 ? 0 : cur <= ideg ? cur + 1 : cur + 2;
            gfx_fill(8 + col * 12, 61, 12, 2, GFX_WHITE);
            dirty = 0;
        }
        unsigned k = trk_key();
        if (k == H9_KEY_BACK) return 0;
        if (k == H9_KEY_MENU) {
            int32_t r = 0;
            for (unsigned i = 0; i < n; i++) r = r * 10 + dg[i];
            if (r > (int32_t)(ideg == 2 ? 9000000 : 18000000)) r = ideg == 2 ? 9000000 : 18000000;
            *v = (minus ? -r : r) * 10;
            return 1;
        }
        if (k == H9_KEY_UP) { cur = cur ? cur - 1 : n; dirty = 1; }
        else if (k == H9_KEY_DOWN) { cur = cur < n ? cur + 1 : 0; dirty = 1; }
        else if (k >= 2 && k <= 11) {
            if (cur == 0) minus = !minus;
            else dg[cur - 1] = (uint8_t)(k - 2);
            if (cur < n) cur++;
            dirty = 1;
        }
        h9_delay(10);
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)api; (void)self;
    struct trk *s = trk_open();
    if (!s) return 0;
    unsigned sel = 0;
    for (;;) {
        int r = menu_run("FIXED", 2, sel, fixed_label);
        if (r < 0) { trk_run("TRACKER"); return 0; }
        sel = (unsigned)r;
        int32_t lat = s->have_target ? s->tlat : s->lat;
        int32_t lon = s->have_target ? s->tlon : s->lon;
        if (r == 0 ? edit("LAT", &lat, 2, 'N', 'S') : edit("LON", &lon, 3, 'E', 'W')) {
            s->tlat = lat;
            s->tlon = lon;
            s->have_target = 1;
        }
    }
}
