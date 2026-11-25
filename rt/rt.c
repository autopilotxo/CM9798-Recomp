/* rt.c - core runtime for the recompiled CM2E16. */
#include "rt_internal.h"
#include <stdio.h>
#include <stdlib.h>

uint8_t *M;
uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
uint32_t g_cf, g_zf, g_sf, g_of, g_df, g_ret;
uint32_t sel_[6], segbase_[6];
/* descriptor table (8192 entries, indexed by selector>>3) */
uint32_t desc_base[8192], desc_limit[8192]; uint8_t desc_used[8192];
void rt_set_seg(int i, uint32_t sel) { sel_[i] = sel & 0xFFFF; segbase_[i] = desc_base[(sel & 0xFFFF) >> 3]; }
long double fst_[8]; int ftop; uint32_t fsw, fcw = 0x37F;

/* ---------- image ---------- */
/* Image is relocated +16MB so linear 0..1MB stays free for DOS memory (BIOS data, VGA, DOS buffers). */
#define ADDR_SHIFT 0x01000000u
#define CODE_BASE (0x00010000u+ADDR_SHIFT)
#define DATA_BASE (0x000E0000u+ADDR_SHIFT)
#define STACK_TOP (0x00AFEAB0u+ADDR_SHIFT)   /* LE: ESP = object 2 + 0xA1EAB0 */

static long load_file(const char *path, uint32_t at) {
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    long n = (long)fread(M + at, 1, GUEST_MEM_SIZE - at, f); fclose(f); return n;
}
int rt_load_image(const char *dir) {
    char p[1024];
    snprintf(p, sizeof p, "%s/obj1_01010000.bin", dir); if (load_file(p, CODE_BASE) <= 0) return -1;
    snprintf(p, sizeof p, "%s/obj2_010e0000.bin", dir); if (load_file(p, DATA_BASE) <= 0) return -1;
    return 0;
}
void rt_init(void) {
    M = calloc(1, GUEST_MEM_SIZE);
    if (!M) { fprintf(stderr, "out of memory\n"); exit(1); }
    esp = STACK_TOP;
}

/* ---------- call trace (build with -DCALLTRACE) ---------- */
uint32_t g_trace_ring[256]; unsigned g_trace_pos; uint32_t g_break_fn, g_watch_addr;
/* poor man's watchpoint: checked at every function entry (CALLTRACE builds, CM_WATCH=<hex addr>) */
void rt_watch(uint32_t a) {
    static uint32_t last; static int init, reports;
    uint32_t v = RD32(g_watch_addr);
    if (!init) { init = 1; last = v; return; }
    if (v != last && reports++ < 40) {
        fprintf(stderr, "[watch] %08x: %08x -> %08x, noticed entering %06x (previous fn %06x)\n",
                g_watch_addr, last, v, a - 0x01000000u, g_trace_ring[(g_trace_pos - 2) & 255] - 0x01000000u);
        if (reports == 1) rt_dump_trace();          /* the first time: how it got there */
    }
    last = v;
}
void rt_break(uint32_t a) {
    static int hits; int maxh = getenv("CM_BREAK_N") ? atoi(getenv("CM_BREAK_N")) : 1;
    if (++hits > maxh) return;
    fprintf(stderr, "[break] %06x hit #%d  EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x ESP=%08x\n",
            a - 0x01000000u, hits, eax, ebx, ecx, edx, esi, edi, esp);
    if (eax > 0x01000000u && eax < GUEST_MEM_SIZE - 64) {
        char t[80]; int i = 0; for (; i < 79 && M[eax + i] >= 32 && M[eax + i] < 127; i++) t[i] = (char)M[eax + i]; t[i] = 0;
        if (i > 2) fprintf(stderr, "        EAX -> \"%s\"\n", t);
    }
    fprintf(stderr, "        caller (return address) %06x, stack arg %08x\n", RD32(esp) - 0x01000000u, RD32(esp + 4));
    rt_dump_trace();
}
void rt_dump_trace(void) {
    fprintf(stderr, "last functions entered (oldest first, original addresses):\n");
    for (unsigned i = 0; i < 256; i++) { uint32_t a = g_trace_ring[(g_trace_pos + i) & 255];
        if (a) fprintf(stderr, " %06x", a - 0x01000000u); }
    fprintf(stderr, "\n");
}

