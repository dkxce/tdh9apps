/* H9 Toolbox module menu (module 0 of the bundle).
 *
 * Lists the bundle's other modules, then USB UPDATE (the BR23 USB loader, as
 * holding 8 at power-on). Up/Down move, Menu runs the selection, Back goes
 * back to the radio without a restart. Returns the module index to run, or 0
 * for the radio.
 *
 * Going back needs the firmware whose loader stub keeps the radio's task frame
 * (src/stub.s ml_main returns); on the older firmware returning 0 restarts the
 * radio. Before returning it clears the screen and calls stock's RF refresh and
 * display redraw (the calls SAT PASS makes after a VFO step), because stock
 * does not redraw by itself.
 *
 * Holding # is what opened the menu, so input is ignored until every key
 * has been released. With exactly one visible module (USB UPDATE aside) the
 * menu opened by # runs it at once; when it returns the list is shown.
 *
 * Owner-lock integration (dkxce lock firmware):
 *   skip_check lives at 0x2B8DF: 0/fresh, 1=unlocked, 2=locked.
 *   When opened with skip_check == 2 the menu does not draw the module list:
 *     - PASSLOCK present -> return its index so the loader runs it (it verifies
 *       the password and clears skip_check to 1 before returning to the radio);
 *     - PASSLOCK absent  -> fail open: set skip_check = 1 and return to radio.
 *   Otherwise the normal list shows (PASSLOCK included as a regular entry).
 */
#include "h9mod.h"
#include "gfx.h"
#include "vfo_persist.h"   /* put the user's VFOs back if SAT TRACK's tracker had changed them */

enum { TITLE_H = 22, ROW_Y = 26, ROW_H = 18, ROWS = 5, FOOT_Y = 117, MAX_ITEMS = 64 };
enum { ITEM_USB = -2 };
enum { KEY_HASH = 0x14 };   /* the stub opens the menu on holding # */
#ifndef STOCK_REFRESH
#define STOCK_REFRESH() ((void (*)(unsigned))0x01E58E3E)(0)   /* full RF refresh: reloads both VFOs (satauto.c) */
#define STOCK_REDRAW() ((void (*)(unsigned))0x01E53306)(0)    /* marks the three display rows for redraw */
#endif

/* Owner-lock flag, shared with the main-flash stub (stub.s mlk_check). */
#define SKIP_CHECK ((volatile unsigned char *)0x2B8DFu)
#define SKIP_UNLOCKED 1u
#define SKIP_LOCKED   2u
#define SKIP_NORMAL   3u

#define PASSCODE_NAME "PASSLOCK"

static void read_entry(const struct h9_api *api, unsigned index, struct h9_entry *entry)
{
    api->read(entry, api->bundle + H9_BUNDLE_HEADER + H9_BUNDLE_ENTRY * index, sizeof *entry);
}

/* Return the bundle index of the PASSCODE module, or 0 if there is none. */
static int find_passcode(const struct h9_api *api, unsigned count)
{
    for (unsigned i = 1; i < count; i++) {
        struct h9_entry entry;
        unsigned k = 0;
        read_entry(api, i, &entry);
        while (k < 8 && PASSCODE_NAME[k] && entry.name[k] == PASSCODE_NAME[k]) k++;
        if (k == 8 || (k > 0 && entry.name[k] == 0 && PASSCODE_NAME[k] == 0))
            return (int)i;
    }
    return 0;
}

/* Item i of the list: the visible modules (vis[], bundle indices), then
 * USB UPDATE. */
static int item_module(unsigned item, unsigned modules, const uint8_t *vis)
{
    if (item < modules) return vis[item];
    return ITEM_USB;
}

static void draw_row(const struct h9_api *api, unsigned item, unsigned modules, const uint8_t *vis, unsigned row, int selected)
{
    uint16_t fg = selected ? GFX_BLACK : GFX_WHITE, bg = selected ? GFX_YELLOW : GFX_BLACK;
    unsigned y = ROW_Y + row * ROW_H;
    struct h9_entry entry;
    const char *name;
    char label[H9_NAME_BYTES + 1];
    int which = item_module(item, modules, vis);
    if (which == ITEM_USB) {
        name = "USB UPDATE";
    } else {
        read_entry(api, (unsigned)which, &entry);
        for (unsigned i = 0; i < H9_NAME_BYTES; i++) label[i] = entry.name[i];
        label[H9_NAME_BYTES] = 0;
        name = label;
    }
    /* 2x text (12 x 16 px cells), up to 12 characters after a 4 px margin,
     * inside an 18 px row: one line above and below the text. */
    unsigned x = gfx_text(4, y + 1, name, 12, 2, fg, bg);
    gfx_fill(0, y, H9_WIDTH, 1, bg);
    gfx_fill(0, y + 17, H9_WIDTH, 1, bg);
    gfx_fill(0, y + 1, 4, 16, bg);
    if (x < H9_WIDTH) gfx_fill(x, y + 1, H9_WIDTH - x, 16, bg);
}

