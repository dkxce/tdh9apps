/* bootscreen.c: show the radio's boot logo on demand.
 *
 * Inside a menu app, reads the 160x128 big-endian RGB565 bitmap from the
 * resource flash (0xA00000, the same location td9_logo.py writes) through the
 * loader's api->read and streams it to the LCD in 128-byte chunks (the LCD's
 * own transfer size). Keys: any key exits back to the menu.
 *
 * Freestanding: no globals, no libc. State on the stack only. */
#include "h9mod.h"
#include "gfx.h"

enum {
    LOGO_ADDRESS = 0xA00000,
    WIDTH = 160,
    HEIGHT = 128,
    LOGO_BYTES = WIDTH * HEIGHT * 2,   /* 40,960 */
    CHUNK = 128,
};

static void show_logo(const struct h9_api *api)
{
    uint8_t buffer[CHUNK];
    h9_lcd_window(0, 0, WIDTH - 1, HEIGHT - 1);
    for (uint32_t offset = 0; offset < LOGO_BYTES; offset += CHUNK) {
        api->read(buffer, LOGO_ADDRESS + offset, CHUNK);
        h9_lcd_write(buffer, CHUNK);
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)self;
    show_logo(api);
    gfx_text(4, 116, "ANY KEY: EXIT", 26, 1, GFX_GREY, GFX_BLACK);
    gfx_wait_release();
    for (;;) {
        unsigned key = h9_key();
        if (key != H9_KEY_NONE) break;
        h9_delay(5);
    }
    gfx_wait_release();
    return 0;   /* back to the menu */
}