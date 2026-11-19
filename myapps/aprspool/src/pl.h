/* APRS 8-ball. State at 0x2B8E0; body must end before satshare 0x2B880. */
#ifndef PL_H
#define PL_H

#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

enum { KEY_0 = 2, KEY_1 = 3, KEY_2 = 4, KEY_3 = 5, KEY_4 = 6, KEY_5 = 7,
       KEY_6 = 8, KEY_8 = 10, KEY_9 = 11, KEY_MENU = 12, KEY_BACK = 15,
       KEY_STAR = 0x13 };
enum { FP_SHIFT = 8, TBL_X = 8, TBL_Y = 18, TBL_W = 144, TBL_H = 104,
       BALL_R = 3, POCK_R = 6, MAX_BALLS = 10 };
enum { COL_CLOTH = 0x03E0, COL_BORDER = 0x8200, COL_AIM = 0xFFE0 };
enum { ST_AIM = 0, ST_ROLL = 1, ST_WIN = 2, ST_LOSE = 3 };
enum { BODY_TOP = 0x2B880 };

#define TO_FP(v) ((int32_t)(v) << FP_SHIFT)
#define TO_PX(v) ((int)((v) >> FP_SHIFT))

struct pl_ball {
    int32_t x, y, vx, vy, ox, oy;
    uint8_t active, _pad[3];
};

struct pl {
    struct pl_ball balls[MAX_BALLS];
    char call[7], room[2];
    int16_t aim;
    uint8_t power, state, klast, kseen;
    uint8_t aprs, turn, we_shot, started;
    uint32_t shots, last_tick;
};

#define PL ((struct pl *)0x2B8E0)

extern void *memset(void *dst, int value, unsigned n);
extern const uint32_t pl_end;
extern const uint16_t PL_COLORS[MAX_BALLS];

struct pl *pl_open(void);
unsigned pl_key(void);
void pl_reset(void);
void pl_hud(void);
void pl_render(void);
void pl_strike(int angle, int power);
int pl_step(void);
void pl_after_rest(void);
int pl_ended(void);
void pl_show_end(void);
void pl_pack_sync(uint8_t *dst);
void pl_apply_sync(const uint8_t *src);
void pl_play(void (*do_shot)(void), void (*on_tick)(void), void (*on_rest)(void));

void pl_load_call(void);
int pl_select_room(void);
void pl_radio_setup(void);
void pl_radio_shot(void);
void pl_radio_tick(void);
void pl_radio_rest(void);

#endif
