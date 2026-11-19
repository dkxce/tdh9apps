/* APRSPOOL: APRS pool/billiards.  Visible module APRSPOOL (menu) hands off to
 * the hidden .PL RADIO module, which runs the APRS game. Shared state lives at
 * 0x2B8E0 (below the mailbox 0x2BEE0), like seafight's struct sf.
 *
 * Protocol (relative to the APRS KISS payload, see asp_radio.c):
 *   >PL<ROOM>:SHOT <angle> <power>
 *   >PL<ROOM>:SYNC <x0><y0>...<x9><y9>   (40 hex digits, 10 balls x 2 bytes)
 * Room is 2 ASCII digits 00..99.  SHOT fires a deterministic local physics step
 * on both radios; SYNC is the authoritative hex snapshot sent after balls stop.
 */
#ifndef ASP_H
#define ASP_H

#include <stdint.h>
#include "gfx.h"
#include "h9mod.h"

#define TICK (*(const volatile uint32_t *)0x1D8B0)

enum { KEY_0 = 2, KEY_1 = 3, KEY_2 = 4, KEY_3 = 5, KEY_4 = 6, KEY_5 = 7, KEY_6 = 8,
       KEY_7 = 9, KEY_8 = 10, KEY_9 = 11, KEY_MENU = 12, KEY_BACK = 15, KEY_STAR = 0x13, KEY_HASH = 0x14 };

/* Fixed-point Q8 physics (same as 8ballpool). */
#define FP_SHIFT 8
#define TO_FP(v) ((int32_t)(v) << FP_SHIFT)
#define TO_PX(v) ((int)((v) >> FP_SHIFT))

/* Wide table 110x62, centered. */
#define TBL_X 25
#define TBL_Y 33
#define TBL_W 110
#define TBL_H 62
#define BALL_R 3
#define POCK_R 6
#define MAX_BALLS 10 /* 0: биток, 1..8: цветные, 9: чёрный 8-ка */

enum { COL_CLOTH = 0x03E0, COL_BORDER = 0x8200, COL_AIM = 0xFFE0,
       COL_SEA = 0x0015, COL_SHIP = 0x07E0, COL_MISS = 0xC618, COL_CUR = 0xFFE0 };

/* Game/round state. */
enum { ST_AIM = 0, ST_MOVING = 1, ST_WIN = 2, ST_LOSE = 3 };

struct asp {
    int32_t bx[MAX_BALLS], by[MAX_BALLS];      /* positions Q8 */
    int32_t vx[MAX_BALLS], vy[MAX_BALLS];      /* velocities Q8 */
    int32_t ox[MAX_BALLS], oy[MAX_BALLS];      /* old positions Q8 (redraw) */
    uint8_t act[MAX_BALLS];                    /* active flag */
    uint8_t cx, cy;                            /* cursor (menu bits reused) */
    uint8_t aim, power;                        /* aim angle, power 2..11 */
    uint8_t state;                             /* ST_* */
    uint8_t myturn;                            /* 1: I shoot, 0: opponent */
    uint8_t started;                           /* first shot received/ours */
    uint8_t pending;                           /* awaiting SYNC ack */
    uint8_t timeout;                           /* SYNC timeout flagged */
    uint8_t shots;                             /* shots taken this game */
    uint8_t room[2];                           /* room digits */
    char call[7];                              /* my callsign */
    uint8_t mode;                              /* 'S' solo (BALLPOOL), 'R' APRS radio */
    /* radio ring (reused per module) */
    uint8_t klast, kseen;
    uint32_t ktick;
    uint32_t rng;
    uint32_t ftick;
    /* APRS receive */
    uint8_t rx[128];
    uint8_t rxn, esc;
};

#define ASP ((struct asp *)0x2B8E0)

extern void *memset(void *, int, unsigned);

struct asp *asp_open(void);
unsigned asp_key(void);
uint32_t asp_rnd(void);
void asp_status(const char *m, uint16_t c);

int32_t asp_sin(int a);
int32_t asp_cos(int a);

void asp_ball(int idx);
void asp_redraw_ball(int idx);
void asp_draw_table(void);
void asp_draw_aim(void);
void asp_draw_hud(void);
void asp_render(void);
void asp_reset_rack(void);
int  asp_step(void);            /* one physics step; returns still moving */
void asp_balls_stop(void);

void asp_load_call(void);
int  asp_select_room(void);
void asp_radio_setup(void);
void asp_radio_fire(void);

#endif