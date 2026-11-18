#pragma once
#include "rt.h"
struct gfunc { uint32_t addr; gfn fn; };
extern const struct gfunc g_funcs[];
extern const unsigned g_nfuncs;
gfn rt_lookup(uint32_t addr);
int  rt_load_image(const char *dir);
void rt_init(void);