static void draw_list(const struct h9_api *api, unsigned items, unsigned modules, const uint8_t *vis, unsigned top, unsigned selected)
{
    for (unsigned row = 0; row < ROWS; row++) {
        unsigned item = top + row;
        if (item < items) draw_row(api, item, modules, vis, row, item == selected);
        else gfx_fill(0, ROW_Y + row * ROW_H, H9_WIDTH, ROW_H, GFX_BLACK);
    }
}

static void message(const char *text)
{
    gfx_fill(0, ROW_Y, H9_WIDTH, ROWS * ROW_H, GFX_BLACK);
    gfx_text(4, ROW_Y + 8, text, 12, 2, GFX_YELLOW, GFX_BLACK);
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    uint8_t header[H9_BUNDLE_HEADER];
    (void)self;
    api->read(header, api->bundle, sizeof header);
    unsigned count = header[10] | (unsigned)header[11] << 8;
    unsigned modules = count > 1 ? count - 1 : 0;

    if (*SKIP_CHECK == SKIP_LOCKED) {
        int pc = find_passcode(api, count);
        if (!pc) {
            *SKIP_CHECK = SKIP_UNLOCKED;
            return 0;
        }
        return (unsigned)pc;
	}
	
	if (*SKIP_CHECK == SKIP_NORMAL) {
		*SKIP_CHECK = SKIP_UNLOCKED;
		 gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);   /* stock redraws only what it thinks changed */
         STOCK_REFRESH();
         STOCK_REDRAW();
		 h9_delay(120);
         return 0;
    }

    /* A module asked for another one: run it without showing the menu. */
    volatile struct h9_request *rq = H9_REQUEST;
    if (rq->magic != H9_REQUEST_MAGIC) vfo_restore_pending(api->read);   /* opened by hold #: the tracker is stopped */
    if (rq->magic == H9_REQUEST_MAGIC) {
        rq->magic = 0;
        for (unsigned i = 1; i < count; i++) {
            struct h9_entry entry;
            unsigned k = 0;
            read_entry(api, i, &entry);
            while (k < H9_NAME_BYTES && entry.name[k] == rq->name[k] && entry.name[k]) k++;
            if (k == H9_NAME_BYTES || entry.name[k] == rq->name[k]) return i;
        }
    }
    /* Visible modules: names starting with '.' are helper screens that other
     * modules open by request; they are not listed. */
    uint8_t vis[MAX_ITEMS];
    unsigned shown = 0;
    for (unsigned i = 1; i < count && shown < MAX_ITEMS; i++) {
        char first;
        api->read(&first, api->bundle + H9_BUNDLE_HEADER + H9_BUNDLE_ENTRY * i, 1);
        if (first != '.') vis[shown++] = (uint8_t)i;
    }
    modules = shown;

    /* Opened by holding # with a single app: run it at once. # if not passcode
     * held when that app returns, so leaving it shows this list. */
    if (modules == 1 && h9_key() == KEY_HASH) 
	{
		int pc = find_passcode(api, count);
		if (!pc) return vis[0];
	};

    unsigned items = modules + 1, selected = 0, top = 0;

    gfx_fill(0, 0, H9_WIDTH, TITLE_H, GFX_BLUE);
    gfx_text(8, 3, "RADIO WIDGET", 12, 2, GFX_WHITE, GFX_BLUE);
    gfx_fill(0, TITLE_H, H9_WIDTH, ROW_Y - TITLE_H, GFX_BLACK);
    gfx_fill(0, ROW_Y + ROWS * ROW_H, H9_WIDTH, FOOT_Y - (ROW_Y + ROWS * ROW_H), GFX_BLACK);
    gfx_text(4, FOOT_Y, "MENU: RUN   BACK: RADIO", 26, 1, GFX_GREY, GFX_BLACK);
    gfx_fill(0, FOOT_Y + 8, H9_WIDTH, H9_HEIGHT - FOOT_Y - 8, GFX_BLACK);
    draw_list(api, items, modules, vis, top, selected);

    gfx_wait_release();
    for (;;) {
        unsigned key = gfx_wait_key();
        unsigned previous = selected;
        if (key == H9_KEY_UP) {
            selected = selected ? selected - 1 : items - 1;
        } else if (key == H9_KEY_DOWN) {
            selected = selected + 1 < items ? selected + 1 : 0;
        } else if (key == H9_KEY_BACK) {
            gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);   /* stock redraws only what it thinks changed */
            STOCK_REFRESH();
            STOCK_REDRAW();
            return 0;
        } else if (key == H9_KEY_MENU) {
            int which = item_module(selected, modules, vis);
            if (which == ITEM_USB) {
                message("USB UPDATE");
                h9_usb_loader();
            }
            message("LOADING");
            return (unsigned)which;
        } else {
            continue;
        }
        unsigned new_top = selected < top ? selected : selected >= top + ROWS ? selected - ROWS + 1 : top;
        if (new_top != top) {
            top = new_top;
            draw_list(api, items, modules, vis, top, selected);
        } else {
            draw_row(api, previous, modules, vis, previous - top, 0);
            draw_row(api, selected, modules, vis, selected - top, 1);
        }
    }
}