/* ---------- the running program's tables: the game's (gen/), or the editor's (rt/editor.c) ---------- */
extern const uint32_t g_retsites[]; extern const unsigned g_nretsites;
static const struct gfunc *p_funcs; static unsigned p_nfuncs;       /* NULL: the game's */
static const uint32_t *p_rets; static unsigned p_nrets;
void rt_set_program(const struct gfunc *f, unsigned nf, const uint32_t *ret, unsigned nret) {
    p_funcs = f; p_nfuncs = nf; p_rets = ret; p_nrets = nret;
}

/* ---------- return sites ---------- */
int rt_is_retaddr(uint32_t a) {
    if (a < CODE_BASE || a >= DATA_BASE) return 1;     /* outside code (e.g. host sentinel): plain return */
    const uint32_t *r = p_funcs ? p_rets : g_retsites; unsigned lo = 0, hi = p_funcs ? p_nrets : g_nretsites;
    while (lo < hi) { unsigned m = (lo + hi) / 2; if (r[m] == a) return 1; if (r[m] < a) lo = m + 1; else hi = m; }
    return 0;
}

/* ---------- dispatch ---------- */
gfn rt_lookup(uint32_t a) {
    const struct gfunc *F = p_funcs ? p_funcs : g_funcs; unsigned lo = 0, hi = p_funcs ? p_nfuncs : g_nfuncs;
    while (lo < hi) { unsigned mid = (lo + hi) / 2;
        if (F[mid].addr == a) return F[mid].fn;
        if (F[mid].addr < a) lo = mid + 1; else hi = mid; }
    return 0;
}

/* ---------- state (rt_internal.h) ---------- */
size_t rt_vars_size(const struct rt_var *v, int n) { size_t t = 0; for (int i = 0; i < n; i++) t += v[i].n; return t; }
void rt_vars_swap(const struct rt_var *v, int n, unsigned char *buf) {
    for (int i = 0; i < n; i++) {
        unsigned char *p = v[i].p;
        for (size_t k = 0; k < v[i].n; k++) { unsigned char t = p[k]; p[k] = buf[k]; buf[k] = t; }
        buf += v[i].n;
    }
}
static const struct rt_var rt_vars[] = {
    { &M, sizeof M }, { &eax, 4 }, { &ecx, 4 }, { &edx, 4 }, { &ebx, 4 }, { &esp, 4 }, { &ebp, 4 }, { &esi, 4 }, { &edi, 4 },
    { &g_cf, 4 }, { &g_zf, 4 }, { &g_sf, 4 }, { &g_of, 4 }, { &g_df, 4 }, { &g_ret, 4 },
    { sel_, sizeof sel_ }, { segbase_, sizeof segbase_ },
    { desc_base, sizeof desc_base }, { desc_limit, sizeof desc_limit }, { desc_used, sizeof desc_used },
    { fst_, sizeof fst_ }, { &ftop, sizeof ftop }, { &fsw, sizeof fsw }, { &fcw, sizeof fcw },
    { &p_funcs, sizeof p_funcs }, { &p_nfuncs, sizeof p_nfuncs }, { &p_rets, sizeof p_rets }, { &p_nrets, sizeof p_nrets },
};
#define NV(v) (int)(sizeof v / sizeof v[0])
size_t rt_state_size(void) { return rt_vars_size(rt_vars, NV(rt_vars)); }
void rt_state_swap(void *buf) { rt_vars_swap(rt_vars, NV(rt_vars), buf); }
void rt_call(uint32_t t) {
    gfn f = rt_lookup(t);
    if (f) { f(); return; }
    /* Watcom int386 thunks: "int nn ; ret" built in the code object */
    if (t < GUEST_MEM_SIZE - 3 && M[t] == 0xCD && M[t + 2] == 0xC3) { rt_int(M[t + 1]); g_ret = RD32(esp); esp += 4; return; }
    fprintf(stderr, "rt_call: no guest function at %06x (esp=%08x)\n", t, esp); exit(2);
}

/* ---------- traps / stubs (filled in by the DOS, DPMI, VESA and Miles shims) ---------- */
void rt_unimpl(uint32_t a, const char *w) { fprintf(stderr, "unimplemented instruction at %06x: %s\n", a, w); exit(3); }
void rt_divide_error(uint32_t a) { fprintf(stderr, "divide error at %06x\n", a); exit(4); }
void rt_sys(uint32_t a, const char *w) { fprintf(stderr, "system instruction at %06x: %s (ignored)\n", a, w); }

