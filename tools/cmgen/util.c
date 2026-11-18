#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util.h"
void sb_addn(sb_t *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 64; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
void sb_add(sb_t *b, const char *s) { sb_addn(b, s, strlen(s)); }
void sb_printf(sb_t *b, const char *fmt, ...) {
    va_list a; va_start(a, fmt); char tmp[512]; int n = vsnprintf(tmp, sizeof tmp, fmt, a); va_end(a);
    if (n < (int)sizeof tmp) { sb_addn(b, tmp, n); return; }
    char *big = malloc(n + 1); va_start(a, fmt); vsnprintf(big, n + 1, fmt, a); va_end(a); sb_addn(b, big, n); free(big);
}
void sb_free(sb_t *b) { free(b->s); b->s = 0; b->n = b->cap = 0; }
static char **tmps; static size_t ntmp, captmp;
static char *keep(char *s) {
    if (ntmp == captmp) { captmp = captmp * 2 + 256; tmps = realloc(tmps, captmp * sizeof *tmps); }
    return tmps[ntmp++] = s;
}
char *T(const char *fmt, ...) {
    va_list a; va_start(a, fmt); int n = vsnprintf(0, 0, fmt, a); va_end(a);
    char *s = malloc(n + 1); va_start(a, fmt); vsnprintf(s, n + 1, fmt, a); va_end(a); return keep(s);
}
char *Tdup(const char *s) { return keep(strdup(s)); }
void tmp_reset(void) { for (size_t i = 0; i < ntmp; i++) free(tmps[i]); ntmp = 0; }
