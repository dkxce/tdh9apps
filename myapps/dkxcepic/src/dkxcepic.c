/* dkxcepic.c: DKXCEPIC picture viewer.
 *
 * Reads a picture pack from the resource flash and displays one image at a
 * time. Pack format (written by td9_kpi.py):
 *   +0  "DKXCEPI"      7-byte magic
 *   +7  count          1 byte (number of images to follow)
 *   +8  images         count × 160×128 RGB565 big-endian (40960 bytes each)
 *
 * The pack lives in the free font-tail area at PACK_ADDR (default 0x717000,
 * the same default td9_kpi.py uses).
 *
 * Keys: Up = previous image, Down = next image (wrap around), Back = exit.
 * Freestanding: no globals, no libc. */
#include "h9mod.h"
#include "gfx.h"

#define PACK_ADDR 0x717000u
#define IMG_W 160u
#define IMG_H 128u
#define IMG_BYTES (IMG_W * IMG_H * 2u)   /* 40960 */
#define CHUNK 128u

static uint32_t max_images(void)
{
    /* The pack header + count + count*IMG_BYTES must fit between PACK_ADDR
     * and the end of the font tail (0x800000). */
    return (0x800000u - PACK_ADDR - 8u) / IMG_BYTES;
}

/* Show image `index` (0-based) by streaming its RGB565 bytes to the LCD. */
static void show_image(const struct h9_api *api, unsigned index)
{
    uint8_t buffer[CHUNK];
    uint32_t off = 8u + (uint32_t)index * IMG_BYTES;   /* skip magic+count */
    h9_lcd_window(0, 0, IMG_W - 1, IMG_H - 1);
    for (uint32_t i = 0; i < IMG_BYTES; i += CHUNK) {
        api->read(buffer, PACK_ADDR + off + i, CHUNK);
        h9_lcd_write(buffer, CHUNK);
    }
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)self;
    char hdr[8];
    api->read(hdr, PACK_ADDR, 8);
    unsigned count = 0;
    if (hdr[0] == 'D' && hdr[1] == 'K' && hdr[2] == 'X' &&
        hdr[3] == 'C' && hdr[4] == 'E' && hdr[5] == 'P' && hdr[6] == 'I') {
        count = (unsigned)hdr[7];
    }
    if (count == 0 || count > max_images()) {
        gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLACK);
        gfx_text(2, 50, "NO PICTURES", 12, 2, GFX_RED, GFX_BLACK);
        gfx_text(2, 90, "RUN td9_kpi.py write ...", 26, 1, GFX_GREY, GFX_BLACK);
        gfx_wait_release();
        for (;;) {
            unsigned k = h9_key();
            if (k == H9_KEY_BACK) return 0;
            h9_delay(5);
        }
    }

    unsigned cur = 0;
    show_image(api, cur);
    gfx_wait_release();
    unsigned last = H9_KEY_NONE;
    for (;;) {
        unsigned k = h9_key();
        if (k != last && k != H9_KEY_NONE) {
            last = k;
            if (k == H9_KEY_BACK) break;
            if (k == H9_KEY_UP) {
                cur = cur == 0 ? count - 1 : cur - 1;
                show_image(api, cur);
            } else if (k == H9_KEY_DOWN) {
                cur = (cur + 1) % count;
                show_image(api, cur);
            }
        } else if (k == H9_KEY_NONE) {
            last = H9_KEY_NONE;
        }
        h9_delay(5);
    }
    gfx_wait_release();
    return 0;   /* back to the menu */
}