/* ---------- eflags ---------- */
uint32_t rt_eflags(void) { return 0x202u | g_cf | (g_zf << 6) | (g_sf << 7) | (g_df << 10) | (g_of << 11); }
void rt_set_eflags(uint32_t f) { g_cf = f & 1; g_zf = (f >> 6) & 1; g_sf = (f >> 7) & 1; g_df = (f >> 10) & 1; g_of = (f >> 11) & 1; }

/* ---------- rotates ---------- */
uint32_t rt_rot(int op, int w, uint32_t a, uint32_t n) {
    uint32_t m = w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1); a &= m;
    if (op <= 1) {               /* rol / ror */
        uint32_t k = n % w; if (n == 0) return a;
        uint32_t r = op == 0 ? ((a << k) | (a >> ((w - k) % w))) & m : ((a >> k) | (a << ((w - k) % w))) & m;
        if (k == 0) r = a;
        g_cf = op == 0 ? (r & 1) : (r >> (w - 1)) & 1;
        if (n == 1) g_of = op == 0 ? (((r >> (w - 1)) & 1) ^ g_cf) : (((r >> (w - 1)) ^ (r >> (w - 2))) & 1);
        return r;
    }
    n %= (w + 1);                /* rcl / rcr through carry */
    for (uint32_t i = 0; i < n; i++) {
        if (op == 2) { uint32_t c = (a >> (w - 1)) & 1; a = ((a << 1) | g_cf) & m; g_cf = c; }
        else { uint32_t c = a & 1; a = (a >> 1) | (g_cf << (w - 1)); g_cf = c; }
    }
    if (n == 1) g_of = op == 2 ? (((a >> (w - 1)) & 1) ^ g_cf) : (((a >> (w - 1)) ^ (a >> (w - 2))) & 1);
    return a;
}

/* ---------- string instructions ---------- */
#define SRC (segbase_[3] + esi)
#define DST (segbase_[0] + edi)
static inline uint32_t rdn(uint32_t a, int s) { return s == 1 ? RD8(a) : s == 2 ? RD16(a) : RD32(a); }
static inline void wrn(uint32_t a, uint32_t v, int s) { if (s == 1) WR8(a, v); else if (s == 2) WR16(a, v); else WR32(a, v); }
#define STEP(s) (g_df ? (uint32_t)-(s) : (uint32_t)(s))
void rt_movs(int s, int rep) {
    if (!rep) { wrn(DST, rdn(SRC, s), s); esi += STEP(s); edi += STEP(s); return; }
    if (!g_df && DST + (uint64_t)ecx * s <= GUEST_MEM_SIZE && SRC + (uint64_t)ecx * s <= GUEST_MEM_SIZE
        && (DST >= SRC + ecx * s || DST + ecx * s <= SRC)) {
        memcpy(M + DST, M + SRC, (size_t)ecx * s); esi += ecx * s; edi += ecx * s; ecx = 0; return;
    }
    while (ecx) { wrn(DST, rdn(SRC, s), s); esi += STEP(s); edi += STEP(s); ecx--; }
}
void rt_stos(int s, int rep) {
    uint32_t v = s == 1 ? eax & 0xFF : s == 2 ? eax & 0xFFFF : eax;
    if (!rep) { wrn(DST, v, s); edi += STEP(s); return; }
    if (s == 1 && !g_df && DST + (uint64_t)ecx <= GUEST_MEM_SIZE) { memset(M + DST, (int)v, ecx); edi += ecx; ecx = 0; return; }
    while (ecx) { wrn(DST, v, s); edi += STEP(s); ecx--; }
}
void rt_lods(int s, int rep) {
    do { uint32_t v = rdn(SRC, s); if (s == 1) SET8L(eax, v); else if (s == 2) SET16(eax, v); else eax = v;
         esi += STEP(s); } while (rep && --ecx);
}
static void cmpflags(uint32_t a, uint32_t b, int s) {
    int w = s * 8; uint32_t m = w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1); uint32_t r = (a - b) & m;
    g_cf = a < b; g_zf = r == 0; g_sf = (r >> (w - 1)) & 1; g_of = (((a ^ b) & (a ^ r)) >> (w - 1)) & 1;
}
void rt_scas(int s, int rep) {
    uint32_t v = s == 1 ? eax & 0xFF : s == 2 ? eax & 0xFFFF : eax;
    if (!rep) { cmpflags(v, rdn(DST, s), s); edi += STEP(s); return; }
    while (ecx) { cmpflags(v, rdn(DST, s), s); edi += STEP(s); ecx--;
        if (rep == 1 && !g_zf) break; if (rep == 2 && g_zf) break; }
}
void rt_cmps(int s, int rep) {
    if (!rep) { cmpflags(rdn(SRC, s), rdn(DST, s), s); esi += STEP(s); edi += STEP(s); return; }
    while (ecx) { cmpflags(rdn(SRC, s), rdn(DST, s), s); esi += STEP(s); edi += STEP(s); ecx--;
        if (rep == 1 && !g_zf) break; if (rep == 2 && g_zf) break; }
}

