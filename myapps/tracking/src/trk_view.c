/* TRACKER radar / map drawing: primitives clipped to the view (x < VIEW_W)
 * and the value panel on the right. */
#include "trk.h"

void view_px(int x, int y, uint16_t c)
{
    if (x < 0 || x >= VIEW_W || y < 0 || y >= H9_HEIGHT) return;
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
    for (; i < 9 && value[i]; i++) b[i] = value[i];
    for (; i < 9; i++) b[i] = ' ';
    b[9] = 0;
    gfx_text(VIEW_W + 2, row * 18, label, 9, 1, GFX_GREY, GFX_BLACK);
    gfx_text(VIEW_W + 2, row * 18 + 8, b, 9, 1, color, GFX_BLACK);
}
