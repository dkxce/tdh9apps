/* Shared drawing for C modules (gfx.c). */
#ifndef H9_GFX_H
#define H9_GFX_H

#include <stdint.h>

enum { GFX_BLACK = 0x0000, GFX_WHITE = 0xffff, GFX_YELLOW = 0xffe0, GFX_BLUE = 0x001f, GFX_GREY = 0x8410,
       GFX_GREEN = 0x07e0, GFX_RED = 0xf800 };

void gfx_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color);
/* Draw up to `max` characters of `text` (6 x 8 px cells times `scale`); returns the x after. */
unsigned gfx_text(unsigned x, unsigned y, const char *text, unsigned max, unsigned scale, uint16_t fg, uint16_t bg);
void gfx_wait_release(void);
unsigned gfx_wait_key(void);

#endif
