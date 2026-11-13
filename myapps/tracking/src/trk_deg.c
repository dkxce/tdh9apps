/* TRACKER degree text: linked only into the modules that print coordinates
 * (TRACKER, .TRK FIXED, .TRK RADAR), keeping Map small. */
#include "trk.h"

/* At most 9 characters: 5 decimals below 100 degrees, 4 above. */
char *deg_text(char *d, int32_t micro)
{
    if (micro >= 100000000 || micro <= -100000000) return fixed(d, micro / 100, 4);
    return fixed(d, micro / 10, 5);
}
