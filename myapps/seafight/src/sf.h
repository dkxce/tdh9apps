/* SEAFIGHT shared board. State at 0x2B8E0; body must end before satshare 0x2B880. */
#ifndef SF_H
#define SF_H

#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

enum { KEY_0 = 2, KEY_1 = 3, KEY_2 = 4, KEY_4 = 6, KEY_5 = 7, KEY_6 = 8,
       KEY_8 = 10, KEY_9 = 11, KEY_MENU = 12, KEY_BACK = 15,
       KEY_STAR = 0x13, KEY_HASH = 0x14 };
enum { N = 10, CELL = 7, MY_X = 6, EN_X = 84, GRID_Y = 24 };
enum { C_WATER, C_SHIP, C_MISS, C_HIT };
enum { COL_SEA = 0x0015, COL_SHIP = 0x07E0, COL_MISS = 0xC618, COL_HIT = 0xF800, COL_CUR = 0xFFE0 };
enum { BODY_TOP = 0x2B880, FIRE_TIMEOUT = 12000u };

#define AT(g, x, y) ((g)[(y) * N + (x)])

struct sf {
    uint8_t my[100], en[100], real[100], vis[100], rx[128];
    int8_t qx[32], qy[32];
    char call[7], room[2];
    int8_t cx, cy, fx, fy, mx, my, ox, oy;
    uint8_t myhp, enhp, rxn, esc, klast, kseen;
    uint8_t aprs, turn, started, pending, timeout;
    uint32_t rng, ftick;
};

#define SF ((struct sf *)0x2B8E0)

extern void *memset(void *dst, int value, unsigned n);
extern const uint32_t sf_end;
extern const int8_t SF_DX[4], SF_DY[4];

struct sf *sf_open(void);   /* 0 if body overlaps satshare (message shown) */
unsigned sf_key(void);
uint32_t sf_rnd(void);
void sf_status(const char *m, uint16_t c);
void sf_cell(int bx, int x, int y, uint8_t st, int cur);
void sf_redraw(void);
void sf_board(void);
void sf_place(uint8_t *g);
int sf_sunk(uint8_t *orig, uint8_t *dmg, int x, int y);
void sf_sink(uint8_t *g, int bx, int x, int y);
void sf_count_hp(void);
void sf_init(void);
void sf_play(void (*do_fire)(void), void (*on_tick)(void));

void sf_cpu_fire(void);

void sf_load_call(void);
int sf_select_room(void);
void sf_radio_setup(void);
void sf_radio_fire(void);
void sf_radio_tick(void);

#endif
