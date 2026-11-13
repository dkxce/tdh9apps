/* TRACKER radar / map drawing: primitives clipped to the view (x <
 * TRK->view_w: VIEW_W split, H9_WIDTH full screen), the value panel on the
 * right and the recording status icon. */
#include "trk.h"

void view_px(int x, int y, uint16_t c)
{
    if (x < 0 || x >= TRK->view_w || y < 0 || y >= H9_HEIGHT) return;
    gfx_fill((unsigned)x, (unsigned)y, 1, 1, c);
}

void view_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 < x0 ? x0 - x1 : x1 - x0;
    int dy = y1 < y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        view_px(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err * 2;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}

void view_circle(int cx, int cy, int r, uint16_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        view_px(cx + x, cy + y, c); view_px(cx - x, cy + y, c);
        view_px(cx + x, cy - y, c); view_px(cx - x, cy - y, c);
        view_px(cx + y, cy + x, c); view_px(cx - y, cy + x, c);
        view_px(cx + y, cy - x, c); view_px(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* Point at compass angle a (0 = up, clockwise), radius r from (cx, cy). */
void view_spoke(int cx, int cy, unsigned a, int r, int *x, int *y)
{
    *x = cx + sin_deg(a) * r / 1024;
    *y = cy - cos_deg(a) * r / 1024;
}

/* Shaft from the centre to radius r with a head at the tip. */
void view_arrow(int cx, int cy, unsigned a, int r, uint16_t c)
{
    int tx, ty, x, y;
    view_spoke(cx, cy, a, r, &tx, &ty);
    view_line(cx, cy, tx, ty, c);
    view_spoke(cx, cy, a + 12, r * 3 / 4, &x, &y);
    view_line(tx, ty, x, y, c);
    view_spoke(cx, cy, a + 348, r * 3 / 4, &x, &y);
    view_line(tx, ty, x, y, c);
}

/* Panel row: grey label over the value, 18 px per row, padded to clear the old value. */
void view_value(unsigned row, const char *label, const char *value, uint16_t color)
{
    char b[10];
    unsigned i = 0;
    if (!value) value = "---";
    for (; i < 9 && value[i]; i++) b[i] = value[i];
    for (; i < 9; i++) b[i] = ' ';
    b[9] = 0;
    gfx_text(VIEW_W + 2, row * 18, label, 9, 1, GFX_GREY, GFX_BLACK);
    gfx_text(VIEW_W + 2, row * 18 + 8, b, 9, 1, color, GFX_BLACK);
}

void view_status(unsigned x, unsigned y)
{
    static const uint8_t HALF[9] = { 1, 2, 3, 4, 4, 4, 3, 2, 1 };   /* dot of radius 4 */
    unsigned rec = TRK->rec;
    gfx_fill(x, y, 9, 9, GFX_BLACK);
    if (rec == REC_PAUSED) {
        gfx_fill(x + 1, y, 3, 9, GFX_YELLOW);
        gfx_fill(x + 5, y, 3, 9, GFX_YELLOW);
        return;
    }
    for (unsigned i = 0; i < 9; i++)
        gfx_fill(x + 4 - HALF[i], y + i, 2u * HALF[i] + 1, 1, rec == REC_ON ? GFX_GREEN : GFX_RED);
}

void view_rec_key(void)
{
    struct trk *s = TRK;
    if (s->rec == REC_OFF) trk_start();
    else s->rec = s->rec == REC_ON ? REC_PAUSED : REC_ON;
}

void view_frame(int full)
{
    struct trk *s = TRK;
    s->view_w = (uint8_t)(full ? H9_WIDTH : VIEW_W);
    gfx_fill(0, 0, s->view_w, H9_HEIGHT, GFX_BLACK);
    if (!full) {
        gfx_fill(VIEW_W, 0, 1, H9_HEIGHT, C_DGREY);
        gfx_fill(VIEW_W + 1, 0, H9_WIDTH - VIEW_W - 1, H9_HEIGHT, GFX_BLACK);
    }
}
