#pragma once
#include <stdint.h>
#include "util.h"
/* one decoded instruction (as make_gen.py stores it in disasm.json) */
typedef struct { int n; char *mn, *ops; } ins_t;
typedef struct {
    uint32_t base, size;              /* code object */
    ins_t **ins;                      /* by address - base; NULL if not decoded */
    uint8_t *isfunc;                  /* function starts */
    uint32_t *jt_tab, *jt_cnt;        /* by address - base: jump table at an indirect jmp (cnt 0 = none) */
    uint8_t *has_jt;
    uint32_t *fix_src, *fix_dst; size_t nfix;   /* sorted by source */
    int *body, *label;                /* stamps: == current function id */
} prog_t;
/* the C for one function (lift.py Fn.emit); unsup counts instructions it can't translate */
void lift_function(prog_t *P, int fid, uint32_t start, const uint32_t *addrs, int naddrs, sb_t *out, int *unsup);
int prog_fix(prog_t *P, uint32_t src, uint32_t *dst);
