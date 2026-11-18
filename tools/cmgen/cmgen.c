/* cmgen.c - make_gen.py + tools/gen/img.py + gen.py + relocate.py in C, so the game can be
   generated on a player's machine with no Python or binutils. Its output is identical to make_gen.py's. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "cmgen.h"
#include "x86dis.h"
#include "lift.h"
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

#define SHIFT 0x1000000u
static char *g_err; static size_t g_errn;
static int fail(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vsnprintf(g_err, g_errn, fmt, a); va_end(a); return -1;
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc(sz + 1); if (!b) { fclose(f); return 0; }
    if (fread(b, 1, sz, f) != (size_t)sz) { fclose(f); free(b); return 0; }
    fclose(f); b[sz] = 0; if (n) *n = sz; return b;
}
static uint32_t crc32(const unsigned char *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const unsigned char *p) { return p[0] | p[1] << 8; }

/* ---- the LE image, as DOS/4GW loads it (img.py) */
typedef struct { uint32_t base; size_t size; unsigned char *buf; } obj_t;
typedef struct { uint32_t src, dst; } fx_t;
static int fxcmp(const void *a, const void *b) {
    const fx_t *x = a, *y = b;
    if (x->src != y->src) return x->src < y->src ? -1 : 1;
    if (x->dst != y->dst) return x->dst < y->dst ? -1 : 1;
    return 0;
}
static int load_le(const unsigned char *d, size_t dn, obj_t *objs, int *nobj, fx_t **fx, size_t *nfx) {
    const uint32_t mz = 0x26654, le = 0x290a4;
#define H(off) rd32(d + le + (off))
    uint32_t npages = H(0x14), ps = H(0x28), last = H(0x2c), dp = mz + H(0x80);
    uint32_t no = H(0x44); if (no > 8) return fail("bad LE header");
    uint32_t *pm = calloc(npages + 1, 4);
    for (uint32_t k = 0; k < npages; k++) { const unsigned char *b = d + le + H(0x48) + 4 * k; pm[k] = (b[0] << 16) | (b[1] << 8) | b[2]; }
    int pg_obj[4096]; uint32_t pg_off[4096];
    if (npages + 1 > 4096) return fail("bad LE header");
    for (uint32_t oi = 0; oi < no; oi++) {
        const unsigned char *o = d + le + H(0x40) + 24 * oi;
        uint32_t vs = rd32(o), base = rd32(o + 4), pmi = rd32(o + 12), pmn = rd32(o + 16);
        objs[oi].base = base + SHIFT; objs[oi].size = vs; objs[oi].buf = calloc(vs ? vs : 1, 1);
        for (uint32_t j = 0; j < pmn; j++) {
            uint32_t pg = pmi + j, num = pm[pg - 1], sz = pg == npages ? last : ps;
            size_t off = dp + (size_t)(num - 1) * ps;
            size_t avail = off < dn ? dn - off : 0; if (avail > sz) avail = sz;
            size_t dst = (size_t)j * ps; size_t room = dst < vs ? vs - dst : 0;
            size_t c = avail < room ? avail : room;
            memcpy(objs[oi].buf + dst, d + off, c);
            pg_obj[pg] = oi; pg_off[pg] = j * ps;
        }
    }
    *nobj = no;
    uint32_t *fp = malloc((npages + 1) * 4);
    for (uint32_t k = 0; k <= npages; k++) fp[k] = rd32(d + le + H(0x68) + 4 * k);
    size_t cap = 1 << 16, n = 0; fx_t *F = malloc(cap * sizeof *F);
    for (uint32_t p_ = 1; p_ <= npages; p_++) {
        size_t a = le + H(0x6c) + fp[p_ - 1], b = le + H(0x6c) + fp[p_];
        int so = pg_obj[p_]; uint32_t po = pg_off[p_];
        obj_t *O = &objs[so];
        while (a < b) {
            int src = d[a], flg = d[a + 1]; a += 2;
            int lst = src & 0x20, cnt = 0; int16_t soff = 0;
            if (lst) cnt = d[a++]; else { soff = (int16_t)rd16(d + a); a += 2; }
            int tobj = d[a++]; uint32_t toff;
            if (flg & 0x10) { toff = rd32(d + a); a += 4; } else { toff = rd16(d + a); a += 2; }
            int16_t offs1[1] = {soff}; const unsigned char *lp = d + a; int m = lst ? cnt : 1;
            if (lst) a += 2 * cnt;
            if (tobj < 1 || tobj > (int)no) return fail("CM2E16.EXE: bad fixup record");
            uint32_t val = objs[tobj - 1].base + toff;
            for (int k = 0; k < m; k++) {
                int32_t s = lst ? (int16_t)rd16(lp + 2 * k) : offs1[0];
                int64_t o = (int64_t)po + s;
                if (o >= 0 && o + 4 <= (int64_t)O->size) { unsigned char *q = O->buf + o; q[0] = val; q[1] = val >> 8; q[2] = val >> 16; q[3] = val >> 24; }
                else if (o >= 0 && o < (int64_t)O->size) for (int kk = 0; kk < 4; kk++) if (o + kk < (int64_t)O->size) O->buf[o + kk] = (unsigned char)(val >> (8 * kk));
                if (n == cap) { cap *= 2; F = realloc(F, cap * sizeof *F); }
                F[n].src = (uint32_t)(O->base + o); F[n].dst = val; n++;
            }
        }
    }
    /* sorted(set(...)) then dict(): the last (largest) target per source wins */
    qsort(F, n, sizeof *F, fxcmp);
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        if (w && F[w - 1].src == F[i].src) F[w - 1] = F[i]; else F[w++] = F[i];
    }
    *fx = F; *nfx = w;
    free(pm); free(fp);
    return 0;
