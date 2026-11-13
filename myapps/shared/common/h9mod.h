/* H9 Toolbox module interface (see ../../README.md).
 *
 * A module is linked for RAM 0x29F00 and run by the main-flash loader stub:
 *     unsigned module_main(const struct h9_api *api, unsigned self);
 * `self` is the module's own index in the bundle. When it returns, the stub
 * shows the menu again. The menu (index 0) returns the index to run, or 0
 * to restart the radio.
 *
 * Freestanding: no globals (no .data/.bss is set aside beyond what the
 * bundle entry declares), no library calls. */
#ifndef H9MOD_H
#define H9MOD_H

#include <stdint.h>

struct h9_api {
    uint32_t magic;      /* "H9AP" */
    uint32_t version;    /* 1 */
    uint32_t bundle;     /* chip address of the bundle header */
    uint32_t load;       /* 0x29F00 */
    uint32_t slot;       /* bytes available at `load` (8 KiB) */
    void (*read)(void *dst, uint32_t chip_address, uint32_t length);
};

/* Bundle layout on the resource chip (little-endian). */
enum { H9_BUNDLE_HEADER = 16, H9_BUNDLE_ENTRY = 32, H9_NAME_BYTES = 16 };

struct h9_entry {
    char name[H9_NAME_BYTES];
    uint32_t offset, size, bss;
    uint16_t entry, crc;
};

/* modules/common/h9lib.s */
void h9_lcd_window(unsigned x0, unsigned y0, unsigned x1, unsigned y1);
void h9_lcd_write(const void *buffer, unsigned length);
unsigned h9_key(void);
void h9_delay(unsigned units);
void h9_led(unsigned on);
void h9_usb_loader(void);
void h9_reset(void);  /* restart into the radio (stock cpu_reset) */

enum { H9_WIDTH = 160, H9_HEIGHT = 128 };

/* "Run this module next": a module writes the name here and returns; the
 * loader then runs the menu (module 0), which clears the request and returns
 * that module's index without drawing. It sits in the last 32 bytes of the
 * 8 KiB RAM slot, which module code never reaches. Module state kept across
 * module switches must end at or below H9_SHARED_TOP. */
struct h9_request {
    uint32_t magic;
    char name[H9_NAME_BYTES];
    uint32_t spare[3];  /* the stub's GPS recorder (src/stub.s rec_tick) */
};
#define H9_REQUEST ((volatile struct h9_request *)0x2BEE0)
#define H9_SHARED_TOP 0x2BEE0u
enum { H9_REQUEST_MAGIC = 0x4E555248 };  /* "HRUN" */
enum { H9_KEY_NONE = 0, H9_KEY_1 = 3, H9_KEY_2 = 4, H9_KEY_3 = 5, H9_KEY_4 = 6, H9_KEY_5 = 7 /* digit d = d + 2 */, H9_KEY_MENU = 12, H9_KEY_UP = 13, H9_KEY_DOWN = 14, H9_KEY_BACK = 15 };

#endif
