/* TRACKER menus: three colours (black background, white text, the current
 * item on dark grey), the recording status on the bottom line, and the
 * short <MESSAGE> screens. */
#include "trk.h"

enum { ROW_Y = 14, ROW_H = 10, ROWS = 10, STATUS_Y = 118 };

static void center(unsigned y, const char *s, unsigned scale)
{
    unsigned w = text_len(s) * 6 * scale;
    gfx_text(w < H9_WIDTH ? (H9_WIDTH - w) / 2 : 0, y, s, 26, scale, GFX_WHITE, GFX_BLACK);
}

void menu_title(const char *title)
{
    char b[28], *d = put(b, "-- ");
    d = put(d, title);
    put(d, " --");
    center(2, b, 1);
}

static void row(unsigned y, unsigned n, const char *label, int selected)
{
    char b[28], *d = b;
    uint16_t bg = selected ? C_DGREY : GFX_BLACK;
    *d++ = '[';
    d = num(d, n, 1);
    d = put(d, "] ");
    put(d, label);
    gfx_fill(0, y, H9_WIDTH, ROW_H, bg);
    gfx_text(4, y + 1, b, 26, 1, GFX_WHITE, bg);
}

static void status_line(void)
{
    static const char *const S[3] = { "STATUS: IDLE", "STATUS: RECORDING...", "STATUS: PAUSED" };
    gfx_fill(0, STATUS_Y - 3, H9_WIDTH, 1, C_DGREY);
    gfx_text(4, STATUS_Y, S[TRK->rec], 20, 1, GFX_WHITE, GFX_BLACK);
}

/* Up / Down move (wrapping), Menu picks, digit 1-9 picks that item
 * (0 = item 10), Back returns MENU_BACK. */
int menu_run(const char *title, unsigned count, unsigned sel, menu_label_fn label)
{
    char b[24];
    unsigned top = 0, dirty = 1;
    if (sel >= count) sel = 0;
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    menu_title(title);
    status_line();
    for (;;) {
        trk_service();
        if (dirty) {
            if (sel < top) top = sel;
            if (sel >= top + ROWS) top = sel - ROWS + 1;
            for (unsigned r = 0; r < ROWS && top + r < count; r++) {
                label(top + r, b);
                row(ROW_Y + r * ROW_H, top + r + 1, b, top + r == sel);
            }
            dirty = 0;
        }
        unsigned k = trk_key();
        if (k == H9_KEY_BACK) return MENU_BACK;
        if (k == H9_KEY_MENU) return (int)sel;
        if (k == H9_KEY_UP) {
            sel = sel ? sel - 1 : count - 1;
            dirty = 1;
        } else if (k == H9_KEY_DOWN) {
            sel = sel + 1 < count ? sel + 1 : 0;
            dirty = 1;
        } else if (k >= 2 && k <= 11) {
            unsigned i = k == 2 ? 9 : k - 3;
            if (i < count) return (int)i;
        }
        h9_delay(10);
    }
}

void trk_msg(const char *big, const char *line1, const char *line2, unsigned ticks)
{
    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
    center(40, big, 2);
    if (line1) center(72, line1, 1);
    if (line2) center(86, line2, 1);
    if (ticks) {
        uint32_t t0 = TICK;
        while (TICK - t0 < ticks) { trk_service(); h9_delay(10); }
        return;
    }
    while (!trk_key()) { trk_service(); h9_delay(10); }
}
