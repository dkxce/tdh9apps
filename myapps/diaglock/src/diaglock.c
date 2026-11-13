/* diaglock.c - on-screen diagnostic for the owner-lock chain.
 *
 * Prints, in hex on the 160x128 display:
 *   PASS: 8 bytes read from resource flash 0x716FF8
 *   SKIP: skip_check byte at 0x2B8DF
 *   KEY : live key byte from the stock scan (0x01E515AC -> 0xC3AF)
 *
 * It is a normal module (index 1 of a bundle whose index 0 is MENU), so we
 * just show the diagnostics directly; no return to menu needed for a probe.
 */
#include "h9mod.h"
#include "gfx.h"

#define PASS_ADDR 0x716FF8u
#define PASS_LEN  8u
#define SKIP_CHECK_ADDR 0x2B8DFu

/* h9lib.s */
extern unsigned h9_key(void);

static void app_hex(unsigned x, unsigned y, unsigned v)
{
    char tmp[9];
    char digits[] = "0123456789abcdef";
    tmp[8] = 0;
    for (int i = 7; i >= 0; i--) {
        tmp[i] = digits[v & 0xf];
        v >>= 4;
    }
    gfx_text(x, y, tmp, 8, 1, GFX_YELLOW, GFX_BLACK);
}

/* Format the 8 lock bytes as hex into out (16 chars + NUL). */
static void pass_hex(const unsigned char *p, char *out)
{
    char digits[] = "0123456789abcdef";
    int k = 0;
    for (unsigned i = 0; i < PASS_LEN; i++) {
        out[k++] = digits[p[i] >> 4];
        out[k++] = digits[p[i] & 0xf];
    }
    out[k] = 0;
}

unsigned module_main(const struct h9_api *api, unsigned self)
{
    (void)self;
    unsigned char pass[PASS_LEN];
    api->read(pass, PASS_ADDR, PASS_LEN);

    unsigned skip = *(volatile unsigned char *)SKIP_CHECK_ADDR;

    gfx_fill(0, 0, H9_WIDTH, H9_HEIGHT, GFX_BLUE);
    gfx_text(6, 2, "DIAG LOCK", 16, 2, GFX_WHITE, GFX_BLUE);
    gfx_text(6, 30, "PASS:", 10, 1, GFX_GREY, GFX_BLACK);
    char hex[17];
    pass_hex(pass, hex);
    gfx_text(6, 42, hex, 16, 1, GFX_WHITE, GFX_BLACK);
    gfx_text(6, 60, "SKIP:", 10, 1, GFX_GREY, GFX_BLACK);
    app_hex(6, 72, skip);
    gfx_text(6, 92, "KEY:", 10, 1, GFX_GREY, GFX_BLACK);
    app_hex(6, 104, h9_key());

    gfx_wait_release();
    for (;;) {
        unsigned key = h9_key();
        /* refresh KEY line */
        gfx_fill(6, 104, H9_WIDTH, 12, GFX_BLACK);
        app_hex(6, 104, key);
        gfx_fill(6, 122, H9_WIDTH, 6, GFX_BLACK);
        if (key == H9_KEY_BACK) break;
    }
    return 0;
}