/* ---------- x87 ---------- */
void fpu_compare(long double a, long double b) {
    fsw &= ~0x4500u;                        /* C0 C2 C3 */
    if (a != a || b != b) fsw |= 0x4500u;
    else if (a < b) fsw |= 0x0100u;
    else if (a == b) fsw |= 0x4000u;
}
uint32_t fpu_status(void) { return (fsw & ~0x3800u) | ((uint32_t)(ftop & 7) << 11); }
/* The game sets the FPU to 53-bit (double) precision at start-up (fldcw 0x127F), so
   every x87 result is rounded to a double's precision.  The host computes the same
   sums in long double, which on x86 uses the x87 too: copy the guest's precision
   bits onto the host FPU so the results match the original exactly.  Without this,
   e.g. 3000000 * 0.001 came out a hair above 3000 and the contract screen's wage
   "+" button never moved. Rounding mode is left alone (fpu_round handles it). */
void fpu_setcw(void) {
#if (defined(__x86_64__) || defined(__i386__)) && (defined(__GNUC__) || defined(__clang__))
    if (sizeof(long double) > sizeof(double)) {
        unsigned short cw;
        __asm__ volatile ("fnstcw %0" : "=m"(cw));
        cw = (unsigned short)((cw & ~0x300u) | (fcw & 0x300u));
        __asm__ volatile ("fldcw %0" : : "m"(cw));
    }
#endif
}
int64_t fpu_round(long double v) {
    switch ((fcw >> 10) & 3) {
    case 0: return (int64_t)nearbyintl(v);     /* host default mode is round-to-nearest-even */
    case 1: return (int64_t)floorl(v);
    case 2: return (int64_t)ceill(v);
    default: return (int64_t)truncl(v);
    }
}
long double fpu_ld80(uint32_t a) {
    uint64_t mant; uint16_t se; memcpy(&mant, M + a, 8); memcpy(&se, M + a + 8, 2);
    int e = se & 0x7FFF; int sgn = se >> 15;
    if (e == 0 && mant == 0) return sgn ? -0.0L : 0.0L;
    long double v = ldexpl((long double)mant, e - 16383 - 63); return sgn ? -v : v;
}
void fpu_st80(uint32_t a, long double v) {
    uint16_t se = 0; uint64_t mant = 0;
    if (v != 0) { int e; long double f = frexpl(fabsl(v), &e); mant = (uint64_t)ldexpl(f, 64);
        se = (uint16_t)((e - 1 + 16383) & 0x7FFF); if (v < 0) se |= 0x8000; }
    memcpy(M + a, &mant, 8); memcpy(M + a + 8, &se, 2);
}

/* FSAVE/FRSTOR, 32-bit protected-mode layout: 28-byte environment + 8 x 80-bit registers */
void fpu_save(uint32_t a) {
    memset(M + a, 0, 28); WR16(a, fcw); WR16(a + 4, fpu_status()); WR16(a + 8, 0xFFFF);
    for (int i = 0; i < 8; i++) fpu_st80(a + 28 + 10 * i, ST(i));
    ftop = 0; fsw = 0; fcw = 0x37F; fpu_setcw();        /* FSAVE re-initialises the FPU */
}
void fpu_restore(uint32_t a) {
    fcw = RD16(a); fpu_setcw(); uint32_t sw = RD16(a + 4); ftop = (sw >> 11) & 7; fsw = sw & ~0x3800u;
    for (int i = 0; i < 8; i++) ST(i) = fpu_ld80(a + 28 + 10 * i);
}

/* Called every 16384 loop iterations of guest code: a deterministic point at which pending
   input can be delivered and the mouse handler run, as a hardware IRQ would in DOS. */
uint32_t g_backedge_ctr;
void host_async(void);
void rt_backedge(void) {
    extern unsigned long g_polls;
    g_backedge_ctr = 0;
    g_polls++;
    host_async();
}
