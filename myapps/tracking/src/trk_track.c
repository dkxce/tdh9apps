/* TRACKER track editing and point queries: linked only into the modules
 * that use them (TRACKER, .TRK SETUP, .TRK POINTS), keeping Radar and Map
 * small. */
#include "trk.h"

#define GPS_TZ (*(const volatile uint8_t *)0xC1C6)   /* radio Time zone option: UTC-12..+12 as 0..24 */

char *time_text(char *d, uint32_t sec)
{
    d = num(d, sec / 3600, 1);
    *d++ = ':';
    d = num(d, sec / 60 % 60, 2);
    *d++ = ':';
    return num(d, sec % 60, 2);
}

int32_t trk_point_clock(unsigned i)
{
    struct trk *s = TRK;
    unsigned tz = GPS_TZ;
    if (s->start_utc < 0 || i >= s->count) return -1;
    int32_t t = s->start_utc + (int32_t)s->p[i].t * 2 + (tz <= 24 ? ((int32_t)tz - 12) * 3600 : 0);
    return (t % 86400 + 86400) % 86400;
}

void trk_clear(void)
{
    struct trk *s = TRK;
    s->count = 0;
    s->base_dist = 0;
    s->show = -1;
    s->cur = 0;
}

void trk_set_cap(unsigned cap)
{
    struct trk *s = TRK;
    s->cap = (uint16_t)cap;
    while (s->count > cap) trk_drop_oldest(s);
    if (s->cur >= (int)s->count) s->cur = 0;
}

uint32_t trk_point_dist(unsigned i)
{
    struct trk *s = TRK;
    uint32_t d = s->base_dist;
    for (unsigned k = 1; k <= i && k < s->count; k++) d += trk_leg(k);
    return d;
}
