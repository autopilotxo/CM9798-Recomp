#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
/* growable string */
typedef struct { char *s; size_t n, cap; } sb_t;
void sb_add(sb_t *b, const char *s);
void sb_addn(sb_t *b, const char *s, size_t n);
void sb_printf(sb_t *b, const char *fmt, ...);
void sb_free(sb_t *b);
/* temporary strings, freed together with tmp_reset() */
char *T(const char *fmt, ...);
char *Tdup(const char *s);
void tmp_reset(void);
