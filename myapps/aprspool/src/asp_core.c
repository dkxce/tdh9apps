/* ASPRPOOl light shared helpers (open/key/rnd/status/sin/cos). Linked into
 * both APRSPOOL and .PL RADIO. Heavy physics/rendering live in asp_phys.c
 * (radio only) so the menu module stays small. */
#include "asp.h"

int32_t asp_sin(int a)
{
    static const uint16_t T[10] = { 0, 178, 350, 512, 658, 784, 887, 962, 1008, 1024 };
    a %= 360;
    if (a < 0) a += 360;
    int neg = (a >= 180);
    a %= 180;
    if (a > 90) a = 180 - a;
    int i = a / 10, f = a % 10;
    int32_t v = (int32_t)T[i] + (i < 9 ? ((int32_t)T[i + 1] - (int32_t)T[i]) * f / 10 : 0);
    return neg ? -v : v;
}

int32_t asp_cos(int a) { return asp_sin(a + 90); }

struct asp *asp_open(void)
{
    return ASP;
}

unsigned asp_key(void)
{
    struct asp *a = ASP;
    unsigned k = h9_key();
    if (k != a->klast) { a->klast = (uint8_t)k; return 0; }
    if (k == a->kseen) return 0;
    if (k == H9_KEY_NONE) { a->kseen = 0; return 0; }
    a->kseen = (uint8_t)k;
    return k;
}

uint32_t asp_rnd(void)
{
    struct asp *a = ASP;
    a->rng ^= a->rng << 13;
    a->rng ^= a->rng >> 17;
    a->rng ^= a->rng << 5;
    return a->rng;
}

void asp_status(const char *m, uint16_t c)
{
    gfx_fill(0, 100, H9_WIDTH, 13, GFX_BLACK);
    gfx_text(2, 102, m, 26, 1, c, GFX_BLACK);
}