#undef H
}

/* ---- functions.json: {"funcs": {"hex": [delta, ...], ...}, "jtabs": ["hex", ...]} */
typedef struct { uint32_t start; uint32_t *a; int n; } func_t;
static const char *skipws(const char *p) { while (*p && isspace((unsigned char)*p)) p++; return p; }
static const char *jstr(const char *p, char *out, size_t on) {
    p = skipws(p); if (*p != '"') return 0; p++;
    size_t k = 0; while (*p && *p != '"') { if (*p == '\\' && p[1]) p++; if (k + 1 < on) out[k++] = *p; p++; }
    out[k] = 0; return *p == '"' ? p + 1 : 0;
}
static const char *jskip(const char *p) {     /* skip one JSON value */
    p = skipws(p);
    if (*p == '"') { char t[8]; const char *q = p + 1; while (*q && *q != '"') { if (*q == '\\' && q[1]) q++; q++; } (void)t; return *q ? q + 1 : 0; }
    if (*p == '{' || *p == '[') { int depth = 0; do { if (*p == '"') { p = jskip(p); if (!p) return 0; continue; } if (*p == '{' || *p == '[') depth++; else if (*p == '}' || *p == ']') depth--; p++; } while (*p && depth); return p; }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}
static int load_manifest(const char *path, func_t **fs, int *nf, uint32_t **jt, int *njt) {
    char *s = (char *)slurp(path, 0); if (!s) return fail("can't read %s", path);
    const char *p = skipws(s); if (*p++ != '{') return fail("%s: not JSON", path);
    *fs = 0; *nf = 0; *jt = 0; *njt = 0;
    for (;;) {
        char key[64]; p = jstr(p, key, sizeof key); if (!p) break;
        p = skipws(p); if (*p++ != ':') return fail("%s: bad JSON", path);
        if (!strcmp(key, "funcs")) {
            p = skipws(p); if (*p++ != '{') return fail("%s: bad funcs", path);
            int cap = 2048; *fs = malloc(cap * sizeof **fs);
            for (;;) {
                p = skipws(p); if (*p == '}') { p++; break; }
                char k2[32]; p = jstr(p, k2, sizeof k2); if (!p) return fail("%s: bad funcs", path);
                p = skipws(p); if (*p++ != ':') return fail("%s: bad funcs", path);
                p = skipws(p); if (*p++ != '[') return fail("%s: bad funcs", path);
                if (*nf == cap) { cap *= 2; *fs = realloc(*fs, cap * sizeof **fs); }
                func_t *F = &(*fs)[(*nf)++]; F->start = (uint32_t)strtoul(k2, 0, 16) + SHIFT; F->n = 0;
                int ac = 64; F->a = malloc(ac * 4); uint32_t a = F->start;
                for (;;) {
                    p = skipws(p); if (*p == ']') { p++; break; }
                    char *e; long dlt = strtol(p, &e, 10); if (e == p) return fail("%s: bad funcs", path); p = skipws(e);
                    a += (uint32_t)dlt; if (F->n == ac) { ac *= 2; F->a = realloc(F->a, ac * 4); } F->a[F->n++] = a;
                    if (*p == ',') p++;
                }
                p = skipws(p); if (*p == ',') p++;
            }
        } else if (!strcmp(key, "jtabs")) {
            p = skipws(p); if (*p++ != '[') return fail("%s: bad jtabs", path);
            int cap = 64; *jt = malloc(cap * 4);
            for (;;) {
                p = skipws(p); if (*p == ']') { p++; break; }
                char k2[32]; p = jstr(p, k2, sizeof k2); if (!p) return fail("%s: bad jtabs", path);
                if (*njt == cap) { cap *= 2; *jt = realloc(*jt, cap * 4); }
                (*jt)[(*njt)++] = (uint32_t)strtoul(k2, 0, 16) + SHIFT;
                p = skipws(p); if (*p == ',') p++;
            }
        } else { p = jskip(p); if (!p) return fail("%s: bad JSON", path); }
        p = skipws(p); if (*p == ',') { p++; continue; }
        break;
    }
    free(s);
    if (!*nf) return fail("%s: no functions", path);
    return 0;
}

