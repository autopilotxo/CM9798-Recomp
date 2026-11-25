/* editor_none.c - empty editor tables, for a game made without CM2ED.EXE (no Database Editor).
 * Linked only when gen/ has no ed_*.c: the library's copy is used only if nothing else defines them. */
#include "rt_internal.h"
const struct gfunc e_funcs[1] = { { 0, 0 } };
const unsigned e_nfuncs = 0;
const uint32_t e_retsites[1] = { 0 };
const unsigned e_nretsites = 0;
