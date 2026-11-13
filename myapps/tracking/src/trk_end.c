/* Listed last in every module of build.json: the linker puts all .text
 * first, then .rodata in file order, so this is the last word of the body
 * (h9lib.s has no .rodata). trk_open compares it with the state address. */
#include "trk.h"

const uint32_t trk_end = 0x444E4554;   /* "TEND" */