/* ---- text files: patches.txt, overrides.txt, relocations.txt, labels.csv */
typedef struct { uint32_t addr; int after; char *stmt; int applied; } patch_t;
typedef struct { uint32_t lo, hi, nb; int blk; uint32_t base, old_end, new_end; } move_t;
typedef struct { uint32_t addr, old, nw; } immr_t;
static char *strip(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t L = strlen(s); while (L && isspace((unsigned char)s[L - 1])) s[--L] = 0;
    return s;
}
static char **lines_of(const char *path, int *n, int required) {
    char *s = (char *)slurp(path, 0); *n = 0;
    if (!s) { if (required) fail("can't read %s", path); return 0; }
    int cap = 256; char **L = malloc(cap * sizeof *L);
    for (char *p = s; *p; ) {
        char *e = strchr(p, '\n'); if (e) *e = 0;
        if (*n == cap) { cap *= 2; L = realloc(L, cap * sizeof *L); }
        L[(*n)++] = p;
        if (!e) break; p = e + 1;
    }
    return L;
}

/* ---- relocate.py */
static int is_hexd(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
static int is_wc(char c) { return isalnum((unsigned char)c) || c == '_'; }
static void relocate_line(const char *ln, move_t *mv, int nmv, immr_t *im, int nim, sb_t *out) {
    /* ^(\s*\/\* ([0-9a-f]+): (.*?) \*\/)(.*)$ */
    const char *p = ln; while (*p && isspace((unsigned char)*p)) p++;
    if (p[0] != '/' || p[1] != '*' || p[2] != ' ') { sb_add(out, ln); return; }
    const char *h = p + 3, *q = h; while (is_hexd(*q)) q++;
    if (q == h || q[0] != ':' || q[1] != ' ') { sb_add(out, ln); return; }
    const char *asm0 = q + 2; const char *ce = strstr(asm0, " */");
    if (!ce) { sb_add(out, ln); return; }
    uint32_t addr = (uint32_t)strtoul(h, 0, 16);
    const char *body = ce + 3;
    /* asm_vals */
    uint32_t vals[16]; int nv = 0;
    for (const char *r = asm0; r < ce; r++) {
        if (r[0] == '0' && r[1] == 'x' && is_hexd(r[2])) {
            const char *e = r + 2; while (e < ce && is_hexd(*e)) e++;
            if (nv < 16) vals[nv++] = (uint32_t)strtoull(r + 2, 0, 16);
            r = e - 1;
        }
    }
    sb_addn(out, ln, body - ln);
    sb_t b = {0};
    for (const char *r = body; *r; ) {
        if (r[0] == '0' && r[1] == 'x' && is_hexd(r[2])) {
            const char *e = r + 2; while (is_hexd(*e)) e++;
            if (*e == 'u') {
                uint32_t v = (uint32_t)strtoull(r + 2, 0, 16); int done = 0, inasm = 0;
                for (int k = 0; k < nv; k++) if (vals[k] == v) inasm = 1;
                if (inasm) for (int k = 0; k < nmv; k++) if (mv[k].lo <= v && v < mv[k].hi) {
                    uint32_t nvv;
                    if (mv[k].blk) { int64_t off = (int64_t)v - mv[k].base; nvv = (uint32_t)(mv[k].nb + (off < (int64_t)mv[k].old_end ? off : (int64_t)mv[k].new_end + off - mv[k].old_end)); }
                    else nvv = mv[k].nb + (v - mv[k].lo);
                    sb_printf(&b, "0x%xu", nvv); done = 1; break;
                }
                if (!done) sb_addn(&b, r, e + 1 - r);
                r = e + 1; continue;
            }
        }
        sb_addn(&b, r++, 1);
    }
    const char *bd = b.s ? b.s : "";
    for (int k = 0; k < nim; k++) if (im[k].addr == addr) {
        char old[24], nw[24]; snprintf(old, sizeof old, "0x%xu", im[k].old); snprintf(nw, sizeof nw, "0x%xu", im[k].nw);
        size_t ol = strlen(old); sb_t c = {0};
        for (const char *r = bd; *r; ) {
            if (!strncmp(r, old, ol) && (r == bd || !is_wc(r[-1])) && !is_wc(r[ol])) { sb_add(&c, nw); r += ol; }
            else sb_addn(&c, r++, 1);
        }
        sb_add(out, c.s ? c.s : ""); sb_free(&c); sb_free(&b); return;
    }
    sb_add(out, bd); sb_free(&b);
}

static int cmp_u32(const void *a, const void *b) { uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
static int cmp_func(const void *a, const void *b) { return cmp_u32(&((const func_t *)a)->start, &((const func_t *)b)->start); }
static int cmp_cstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static int py_isupper(const char *s) { int cased = 0; for (; *s; s++) { if (islower((unsigned char)*s)) return 0; if (isupper((unsigned char)*s)) cased = 1; } return cased; }

static int write_file(const char *dir, const char *name, const char *s, size_t n) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb"); if (!f) return fail("can't write %s: %s", path, strerror(errno));
    if (fwrite(s, 1, n, f) != n) { fclose(f); return fail("can't write %s", path); }
    fclose(f); return 0;
}

int cmgen_run(const char *exe, const char *datadir, const char *outdir,
              void (*progress)(int, const char *, void *), void *ctx, char *err, size_t errn)
{
    g_err = err; g_errn = errn; if (errn) err[0] = 0;
#define PROG(x, w) do { if (progress) progress((x), (w), ctx); } while (0)
    char path[1024];
    size_t dn; unsigned char *d = slurp(exe, &dn);
    if (!d) return fail("can't read %s", exe);
    if (dn != CMGEN_EXE_SIZE || crc32(d, dn) != CMGEN_EXE_CRC32)
        return fail("%s is not CM2E16.EXE v2.93 (size %zu, CRC-32 %08X; expected %u, %08X).", exe, dn, crc32(d, dn), CMGEN_EXE_SIZE, CMGEN_EXE_CRC32);
    PROG(0, "loading the EXE");
    obj_t objs[8]; int nobj; fx_t *fx; size_t nfx;
    if (load_le(d, dn, objs, &nobj, &fx, &nfx)) return -1;
    prog_t P; memset(&P, 0, sizeof P);
    P.base = objs[0].base; P.size = (uint32_t)objs[0].size;
    const unsigned char *code = objs[0].buf;
    P.fix_src = malloc(nfx * 4); P.fix_dst = malloc(nfx * 4); P.nfix = nfx;
    for (size_t i = 0; i < nfx; i++) { P.fix_src[i] = fx[i].src; P.fix_dst[i] = fx[i].dst; }

    func_t *fs = 0; int nf = 0; uint32_t *jt = 0; int njt = 0;
    snprintf(path, sizeof path, "%s/functions.json", datadir);
    if (load_manifest(path, &fs, &nf, &jt, &njt)) return -1;
    qsort(fs, nf, sizeof *fs, cmp_func);
    P.ins = calloc(P.size, sizeof *P.ins); P.isfunc = calloc(P.size, 1);
    P.jt_tab = calloc(P.size, 4); P.jt_cnt = calloc(P.size, 4); P.has_jt = calloc(P.size, 1);
    P.body = calloc(P.size, sizeof(int)); P.label = calloc(P.size, sizeof(int));
    for (uint32_t i = 0; i < P.size; i++) P.body[i] = P.label[i] = -1;
#define INCODE(a) ((a) >= P.base && (a) < P.base + P.size)
    PROG(50, "decoding instructions");
    size_t ndec = 0;
    for (int i = 0; i < nf; i++) {
        if (!INCODE(fs[i].start)) return fail("function %x outside the code", fs[i].start - SHIFT);
        P.isfunc[fs[i].start - P.base] = 1;
        for (int k = 0; k < fs[i].n; k++) {
            uint32_t a = fs[i].a[k]; if (!INCODE(a)) return fail("instruction %x outside the code", a - SHIFT);
            if (P.ins[a - P.base]) continue;
            char mn[64], ops[160];
            int n = x86_dis_line(code + (a - P.base), (int)(P.size - (a - P.base)), a, mn, ops);
            if (!n) return fail("can't decode the instruction at %x", a - SHIFT);
            ins_t *I = malloc(sizeof *I); I->n = n; I->mn = strdup(mn); I->ops = strdup(ops);
            P.ins[a - P.base] = I; ndec++;
        }
    }
    for (int i = 0; i < njt; i++) {
        uint32_t a = jt[i]; if (!INCODE(a) || !P.ins[a - P.base]) return fail("no jump table at %x", a - SHIFT);
        /* \*(?:%cs:)?0x([0-9a-f]+)\(,%e..,4\) */
        const char *o = P.ins[a - P.base]->ops;
        if (*o++ != '*') return fail("no jump table at %x", a - SHIFT);
        if (!strncmp(o, "%cs:", 4)) o += 4;
        if (strncmp(o, "0x", 2)) return fail("no jump table at %x", a - SHIFT);
        char *e; uint32_t tb = (uint32_t)strtoul(o + 2, &e, 16);
        if (e == o + 2 || strncmp(e, "(,%e", 4) || !e[4] || !e[5] || strcmp(e + 6, ",4)")) return fail("no jump table at %x", a - SHIFT);
        uint32_t n = 0, t;
        while (prog_fix(&P, tb + 4 * n, &t) && t >= P.base && t < P.base + P.size) n++;
        P.has_jt[a - P.base] = 1; P.jt_tab[a - P.base] = tb; P.jt_cnt[a - P.base] = n;
    }
    /* overrides, patches, relocations, labels */
    int nl; char **L;
    uint32_t *ovr = 0; int novr = 0;
    snprintf(path, sizeof path, "%s/overrides.txt", datadir);
    if ((L = lines_of(path, &nl, 0))) {
        ovr = malloc(nl * 4);
        for (int i = 0; i < nl; i++) { char *c = strchr(L[i], '#'); if (c) *c = 0; char *t = strip(L[i]); if (*t) ovr[novr++] = (uint32_t)strtoul(t, 0, 16) + SHIFT; }
    }
    patch_t *pt = 0; int npt = 0;
    char **hooks = 0; int nhooks = 0;
    snprintf(path, sizeof path, "%s/patches.txt", datadir);
    if ((L = lines_of(path, &nl, 0))) {
        pt = calloc(nl, sizeof *pt); hooks = malloc(sizeof(char *) * 4096);
        for (int i = 0; i < nl; i++) {
            char *c = strchr(L[i], '#'); if (c) *c = 0; char *t = strip(L[i]); if (!*t) continue;
            char *a_ = t; while (*t && !isspace((unsigned char)*t)) t++; if (*t) *t++ = 0; while (isspace((unsigned char)*t)) t++;
            char *kind = t; while (*t && !isspace((unsigned char)*t)) t++; if (*t) *t++ = 0; while (isspace((unsigned char)*t)) t++;
            if (strcmp(kind, "replace") && strcmp(kind, "after")) return fail("patches.txt: %s", L[i]);
            uint32_t addr = (uint32_t)strtoul(a_, 0, 16) + SHIFT; int k;
            for (k = 0; k < npt; k++) if (pt[k].addr == addr) break;       /* dict: the last line wins */
            if (k == npt) npt++;
            pt[k].addr = addr; pt[k].after = !strcmp(kind, "after"); pt[k].stmt = t; pt[k].applied = 0;
            /* hooks.update(re.findall(r'([A-Za-z_]\w*)\s*\(', stmt)) */
            for (char *r = t; *r; ) {
                if ((isalpha((unsigned char)*r) || *r == '_') && (r == t || !is_wc(r[-1]))) {
                    char *e = r; while (is_wc(*e)) e++;
                    char *f = e; while (isspace((unsigned char)*f)) f++;
                    if (*f == '(') {
                        char nm[128]; snprintf(nm, sizeof nm, "%.*s", (int)(e - r), r); int dup = 0;
                        for (int h = 0; h < nhooks; h++) if (!strcmp(hooks[h], nm)) dup = 1;
                        if (!dup && nhooks < 4096) hooks[nhooks++] = strdup(nm);
                    }
                    r = e;
                } else r++;
            }
        }
    }
    move_t *mv = 0; int nmv = 0; immr_t *im = 0; int nim = 0;
    snprintf(path, sizeof path, "%s/relocations.txt", datadir);
    if (!(L = lines_of(path, &nl, 1))) return -1;
    mv = calloc(nl, sizeof *mv); im = calloc(nl, sizeof *im);
    for (int i = 0; i < nl; i++) {
        char *c = strchr(L[i], '#'); if (c) *c = 0;
        char *w[8]; int nw = 0; for (char *t = strtok(L[i], " \t\r"); t && nw < 8; t = strtok(0, " \t\r")) w[nw++] = t;
        if (!nw) continue;
        if (!strcmp(w[0], "move") && nw >= 4) { move_t m = {0}; m.lo = strtoul(w[1], 0, 16); m.hi = strtoul(w[2], 0, 16); m.nb = strtoul(w[3], 0, 16); mv[nmv++] = m; }
        else if (!strcmp(w[0], "blocks") && nw >= 7) { move_t m = {0}; m.lo = strtoul(w[1], 0, 16); m.hi = strtoul(w[2], 0, 16); m.base = strtoul(w[3], 0, 16);
            m.old_end = strtoul(w[4], 0, 16); m.new_end = strtoul(w[5], 0, 16); m.nb = strtoul(w[6], 0, 16); m.blk = 1; mv[nmv++] = m; }
        else if (!strcmp(w[0], "imm") && nw >= 4) {
            uint32_t a = strtoul(w[1], 0, 16); int k; for (k = 0; k < nim; k++) if (im[k].addr == a) break;
            if (k == nim) nim++; im[k].addr = a; im[k].old = strtoul(w[2], 0, 16); im[k].nw = strtoul(w[3], 0, 16); }
        else return fail("relocations.txt: %s", w[0]);
    }
    char **names = calloc(P.size, sizeof *names);
    snprintf(path, sizeof path, "%s/labels.csv", datadir);
    if ((L = lines_of(path, &nl, 0)))
        for (int i = 0; i < nl; i++) {
            char *t = strip(L[i]); char *c = strchr(t, ','); if (!c) continue; *c = 0;
            uint32_t a = (uint32_t)strtoul(t, 0, 16) + SHIFT; if (INCODE(a)) names[a - P.base] = c + 1;
        }

    MKDIR(outdir);
    /* funcs.h */
    sb_t h = {0};
    sb_add(&h, "/* generated */\n#pragma once\n#include \"rt.h\"\n");
    qsort(hooks, nhooks, sizeof *hooks, cmp_cstr);
    for (int i = 0; i < nhooks; i++) if (!py_isupper(hooks[i]))
        sb_printf(&h, "%s %s(void);  /* patches.txt, rt/ */\n", !strncmp(hooks[i], "hook_", 5) ? "void" : "uint32_t", hooks[i]);
    for (int i = 0; i < nf; i++) {
        uint32_t f = fs[i].start; int ov = 0; for (int k = 0; k < novr; k++) if (ovr[k] == f) ov = 1;
        sb_printf(&h, "void f_%06x(void);", f);
        if (names[f - P.base]) sb_printf(&h, " /* %s */", names[f - P.base]);
        sb_add(&h, "\n");
        if (ov) sb_printf(&h, "void f_%06x_orig(void);  /* generated original, overridden in rt/overrides.c */\n", f);
    }
    if (write_file(outdir, "funcs.h", h.s, h.n)) return -1;
    sb_free(&h);
    /* game_NNN.c */
    int nunsup = 0; const int per_file = 60;
    for (int i0 = 0; i0 < nf; i0 += per_file) {
        sb_t c = {0};
        sb_add(&c, "/* generated by gen.py from CM2E16.EXE - do not edit, override in hand/ instead */\n#include \"funcs.h\"\n");
        sb_add(&c, "#pragma GCC diagnostic ignored \"-Wunused-label\"\n#pragma GCC diagnostic ignored \"-Wunused-variable\"\n#pragma GCC diagnostic ignored \"-Wunused-but-set-variable\"\n\n");
        for (int i = i0; i < nf && i < i0 + per_file; i++) {
            func_t *F = &fs[i]; uint32_t f = F->start;
            if (names[f - P.base]) sb_printf(&c, "/* %s */\n", names[f - P.base]);
            sb_t code = {0};
            lift_function(&P, i, f, F->a, F->n, &code, &nunsup);
            /* patches, relocations, overrides: line by line */
            sb_t o2 = {0}; const char *s = code.s;
            int first_hdr_done = 0; char hdr[40]; snprintf(hdr, sizeof hdr, "void f_%06x(void) {", f);
            int ov = 0; for (int k = 0; k < novr; k++) if (ovr[k] == f) ov = 1;
            for (;;) {
                const char *e = strchr(s, '\n'); size_t ln_n = e ? (size_t)(e - s) : strlen(s);
                char *ln = malloc(ln_n + 1); memcpy(ln, s, ln_n); ln[ln_n] = 0;
                sb_t pl = {0};
                /* apply_patches */
                const char *st = ln; while (isspace((unsigned char)*st)) st++;
                int patched = 0;
                if (npt && !strncmp(st, "/* ", 3)) {
                    const char *colon = strchr(st, ':');
                    if (colon) {
                        const char *hx = st + 3; int ok = colon > hx; for (const char *r = hx; r < colon; r++) if (!isxdigit((unsigned char)*r)) ok = 0;
                        if (ok) {
                            uint32_t a_ = (uint32_t)strtoul(hx, 0, 16);
                            for (int k = 0; k < npt; k++) if (pt[k].addr == a_) {
                                const char *ce = strstr(ln, "*/");
                                if (pt[k].after) sb_printf(&pl, "%s %s  /* patched */", ln, pt[k].stmt);
                                else { sb_addn(&pl, ln, ce + 2 - ln); sb_printf(&pl, " %s  /* patched */", pt[k].stmt); }
                                pt[k].applied = 1; patched = 1; break;
                            }
                        }
                    }
                }
                if (!patched) sb_add(&pl, ln);
                sb_t rl = {0}; relocate_line(pl.s ? pl.s : "", mv, nmv, im, nim, &rl);
                const char *outl = rl.s ? rl.s : "";
                if (ov && !first_hdr_done && !strncmp(outl, hdr, strlen(hdr)) ) {
                    /* code.replace('void f_X(void) {', 'void f_X_orig(void) {', 1): the header is the first line */
                    sb_printf(&o2, "void f_%06x_orig(void) {%s", f, outl + strlen(hdr)); first_hdr_done = 1;
                } else sb_add(&o2, outl);
                sb_free(&pl); sb_free(&rl); free(ln);
                if (!e) break;
                sb_add(&o2, "\n"); s = e + 1;
            }
            sb_add(&c, o2.s); sb_add(&c, "\n\n");
            sb_free(&o2); sb_free(&code);
        }
        char nm[32]; snprintf(nm, sizeof nm, "game_%03d.c", i0 / per_file);
        if (write_file(outdir, nm, c.s, c.n)) return -1;
        sb_free(&c);
        PROG(100 + 850 * (i0 + per_file < nf ? i0 + per_file : nf) / nf, "generating C");
    }
    /* functable.c */
    sb_t t = {0};
    sb_add(&t, "/* generated */\n#include \"funcs.h\"\n#include \"rt_internal.h\"\nconst struct gfunc g_funcs[] = {\n");
    for (int i = 0; i < nf; i++) sb_printf(&t, "  {0x%xu, f_%06x},\n", fs[i].start, fs[i].start);
    sb_printf(&t, "};\nconst unsigned g_nfuncs = %d;\n", nf);
    if (write_file(outdir, "functable.c", t.s, t.n)) return -1;
    sb_free(&t);
    /* retsites.c: the address after every call */
    uint32_t *rets = malloc(ndec * 4); size_t nr = 0;
    for (uint32_t i = 0; i < P.size; i++) if (P.ins[i]) {
        const char *m = P.ins[i]->mn; size_t ml = strlen(m);
        if (ml >= 4 && !strcmp(m + ml - 4, "call") && (ml == 4 || isspace((unsigned char)m[ml - 5]))) rets[nr++] = P.base + i + P.ins[i]->n;
    }
    qsort(rets, nr, 4, cmp_u32);
    size_t nu = 0; for (size_t i = 0; i < nr; i++) if (!nu || rets[nu - 1] != rets[i]) rets[nu++] = rets[i];
    sb_add(&t, "/* generated: addresses following a call instruction */\n#include <stdint.h>\nconst uint32_t g_retsites[] = {\n");
    for (size_t i = 0; i < nu; i++) sb_printf(&t, "0x%xu,\n", rets[i]);
    sb_printf(&t, "};\nconst unsigned g_nretsites = %zu;\n", nu);
    if (write_file(outdir, "retsites.c", t.s, t.n)) return -1;
    sb_free(&t);
    for (int k = 0; k < npt; k++) if (!pt[k].applied) return fail("patches.txt: no instruction at %06x", pt[k].addr - SHIFT);
    if (nunsup) return fail("%d instructions could not be translated", nunsup);
    PROG(1000, "done");
    return 0;
}

#ifdef CMGEN_MAIN
static void prog(int pm, const char *w, void *c) { (void)c; fprintf(stderr, "\r%3d%% %s   ", pm / 10, w); }
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: cmgen <CM2E16.EXE> <data dir> <out dir>\n"); return 2; }
    char err[512];
    if (cmgen_run(argv[1], argv[2], argv[3], prog, 0, err, sizeof err)) { fprintf(stderr, "\n%s\n", err); return 1; }
    fprintf(stderr, "\n"); return 0;
}
#endif
