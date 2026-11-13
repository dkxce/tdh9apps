/* TRACKER: shared state and helpers of the tracker modules.
 *
 * One app, six modules (build.json): TRACKER (menus, help), .TRK SETUP
 * (settings), .TRK FIXED (target editor), .TRK RADAR, .TRK MAP, .TRK POINTS.
 * They hand over through the run-next mailbox (trk_run + return 0) and keep
 * everything in struct trk just below H9_SHARED_TOP, which survives module
 * switches and the radio while no bigger module is loaded. Every module body
 * must end below it: trk_open checks the trk_end marker (trk_end.c, linked
 * last).
 *
 * GPS: stock's parser is stopped while a module runs, so each module drains
 * the APRS modem's NMEA ring itself in trk_service; the track is recorded
 * only while one of the tracker modules is open. */
#ifndef TRK_H
#define TRK_H

#include <stdint.h>

#include "gfx.h"
#include "h9mod.h"

enum { C_DGREY = 0x4208, C_LGREY = 0xC618, C_CYAN = 0x07ff, C_FUCHSIA = 0xF81F };
enum { KEY_STAR = 0x13, KEY_HASH = 0x14, KEY_0 = 2, KEY_5 = 7 };
enum { NMEA = 84, MAX_PTS = 160, FIX_TIMEOUT = 500, STATE_BYTES = 0x6F4, TRK_MAGIC = 0x324B5254 /* "TRK2" */ };
enum { REC_OFF, REC_ON, REC_PAUSED };
enum { STEP_DIST, STEP_TIME };
enum { FROM_MENU, FROM_RADAR, FROM_MAP };
enum { SCR_MAIN, SCR_NAVTO };
enum { MENU_BACK = -1 };
enum { VIEW_W = 106 };   /* radar / map area in split mode; the value panel is to the right */

#define TICK (*(const volatile uint32_t *)0x1D8B0)   /* 100 Hz */

struct trk {
    uint32_t magic;
    uint8_t cmd, frame_len, escape, nmea_len;   /* frame_len: 0 before the command byte, then 1 */
    char nmea[NMEA];
    int32_t lat, lon, alt10;          /* last fix: microdegrees, metres x 10 */
    uint16_t course, spd10, sats;     /* degrees, knots x 10 */
    char utc[6];                      /* hhmmss, utc[0] = 0 until seen */
    uint8_t have_fix, fresh, key, key_seen;
    uint32_t fix_tick;
    int32_t tlat, tlon;               /* navigation target */
    uint8_t have_target, rec, has_start, screen;
    int32_t slat, slon;               /* start point, kept when old points drop out */
    uint32_t start_tick, base_dist;   /* base_dist: start -> p[0] along the track */
    int32_t start_utc;                /* UTC second of day at start, -1 unknown */
    uint16_t cap, count, step_time;   /* step_time: seconds */
    uint8_t step, step_mode, hdg_filter, zoom;
    uint8_t radar_mode, radar_full, map_full, from;
    uint8_t main_sel, view_w, spare[2];
    int16_t show, cur;                /* point to highlight on the map (-1 none), POINTS cursor */
    int32_t p_lat[MAX_PTS], p_lon[MAX_PTS];
    uint16_t p_t[MAX_PTS];            /* seconds from start / 2 */
};

#define TRK ((struct trk *)(H9_SHARED_TOP - STATE_BYTES))

extern const uint32_t trk_end;
extern const uint16_t STEP_M[16];

/* trk_core.c */
struct trk *trk_open(void);          /* 0: this module overlaps the state (message shown) */
void trk_service(void);              /* drain NMEA, record the track */
int trk_live(void);
int trk_heading_ok(void);            /* course worth showing: live, and moving if the 2 km/h filter is on */
unsigned trk_key(void);
void trk_run(const char *name);      /* open that module next; the caller returns 0 */
void trk_start(void);                /* new track, recording */
void trk_track_add(struct trk *s);
void trk_drop_oldest(struct trk *s);
int32_t trk_utc_now(const struct trk *s);   /* UTC second of day, -1 unknown */
uint32_t dist_between(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);

char *put(char *d, const char *s);
char *num(char *d, uint32_t v, unsigned width);
char *fixed(char *d, int32_t v, unsigned frac);
char *km_text(char *d, uint32_t m);       /* "1.234" */
char *dist_label(char *d, uint32_t m);    /* "250M", "2.5KM" */

int sin_deg(unsigned a);
int cos_deg(unsigned a);
void nav_delta(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2, int32_t *dx, int32_t *dy);
uint32_t nav_distance(int32_t dx, int32_t dy);
unsigned nav_bearing(int32_t dx, int32_t dy);

/* trk_track.c: TRACKER, .TRK SETUP, .TRK POINTS only */
void trk_clear(void);
void trk_set_cap(unsigned cap);
uint32_t trk_point_dist(unsigned i);  /* metres from the start along the track */
int32_t trk_point_clock(unsigned i);  /* local second of day, -1 unknown */
char *time_text(char *d, uint32_t sec);   /* "1:02:03" */

/* trk_deg.c: TRACKER, .TRK FIXED, .TRK RADAR only */
char *deg_text(char *d, int32_t micro);   /* "-55.75123" */

/* trk_menu.c */
typedef void (*menu_label_fn)(unsigned i, char *out);
void menu_title(const char *title);
int menu_run(const char *title, unsigned count, unsigned sel, menu_label_fn label);
void trk_msg(const char *big, const char *line1, const char *line2, unsigned ticks);   /* ticks 0: wait for a key */

/* trk_view.c: clipped to x < TRK->view_w */
void view_px(int x, int y, uint16_t c);
void view_line(int x0, int y0, int x1, int y1, uint16_t c);
void view_circle(int cx, int cy, int r, uint16_t c);
void view_spoke(int cx, int cy, unsigned a, int r, int *x, int *y);
void view_arrow(int cx, int cy, unsigned a, int r, uint16_t c);
void view_value(unsigned row, const char *label, const char *value, uint16_t color);   /* value 0: "---" */
void view_status(unsigned x, unsigned y);   /* 9 x 9: recording green dot, stopped red dot, paused yellow bars */
void view_frame(int full);                  /* clear the view; split: clear the panel and draw the divider */
void view_rec_key(void);                    /* Menu: idle -> start, recording -> pause, paused -> resume */

#endif
