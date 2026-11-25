/* lift.c - x86-32 (objdump AT&T text) -> C, a line-for-line port of lift.py: the C it writes must be
   identical, byte for byte, to what lift.py writes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <setjmp.h>
#include "lift.h"

static const char *R32[] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi",0};
static const char *R16[] = {"ax","cx","dx","bx","sp","bp","si","di",0};
static const char *R8L[] = {"al","cl","dl","bl",0};
static const char *R8H[] = {"ah","ch","dh","bh",0};
static const char *SEGS[] = {"es","cs","ss","ds","fs","gs",0};

static int idx_of(const char **t, const char *s) { for (int i = 0; t[i]; i++) if (!strcmp(t[i], s)) return i; return -1; }

static jmp_buf unsup_jb;
static void unsup(void) { longjmp(unsup_jb, 1); }

/* ---- operands */
typedef struct { char **v; int n; } ops_t;
static ops_t split_ops(const char *s) {
    ops_t o = {0, 0}; int depth = 0; sb_t cur = {0};
    o.v = (char **)T("%*s", 8 * (int)sizeof(char *), "");      /* room for 8 pointers */
    for (const char *p = s; ; p++) {
        char ch = *p;
        if (ch == '(') depth++;
        if (ch == ')') depth--;
        if (!ch || (ch == ',' && depth == 0)) {
            /* strip */
            const char *a = cur.s ? cur.s : ""; size_t L = cur.n;
            while (L && isspace((unsigned char)a[0])) { a++; L--; }
            while (L && isspace((unsigned char)a[L - 1])) L--;
            if (ch || L) { if (o.n < 8) o.v[o.n++] = T("%.*s", (int)L, a); }
            cur.n = 0; if (cur.s) cur.s[0] = 0;
            if (!ch) break;
        } else sb_addn(&cur, &ch, 1);
    }
    sb_free(&cur);
    return o;
}
static int regsize(const char *o) {
    const char *r = o + 1;
    if (idx_of(R32, r) >= 0) return 32;
    if (idx_of(R16, r) >= 0) return 16;
    if (idx_of(R8L, r) >= 0 || idx_of(R8H, r) >= 0) return 8;
    return 0;
}
static int is_reg(const char *o) { return o[0] == '%' && !strchr(o, ':') && regsize(o); }
static int is_imm(const char *o) { return o[0] == '$'; }
static uint32_t imm(const char *o) {
    const char *t = o + 1; int neg = 0;
    if (*t == '-') { neg = 1; t++; }
    unsigned long long v = strtoull(t, 0, 16);
    return (uint32_t)(neg ? (0 - v) : v);
}
static int segaware;

static int is_word(char c) { return isalnum((unsigned char)c) || c == '_'; }

static char *ea(const char *o, int lea) {
    int seg = -1;
    if (o[0] == '%' && o[1] && o[2] && o[3] == ':') {
        char s2[3] = {o[1], o[2], 0}; int k = idx_of(SEGS, s2);
        if (k >= 0) { seg = k; o += 4; }
    }
    const char *p = o; int has_d = 0; long long disp = 0; char base[16] = "", index[16] = ""; int scale = 0, has_b = 0, has_i = 0, has_s = 0;
    if (*p == '-' || (p[0] == '0' && p[1] == 'x')) {
        int neg = 0; const char *q = p;
        if (*q == '-') { neg = 1; q++; }
        if (q[0] != '0' || q[1] != 'x') unsup();
        q += 2; const char *h = q;
        while ((*q >= '0' && *q <= '9') || (*q >= 'a' && *q <= 'f')) q++;
        if (q == h) unsup();
        disp = (long long)strtoull(h, 0, 16); if (neg) disp = -disp; has_d = 1; p = q;
    }
    if (*p == '(') {
        p++;
        if (*p == '%') { const char *q = ++p; while (is_word(*q)) q++; if (q == p) unsup(); snprintf(base, sizeof base, "%%%.*s", (int)(q - p), p); has_b = 1; p = q; }
        if (*p == ',') {
            p++; if (*p != '%') unsup();
            const char *q = ++p; while (is_word(*q)) q++; if (q == p) unsup();
            snprintf(index, sizeof index, "%%%.*s", (int)(q - p), p); has_i = 1; p = q;
            if (*p == ',') { p++; if (!isdigit((unsigned char)*p)) unsup(); scale = *p - '0'; has_s = 1; p++; }
        }
        if (*p != ')') unsup();
        p++;
    }
    if (*p) unsup();
    if (!has_d && !has_b && !has_i) unsup();
    sb_t e = {0}; int np = 0;
    sb_add(&e, "(uint32_t)(");
    if (has_d) { sb_printf(&e, "0x%xu", (unsigned)(uint32_t)disp); np++; }
    if (has_b && strcmp(base, "%eiz")) { if (np++) sb_add(&e, "+"); sb_add(&e, base + 1); }
    if (has_i && strcmp(index, "%eiz")) {
        int s = has_s ? scale : 1;
        if (np++) sb_add(&e, "+");
        sb_add(&e, index + 1); if (s != 1) sb_printf(&e, "*%du", s);
    }
    if (!np) sb_add(&e, "0u");
    sb_add(&e, ")");
    char *E = Tdup(e.s); sb_free(&e);
    if (lea) return E;
    if (seg < 0 && segaware && !(has_b && (!strcmp(base, "%esp") || !strcmp(base, "%ebp")))) seg = 3;
    if (seg == 0 || seg == 3 || seg == 4 || seg == 5) {
        static const int SEGIDX[] = {0, 1, 2, 3, 4, 5};
        return T("(uint32_t)(segbase_[%d]+%s)", SEGIDX[seg], E);
    }
    return E;
}

static char *rd(const char *o, int w) {
    if (is_imm(o)) return T("0x%xu", (unsigned)(imm(o) & (uint32_t)((1ull << w) - 1)));
    if (is_reg(o)) {
        const char *r = o + 1; int k;
        if ((k = idx_of(R32, r)) >= 0) return Tdup(r);
        if ((k = idx_of(R16, r)) >= 0) return T("(%s&0xFFFFu)", R32[k]);
        if ((k = idx_of(R8L, r)) >= 0) return T("(%s&0xFFu)", R32[k]);
        if ((k = idx_of(R8H, r)) >= 0) return T("((%s>>8)&0xFFu)", R32[k]);
    }
    return T("RD%d(%s)", w, ea(o, 0));
}
static char *wr(const char *o, int w, const char *v) {
    if (is_reg(o)) {
        const char *r = o + 1; int k;
        if ((k = idx_of(R32, r)) >= 0) return T("%s=(uint32_t)(%s);", r, v);
        if ((k = idx_of(R16, r)) >= 0) return T("SET16(%s,%s);", R32[k], v);
        if ((k = idx_of(R8L, r)) >= 0) return T("SET8L(%s,%s);", R32[k], v);
        if ((k = idx_of(R8H, r)) >= 0) return T("SET8H(%s,%s);", R32[k], v);
    }
    if (is_imm(o)) unsup();
    return T("WR%d(%s,%s);", w, ea(o, 0), v);
}

static const char *COND(const char *c) {
    static const char *t[][2] = {{"e","zf"},{"z","zf"},{"ne","!zf"},{"nz","!zf"},{"l","(sf!=of)"},{"nge","(sf!=of)"},{"ge","(sf==of)"},
        {"nl","(sf==of)"},{"le","(zf||sf!=of)"},{"ng","(zf||sf!=of)"},{"g","(!zf&&sf==of)"},{"nle","(!zf&&sf==of)"},
        {"b","cf"},{"c","cf"},{"nae","cf"},{"ae","!cf"},{"nb","!cf"},{"nc","!cf"},{"be","(cf||zf)"},{"na","(cf||zf)"},
        {"a","(!cf&&!zf)"},{"nbe","(!cf&&!zf)"},{"s","sf"},{"ns","!sf"},{"o","of"},{"no","!of"},{0,0}};
    for (int i = 0; t[i][0]; i++) if (!strcmp(t[i][0], c)) return t[i][1];
    return 0;
}
static int SUF(char c) { return c == 'b' ? 8 : c == 'w' ? 16 : c == 'l' ? 32 : 0; }
static const char *ALU[] = {"add","adc","sub","sbb","cmp","and","or","xor","test","inc","dec","neg","not","mov","shl","sal",
    "shr","sar","rol","ror","rcl","rcr","imul","mul","idiv","div","push","pop","lea","xchg",0};
static void base_size(const char *mn, char *base, int *forced) {
    *forced = 0; strcpy(base, mn);
    if (idx_of(ALU, mn) >= 0) return;
    size_t L = strlen(mn);
    if (L && SUF(mn[L - 1])) { char b[32]; snprintf(b, sizeof b, "%.*s", (int)(L - 1), mn); if (idx_of(ALU, b) >= 0) { strcpy(base, b); *forced = SUF(mn[L - 1]); } }
}
static int opsize(ops_t *o, int forced) {
    if (forced) return forced;
    for (int i = o->n - 1; i >= 0; i--) if (is_reg(o->v[i])) return regsize(o->v[i]);
    return 32;
}
static const char *MSK(int w) { return w == 8 ? "0xFFu" : w == 16 ? "0xFFFFu" : "0xFFFFFFFFu"; }

/* ---- function state */
typedef struct { prog_t *P; int fid; uint32_t start, cur; } Fn;
static int in_body(Fn *s, uint32_t t) { return t >= s->P->base && t < s->P->base + s->P->size && s->P->body[t - s->P->base] == s->fid; }
static int in_funcs(Fn *s, uint32_t t) { return t >= s->P->base && t < s->P->base + s->P->size && s->P->isfunc[t - s->P->base]; }
static void add_label(Fn *s, uint32_t t) { s->P->label[t - s->P->base] = s->fid; }

const char *g_fpfx = "f_";              /* function names: f_XXXXXX (the game) or e_XXXXXX (the editor) */
static char *target(Fn *s, uint32_t t) {
    if (in_body(s, t)) {
        add_label(s, t);
        if (t <= s->cur) return T("BACKEDGE; goto L_%x;", t);
        return T("goto L_%x;", t);
    }
    if (in_funcs(s, t)) return T("SPILL; %s%06x(); return;", g_fpfx, t);
    return T("SPILL; rt_call(0x%xu); return;", t);
}
static char *call(Fn *s, uint32_t t, uint32_t nxt) {
    char *fn = in_funcs(s, t) ? T("%s%06x()", g_fpfx, t) : T("rt_call(0x%xu)", t);
    return T("PUSH32(0x%xu); SPILL; %s; RELOAD; if(g_ret!=0x%xu) return;", nxt, fn, nxt);
}
static char *shift_count(const char *op) {
    if (!strcmp(op, "%cl")) return "(ecx&0x1Fu)";
    return T("0x%xu", imm(op) & 31);
}
static int is_hexaddr(const char *o) {    /* re.fullmatch(r'0x[0-9a-f]+', o) */
    if (o[0] != '0' || o[1] != 'x' || !o[2]) return 0;
    for (const char *p = o + 2; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0;
    return 1;
}
static int st_of(const char *o) {         /* %st or %st(N) -> N, else -1 */
    if (strncmp(o, "%st", 3)) return -1;
    if (!o[3]) return 0;
    if (o[3] == '(' && isdigit((unsigned char)o[4]) && o[5] == ')' && !o[6]) return o[4] - '0';
    return -1;
}
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static char *bitops(Fn *s, const char *mn, ops_t *O) {
    char b[16];
    if (!strcmp(mn, "bsr") || !strcmp(mn, "bsf")) strcpy(b, mn);
    else { strcpy(b, mn); size_t L = strlen(b); while (L && b[L - 1] == 'l') b[--L] = 0; }
    char **ops = O->v;
    if (!strcmp(b, "bt")) {
        int w = opsize(O, !strcmp(mn, "btl") ? 32 : 0);
        return T("cf=(%s>>(%s&%du))&1u;", rd(ops[1], w), rd(ops[0], w), w - 1);
    }
    if (!strcmp(b, "bsr") || !strcmp(b, "bsf")) {
        int w = opsize(O, 0); const char *f = !strcmp(b, "bsr") ? "31-__builtin_clz(v_)" : "__builtin_ctz(v_)";
        return T("{ uint32_t v_=%s; zf=(v_==0); if(v_) { %s } }", rd(ops[0], w), wr(ops[1], w, f));
    }
    char *cnt = shift_count(ops[0]); const char *src = ops[1], *dst = ops[2];
    const char *core = !strcmp(b, "shld") ? "r_=(d_<<n_)|(s_>>(32-n_)); cf=(d_>>(32-n_))&1u;"
                                          : "r_=(d_>>n_)|(s_<<(32-n_)); cf=(d_>>(n_-1))&1u;";
    (void)s;
    return T("{ uint32_t d_=%s,s_=%s,n_=%s,r_; if(n_){ %s SZF(32,r_); %s } }", rd(dst, 32), rd(src, 32), cnt, core, wr(dst, 32, "r_"));
}

static char *fmt2(const char *f, const char *d, const char *s) {  /* '{d}' / '{s}' substitution */
    sb_t b = {0};
    for (const char *p = f; *p; ) {
        if (!strncmp(p, "{d}", 3)) { sb_add(&b, d); p += 3; }
        else if (!strncmp(p, "{s}", 3)) { sb_add(&b, s); p += 3; }
        else sb_addn(&b, p++, 1);
    }
    char *r = Tdup(b.s ? b.s : ""); sb_free(&b); return r;
}

static char *fpu(Fn *s, const char *mn, ops_t *O) {
    char **ops = O->v; int nops = O->n; (void)s;
#define MEMW(c) ((c) == 's' ? "F32" : "F64")
    size_t L = strlen(mn); char last = L ? mn[L - 1] : 0;
    if (!strcmp(mn, "flds") || !strcmp(mn, "fldl")) return T("FPUSH((long double)RD%s(%s));", MEMW(last), ea(ops[0], 0));
    if (!strcmp(mn, "fldt")) return T("FPUSH(fpu_ld80(%s));", ea(ops[0], 0));
    if (!strcmp(mn, "fld")) return T("{ long double v_=ST(%d); FPUSH(v_); }", st_of(ops[0]));
    if (!strcmp(mn, "fildll")) return T("{ uint64_t v_; memcpy(&v_,M+%s,8); FPUSH((long double)(int64_t)v_); }", ea(ops[0], 0));
    if (!strcmp(mn, "filds") || !strcmp(mn, "fildl"))
        return T("FPUSH((long double)(%s(%s)));", !strcmp(mn, "filds") ? "(int16_t)RD16" : "(int32_t)RD32", ea(ops[0], 0));
    static const char *consts[][2] = {{"fldz","0.0L"},{"fld1","1.0L"},{"fldlg2","0.301029995663981195214L"},{"fldpi","3.14159265358979323846L"},
        {"fldln2","0.693147180559945309417L"},{"fldl2e","1.44269504088896340736L"},{0,0}};
    for (int i = 0; consts[i][0]; i++) if (!strcmp(mn, consts[i][0])) return T("FPUSH(%s);", consts[i][1]);
    if (!strcmp(mn, "fsts") || !strcmp(mn, "fstl") || !strcmp(mn, "fstps") || !strcmp(mn, "fstpl")) {
        const char *w = MEMW(last); const char *ty = last == 's' ? "float" : "double";
        return T("WR%s(%s,(%s)ST(0));%s", w, ea(ops[0], 0), ty, strchr(mn, 'p') ? " FPOP();" : "");
    }
    if (!strcmp(mn, "fstpt")) return T("fpu_st80(%s,ST(0)); FPOP();", ea(ops[0], 0));
    if (!strcmp(mn, "fst") || !strcmp(mn, "fstp")) return T("ST(%d)=ST(0);%s", st_of(ops[0]), !strcmp(mn, "fstp") ? " FPOP();" : "");
    if (!strcmp(mn, "fistpll")) return T("{ int64_t v_=fpu_round(ST(0)); memcpy(M+%s,&v_,8); FPOP(); }", ea(ops[0], 0));
    if (!strcmp(mn, "fists") || !strcmp(mn, "fistl") || !strcmp(mn, "fistps") || !strcmp(mn, "fistpl")) {
        int w = last == 's' ? 16 : 32;
        return T("{ int64_t v_=fpu_round(ST(0)); WR%d(%s,(uint32_t)(int%d_t)v_); }%s", w, ea(ops[0], 0), w, strchr(mn, 'p') ? " FPOP();" : "");
    }
    /* f(add|sub|subr|mul|div|divr)(p?)([sl]?) */
    static const char *AO[] = {"add","sub","subr","mul","div","divr",0};
    static const char *FWD[] = {"{d}={d}+{s};","{d}={d}-{s};","{d}={s}-{d};","{d}={d}*{s};","{d}={d}/{s};","{d}={s}/{d};"};
    if (mn[0] == 'f') for (int k = 0; AO[k]; k++) {
        size_t n = strlen(AO[k]);
        if (strncmp(mn + 1, AO[k], n)) continue;
        const char *r = mn + 1 + n; int p = 0; char sz = 0;
        if (*r == 'p') { p = 1; r++; }
        if (*r == 's' || *r == 'l') { sz = *r; r++; }
        if (*r) continue;
        if (sz) return fmt2(FWD[k], "ST(0)", T("(long double)RD%s(%s)", MEMW(sz), ea(ops[0], 0)));
        if (nops == 1 && !p) return fmt2(FWD[k], "ST(0)", T("ST(%d)", st_of(ops[0])));
        if (nops == 2 && st_of(ops[1]) == 0 && !p) return fmt2(FWD[k], "ST(0)", T("ST(%d)", st_of(ops[0])));
        int j = nops == 2 ? st_of(ops[1]) : nops == 1 ? st_of(ops[0]) : 1;
        static const int REV[] = {0, 2, 1, 3, 5, 4};
        return T("%s%s", fmt2(FWD[REV[k]], T("ST(%d)", j), "ST(0)"), p ? " FPOP();" : "");
    }
    /* fi(add|...)([sl]) */
    if (!strncmp(mn, "fi", 2)) for (int k = 0; AO[k]; k++) {
        size_t n = strlen(AO[k]);
        if (strncmp(mn + 2, AO[k], n)) continue;
        const char *r = mn + 2 + n;
        if (!((r[0] == 's' || r[0] == 'l') && !r[1])) continue;
        const char *t = r[0] == 's' ? "(int16_t)RD16" : "(int32_t)RD32";
        char *src = T("(long double)(%s(%s))", t, ea(ops[0], 0));
        static const char *F[] = {"ST(0)+=%s;","ST(0)-=%s;","ST(0)=%s-ST(0);","ST(0)*=%s;","ST(0)/=%s;","ST(0)=%s/ST(0);"};
        return T(F[k], src);
    }
    /* f(u?)com(p{0,2})([sl]?) */
    {
        const char *r = mn + 1;
        if (mn[0] == 'f') {
            if (*r == 'u') r++;
            if (!strncmp(r, "com", 3)) {
                r += 3; int p = 0; while (*r == 'p' && p < 2) { p++; r++; }
                char sz = 0; if (*r == 's' || *r == 'l') { sz = *r; r++; }
                if (!*r) {
                    char *src;
                    if (sz) src = T("(long double)RD%s(%s)", MEMW(sz), ea(ops[0], 0));
                    else if (nops) src = T("ST(%d)", st_of(ops[0]));
                    else src = "ST(1)";
                    return T("fpu_compare(ST(0),%s);%s", src, p == 0 ? "" : p == 1 ? " FPOP();" : " FPOP(); FPOP();");
                }
            }
        }
    }
    if (!strncmp(mn, "ficom", 5)) {
        const char *r = mn + 5; int p = 0; if (*r == 'p') { p = 1; r++; }
        if ((r[0] == 's' || r[0] == 'l') && !r[1]) {
            const char *t = r[0] == 's' ? "(int16_t)RD16" : "(int32_t)RD32";
            return T("fpu_compare(ST(0),(long double)(%s(%s)));%s", t, ea(ops[0], 0), p ? " FPOP();" : "");
        }
    }
    if (!strcmp(mn, "ftst")) return "fpu_compare(ST(0),0.0L);";
    if (!strcmp(mn, "fnstsw") || !strcmp(mn, "fstsw")) {
        if (!nops || !strcmp(ops[0], "%ax")) return "SET16(eax,fpu_status());";
        return T("WR16(%s,fpu_status());", ea(ops[0], 0));
    }
    if (!strcmp(mn, "fnstcw") || !strcmp(mn, "fstcw")) return T("WR16(%s,fcw);", ea(ops[0], 0));
    if (!strcmp(mn, "fldcw")) return T("fcw=RD16(%s); fpu_setcw();", ea(ops[0], 0));
    if (!strcmp(mn, "finit") || !strcmp(mn, "fninit")) return "ftop=0; fsw=0; fcw=0x37Fu; fpu_setcw();";
    if (!strcmp(mn, "fnclex") || !strcmp(mn, "fclex")) return "fsw&=0x7F00u;";
    if (!strcmp(mn, "fsave") || !strcmp(mn, "fnsave")) return T("fpu_save(%s);", ea(ops[0], 0));
    if (!strcmp(mn, "frstor")) return T("fpu_restore(%s);", ea(ops[0], 0));
    static const char *simple[][2] = {{"fchs","ST(0)=-ST(0);"},{"fabs","ST(0)=fabsl(ST(0));"},{"fsqrt","ST(0)=sqrtl(ST(0));"},
        {"frndint","ST(0)=(long double)fpu_round(ST(0));"},{"fyl2x","{ long double x_=ST(0); FPOP(); ST(0)=ST(0)*log2l(x_); }"},
        {"fincstp","ftop=(ftop+1)&7;"},{"fdecstp","ftop=(ftop-1)&7;"},{"fwait",";"},{"fnop",";"},{0,0}};
    for (int i = 0; simple[i][0]; i++) if (!strcmp(mn, simple[i][0])) return (char *)simple[i][1];
    if (!strcmp(mn, "fxch")) { int i = nops ? st_of(ops[0]) : 1; return T("{ long double t_=ST(0); ST(0)=ST(%d); ST(%d)=t_; }", i, i); }
    unsup(); return 0;
}

static int in_list(const char *s, const char **l) { return idx_of(l, s) >= 0; }

static char *lift1(Fn *s, uint32_t a) {
    ins_t *I = s->P->ins[a - s->P->base];
    int n = I->n; uint32_t nxt = a + n; s->cur = a;
    static const char *PFX[] = {"es","cs","ss","ds","fs","gs","notrack","bnd","data16","addr16","rep","repz","repe","repnz","repne","lock",0};
    /* allt = (mn + ' ' + opstr).split() */
    char *all = T("%s %s", I->mn, I->ops);
    char *toks_all[32]; int nt = 0;
    for (char *p = strtok(all, " \t"); p && nt < 32; p = strtok(0, " \t")) toks_all[nt++] = p;
    int k_ = 0; while (k_ < nt && in_list(toks_all[k_], PFX)) k_++;
    char *mn, *opstr;
    if (k_ < nt) {
        sb_t m = {0}, o = {0};
        for (int i = 0; i <= k_; i++) { if (i) sb_add(&m, " "); sb_add(&m, toks_all[i]); }
        for (int i = k_ + 1; i < nt; i++) { if (i > k_ + 1) sb_add(&o, " "); sb_add(&o, toks_all[i]); }
        mn = Tdup(m.s ? m.s : ""); opstr = Tdup(o.s ? o.s : ""); sb_free(&m); sb_free(&o);
    } else { mn = Tdup(I->mn); opstr = Tdup(I->ops); }
    ops_t O = split_ops(opstr); char **ops = O.v;
    /* toks = mn.split() */
    char *mt = Tdup(mn); char *toks[16]; int ntk = 0;
    for (char *p = strtok(mt, " \t"); p && ntk < 16; p = strtok(0, " \t")) toks[ntk++] = p;
    int ti = 0, rep = 0, force16 = 0;
    while (ti < ntk && (in_list(toks[ti], SEGS) || !strcmp(toks[ti], "notrack") || !strcmp(toks[ti], "bnd"))) ti++;
    while (ti < ntk && (!strcmp(toks[ti], "data16") || !strcmp(toks[ti], "addr16"))) { force16 = force16 || !strcmp(toks[ti], "data16"); ti++; }
    if (ti < ntk && (!strcmp(toks[ti], "rep") || !strcmp(toks[ti], "repz") || !strcmp(toks[ti], "repe"))) { rep = 1; ti++; }
    else if (ti < ntk && (!strcmp(toks[ti], "repnz") || !strcmp(toks[ti], "repne"))) { rep = 2; ti++; }
    mn = ti < ntk ? toks[ti] : "";

    /* string instructions */
    {
        static const char *SK[] = {"movs","stos","lods","scas","cmps",0};
        for (int k = 0; SK[k]; k++) if (!strncmp(mn, SK[k], 4) && (!mn[4] || (SUF(mn[4]) && !mn[5]))) {
            int w = mn[4] ? SUF(mn[4]) : 0;
            if (!w) { w = 32; for (int i = 0; i < O.n; i++) if (is_reg(ops[i])) { w = regsize(ops[i]); break; } }
            if (k >= 3) return T("SPILL; rt_%s(%d,%d); RELOAD;", SK[k], w / 8, rep);
            return T("rt_%s(%d,%d);", SK[k], w / 8, rep);
        }
    }
    if (!strcmp(mn, "ret") || !strcmp(mn, "retl")) {
        uint32_t extra = O.n ? imm(ops[0]) : 0;
        return T("{ uint32_t ra_=POP32(); esp+=%uu; SPILL; if(!rt_is_retaddr(ra_)) rt_call(ra_); else g_ret=ra_; return; }", extra);
    }
    if (!strcmp(mn, "lretw")) { uint32_t extra = O.n ? imm(ops[0]) : 0; return T("g_ret=POP16(); esp+=%uu; SPILL; return;", 2 + extra); }
    if (!strcmp(mn, "lret") || !strcmp(mn, "lretl")) { uint32_t extra = O.n ? imm(ops[0]) : 0; return T("g_ret=POP32(); esp+=%uu; SPILL; return;", 4 + extra); }
    if (!strcmp(mn, "iret") || !strcmp(mn, "iretl")) return "g_ret=POP32(); esp+=8u; SPILL; return;";
    if (!strcmp(mn, "jmp")) {
        const char *o = ops[0];
        if (is_hexaddr(o)) return target(s, (uint32_t)strtoul(o, 0, 16));
        char *t = rd(o + 1, 32);
        prog_t *P = s->P;
        if (P->has_jt[a - P->base]) {
            uint32_t tb = P->jt_tab[a - P->base], cnt = P->jt_cnt[a - P->base];
            char **cases = (char **)T("%*s", (int)((cnt + 1) * sizeof(char *)), ""); int nc = 0;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t tt; prog_fix(P, tb + 4 * k, &tt);
                if (in_body(s, tt)) { add_label(s, tt); char *c = T("case 0x%xu: goto L_%x;", tt, tt); int dup = 0; for (int q = 0; q < nc; q++) if (!strcmp(cases[q], c)) dup = 1; if (!dup) cases[nc++] = c; }
            }
            qsort(cases, nc, sizeof *cases, cmpstr);
            sb_t j = {0}; for (int q = 0; q < nc; q++) { if (q) sb_add(&j, " "); sb_add(&j, cases[q]); }
            char *r = T("{ uint32_t _t=%s; switch(_t){ %s default: SPILL; rt_call(_t); return; } }", t, j.s ? j.s : ""); sb_free(&j);
            return r;
        }
        return T("{ uint32_t _t=%s; SPILL; rt_call(_t); return; }", t);
    }
    if (!strcmp(mn, "call")) {
        const char *o = ops[0];
        if (is_hexaddr(o)) return call(s, (uint32_t)strtoul(o, 0, 16), nxt);
        return T("{ uint32_t _t=%s; PUSH32(0x%xu); SPILL; rt_call(_t); RELOAD; if(g_ret!=0x%xu) return; }", rd(o + 1, 32), nxt, nxt);
    }
    if (mn[0] == 'j' && mn[1] && COND(mn + 1)) return T("if(%s) { %s }", COND(mn + 1), target(s, (uint32_t)strtoul(ops[0], 0, 16)));
    if (!strcmp(mn, "jecxz") || !strcmp(mn, "jcxz")) return T("if(ecx==0) { %s }", target(s, (uint32_t)strtoul(ops[0], 0, 16)));
    if (!strcmp(mn, "loop")) return T("if(--ecx) { %s }", target(s, (uint32_t)strtoul(ops[0], 0, 16)));
    if (!strcmp(mn, "loope") || !strcmp(mn, "loopz")) return T("if(--ecx && zf) { %s }", target(s, (uint32_t)strtoul(ops[0], 0, 16)));
    if (!strcmp(mn, "loopne") || !strcmp(mn, "loopnz")) return T("if(--ecx && !zf) { %s }", target(s, (uint32_t)strtoul(ops[0], 0, 16)));
    if (!strncmp(mn, "set", 3) && mn[3] && COND(mn + 3)) return wr(ops[0], 8, T("(%s)?1u:0u", COND(mn + 3)));
    if (mn[0] == 'f') return fpu(s, mn, &O);
    {
        static const char *simple[][2] = {{"nop",";"},{"fwait",";"},{"cli",";"},{"sti",";"},{"cld","g_df=0;"},{"std","g_df=1;"},{"clc","cf=0;"},
            {"stc","cf=1;"},{"cmc","cf^=1;"},{0,0}};
        for (int i = 0; simple[i][0]; i++) if (!strcmp(mn, simple[i][0])) return (char *)simple[i][1];
        if (!strcmp(mn, "hlt")) return T("rt_unimpl(0x%xu,\"hlt\");", a);
        if (!strcmp(mn, "int3")) return T("rt_unimpl(0x%xu,\"int3\");", a);
    }
    if (!strcmp(mn, "int")) return T("SPILL; rt_int(0x%xu); RELOAD;", imm(ops[0]));
    if (!strcmp(mn, "in") || !strcmp(mn, "inb") || !strcmp(mn, "inw") || !strcmp(mn, "inl")) {
        char *port = !strcmp(ops[0], "(%dx)") ? "(edx&0xFFFFu)" : T("0x%xu", imm(ops[0]));
        int w = regsize(ops[1]); return wr(ops[1], w, T("rt_in(%s,%d)", port, w));
    }
    if (!strcmp(mn, "out") || !strcmp(mn, "outb") || !strcmp(mn, "outw") || !strcmp(mn, "outl")) {
        char *port = !strcmp(ops[1], "(%dx)") ? "(edx&0xFFFFu)" : T("0x%xu", imm(ops[1]));
        int w = regsize(ops[0]); return T("rt_out(%s,%s,%d);", port, rd(ops[0], w), w);
    }
    {
        static const char *SYS[] = {"lar","lsl","verr","verw","sidtl","sgdtl","sidt","sgdt","str","sldt","arpl","smsw",
            "lidtl","lgdtl","lidt","lgdt","lmsw","invlpg","lldt","ltr","clts","wbinvd",0};
        if (in_list(mn, SYS) || strstr(opstr, "%cr") || strstr(opstr, "%dr")) return T("SPILL; rt_sys(0x%xu,\"%s\"); RELOAD;", a, mn);
    }
    if (!strcmp(mn, "cwtl") || !strcmp(mn, "cwde")) return "eax=(uint32_t)(int32_t)(int16_t)(eax&0xFFFF);";
    if (!strcmp(mn, "cbtw") || !strcmp(mn, "cbw")) return "SET16(eax,(uint32_t)(int16_t)(int8_t)(eax&0xFF));";
    if (!strcmp(mn, "cltd") || !strcmp(mn, "cdq")) return "edx=((int32_t)eax<0)?0xFFFFFFFFu:0u;";
    if (!strcmp(mn, "cwtd") || !strcmp(mn, "cwd")) return "SET16(edx,((int16_t)(eax&0xFFFF)<0)?0xFFFFu:0u);";
    if (!strcmp(mn, "sahf")) return "sf=(eax>>15)&1; zf=(eax>>14)&1; cf=(eax>>8)&1;";
    if (!strcmp(mn, "lahf")) return "SET8H(eax,(sf<<7)|(zf<<6)|2u|cf);";
    if (!strcmp(mn, "pushf") || !strcmp(mn, "pushfl")) return "SPILL; PUSH32(rt_eflags());";
    if (!strcmp(mn, "pushfw")) return "SPILL; PUSH16(rt_eflags());";
    if (!strcmp(mn, "popf") || !strcmp(mn, "popfl")) return "rt_set_eflags(POP32()); RELOAD;";
    if (!strcmp(mn, "leave")) return "esp=ebp; ebp=POP32();";
    if (!strcmp(mn, "pusha") || !strcmp(mn, "pushal"))
        return "{ uint32_t s_=esp; PUSH32(eax); PUSH32(ecx); PUSH32(edx); PUSH32(ebx); PUSH32(s_); PUSH32(ebp); PUSH32(esi); PUSH32(edi); }";
    if (!strcmp(mn, "popa") || !strcmp(mn, "popal"))
        return "edi=POP32(); esi=POP32(); ebp=POP32(); esp+=4; ebx=POP32(); edx=POP32(); ecx=POP32(); eax=POP32();";
    if (!strcmp(mn, "enter")) {
        uint32_t lvl = imm(ops[1]) & 31;
        if (lvl == 0) return T("PUSH32(ebp); ebp=esp; esp-=0x%xu;", imm(ops[0]));
        return T("{ PUSH32(ebp); uint32_t fr_=esp; for(int i_=1;i_<%u;i_++){ ebp-=4; PUSH32(RD32(ebp)); } PUSH32(fr_); ebp=fr_; esp-=0x%xu; }", lvl, imm(ops[0]));
    }
    if (!strcmp(mn, "aam")) {
        uint32_t b = O.n ? imm(ops[0]) : 10;
        return T("{ uint32_t al_=eax&0xFFu; SET8H(eax,al_/%uu); SET8L(eax,al_%%%uu); SZF(8,eax&0xFFu); }", b, b);
    }
    if (!strcmp(mn, "les") || !strcmp(mn, "lds") || !strcmp(mn, "lfs") || !strcmp(mn, "lgs") || !strcmp(mn, "lss")) {
        char *e = ea(ops[0], 0);
        return T("{ uint32_t a_=%s; uint32_t o_=RD32(a_), s_=RD16(a_+4); SPILL; rt_set_seg(%d,s_); RELOAD; %s }", e, idx_of(SEGS, mn + 1), wr(ops[1], 32, "o_"));
    }
    if (!strcmp(mn, "movsbl") || !strcmp(mn, "movsbw") || !strcmp(mn, "movswl") || !strcmp(mn, "movzbl") || !strcmp(mn, "movzbw") || !strcmp(mn, "movzwl")) {
        int sw = mn[4] == 'b' ? 8 : 16, dw = mn[5] == 'w' ? 16 : 32;
        char *v = rd(ops[0], sw);
        if (mn[3] == 's') v = T("(uint32_t)(int32_t)(int%d_t)%s", sw, v);
        return wr(ops[1], dw, v);
    }
    {
        static const char *BO[] = {"bsr","bsf","bt","btl","shld","shrd","shldl","shrdl",0};
        if (in_list(mn, BO)) return bitops(s, mn, &O);
    }
    char base[32]; int forced; base_size(mn, base, &forced);
    if (force16 && !forced) forced = 16;
    int w = opsize(&O, forced); const char *M_ = MSK(w);
#define B(x) (!strcmp(base, x))
    static const char *SEGREGS[] = {"%es","%cs","%ss","%ds","%fs","%gs",0};
    if (B("mov")) {
        int k;
        if ((k = idx_of(SEGREGS, ops[0])) >= 0) return wr(ops[1], !is_reg(ops[1]) ? 16 : w, T("sel_[%d]", k));
        if ((k = idx_of(SEGREGS, ops[1])) >= 0)
            return T("SPILL; rt_set_seg(%d,%s&0xFFFFu); RELOAD;", k, rd(ops[0], !is_reg(ops[0]) ? 16 : regsize(ops[0])));
        return wr(ops[1], w, rd(ops[0], w));
    }
    if (B("lea")) return wr(ops[1], 32, ea(ops[0], 1));
    if (B("push")) {
        int k;
        if ((k = idx_of(SEGREGS, ops[0])) >= 0) return T("PUSH32(sel_[%d]);", k);
        if (is_imm(ops[0])) return T("PUSH32(0x%xu);", imm(ops[0]));
        return w == 16 ? T("PUSH16(%s);", rd(ops[0], 16)) : T("PUSH32(%s);", rd(ops[0], 32));
    }
    if (B("pop")) {
        int k;
        if ((k = idx_of(SEGREGS, ops[0])) >= 0) return T("SPILL; rt_set_seg(%d,POP32()&0xFFFFu); RELOAD;", k);
        return wr(ops[0], w, w == 16 ? "POP16()" : "POP32()");
    }
    if (B("xchg")) return T("{ uint32_t _a=%s,_b=%s; %s %s }", rd(ops[0], w), rd(ops[1], w), wr(ops[0], w, "_b"), wr(ops[1], w, "_a"));
    if (B("add") || B("adc") || B("sub") || B("sbb") || B("cmp") || B("and") || B("or") || B("xor") || B("test")) {
        if (O.n != 2) unsup();
        const char *src = ops[0], *dst = ops[1];
        sb_t b = {0};
        sb_printf(&b, "{ uint32_t a_=%s,b_=%s,r_; ", rd(dst, w), rd(src, w));
        if (B("add")) sb_printf(&b, "r_=(a_+b_)&%s; cf=r_<a_; of=MSB(%d,(~(a_^b_))&(a_^r_)); ", M_, w);
        else if (B("adc")) sb_printf(&b, "uint64_t t_=(uint64_t)a_+b_+cf; r_=(uint32_t)t_&%s; cf=(uint32_t)(t_>>%d)&1u; of=MSB(%d,(~(a_^b_))&(a_^r_)); ", M_, w, w);
        else if (B("sub") || B("cmp")) sb_printf(&b, "r_=(a_-b_)&%s; cf=a_<b_; of=MSB(%d,(a_^b_)&(a_^r_)); ", M_, w);
        else if (B("sbb")) sb_printf(&b, "uint64_t t_=(uint64_t)b_+cf; r_=(uint32_t)((uint64_t)a_-t_)&%s; cf=(uint64_t)a_<t_; of=MSB(%d,(a_^b_)&(a_^r_)); ", M_, w);
        else { const char *op = B("and") ? "&" : B("or") ? "|" : B("xor") ? "^" : "&"; sb_printf(&b, "r_=(a_%sb_)&%s; cf=0; of=0; ", op, M_); }
        sb_printf(&b, "SZF(%d,r_); ", w);
        if (!B("cmp") && !B("test")) sb_add(&b, wr(dst, w, "r_"));
        sb_add(&b, " }");
        char *r = Tdup(b.s); sb_free(&b); return r;
    }
    if (B("inc") || B("dec")) {
        const char *o = ops[0]; const char *op = B("inc") ? "+" : "-";
        char *ofc = B("inc") ? T("(r_==(1u<<%d))", w - 1) : T("(a_==(1u<<%d))", w - 1);
        return T("{ uint32_t a_=%s,r_=(a_%s1u)&%s; of=%s; SZF(%d,r_); %s }", rd(o, w), op, M_, ofc, w, wr(o, w, "r_"));
    }
    if (B("neg")) { const char *o = ops[0];
        return T("{ uint32_t a_=%s,r_=(0u-a_)&%s; cf=(a_!=0); of=(a_==(1u<<%d)); SZF(%d,r_); %s }", rd(o, w), M_, w - 1, w, wr(o, w, "r_")); }
    if (B("not")) { const char *o = ops[0]; return wr(o, w, T("(~%s)&%s", rd(o, w), M_)); }
    if (B("shl") || B("sal") || B("shr") || B("sar")) {
        const char *cnt, *o;
        if (O.n == 1) { cnt = "1u"; o = ops[0]; } else { cnt = shift_count(ops[0]); o = ops[1]; }
        char *core;
        if (B("shl") || B("sal")) core = T("uint64_t t_=(uint64_t)a_<<n_; r_=(uint32_t)t_&%s; cf=(uint32_t)(t_>>%d)&1u; of=MSB(%d,r_)^cf;", M_, w, w);
        else if (B("shr")) core = T("r_=a_>>n_; cf=(a_>>(n_-1))&1u; of=MSB(%d,a_);", w);
        else core = T("int32_t s_=(int32_t)(a_<<(32-%d))>>(32-%d); uint32_t k_=n_>31?31:n_; r_=(uint32_t)(s_>>k_)&%s; cf=(uint32_t)(s_>>(k_-1))&1u; of=0;", w, w, M_);
        return T("{ uint32_t a_=%s,n_=%s,r_; if(n_){ %s SZF(%d,r_); %s } }", rd(o, w), cnt, core, w, wr(o, w, "r_"));
    }
    if (B("rol") || B("ror") || B("rcl") || B("rcr")) {
        const char *cnt, *o;
        if (O.n == 1) { cnt = "1u"; o = ops[0]; } else { cnt = shift_count(ops[0]); o = ops[1]; }
        int k = B("rol") ? 0 : B("ror") ? 1 : B("rcl") ? 2 : 3;
        return T("{ SPILL; uint32_t r_=rt_rot(%d,%d,%s,%s); RELOAD; %s }", k, w, rd(o, w), cnt, wr(o, w, "r_"));
    }
    if (B("imul")) {
        if (O.n == 1) {
            const char *o = ops[0];
            if (w == 32) return T("{ int64_t p_=(int64_t)(int32_t)eax*(int32_t)%s; eax=(uint32_t)p_; edx=(uint32_t)((uint64_t)p_>>32); cf=of=(p_!=(int32_t)p_); }", rd(o, 32));
            if (w == 16) return T("{ int32_t p_=(int16_t)(eax&0xFFFF)*(int16_t)%s; SET16(eax,(uint32_t)p_); SET16(edx,(uint32_t)p_>>16); cf=of=(p_!=(int16_t)p_); }", rd(o, 16));
            return T("{ int32_t p_=(int8_t)(eax&0xFF)*(int8_t)%s; SET16(eax,(uint32_t)p_); cf=of=(p_!=(int8_t)p_); }", rd(o, 8));
        }
        const char *dst; char *a_, *b_;
        if (O.n == 2) { dst = ops[1]; a_ = rd(dst, w); b_ = rd(ops[0], w); }
        else { dst = ops[2]; a_ = rd(ops[1], w); b_ = rd(ops[0], w); }
        char *st = T("int%d_t", w);
        return T("{ int64_t p_=(int64_t)(%s)%s*(int64_t)(%s)%s; cf=of=(p_!=(%s)p_); %s }", st, a_, st, b_, st, wr(dst, w, "(uint32_t)p_"));
    }
    if (B("mul")) {
        const char *o = ops[0];
        if (w == 32) return T("{ uint64_t p_=(uint64_t)eax*%s; eax=(uint32_t)p_; edx=(uint32_t)(p_>>32); cf=of=(edx!=0); }", rd(o, 32));
        if (w == 16) return T("{ uint32_t p_=(eax&0xFFFFu)*%s; SET16(eax,p_); SET16(edx,p_>>16); cf=of=((p_>>16)!=0); }", rd(o, 16));
        return T("{ uint32_t p_=(eax&0xFFu)*%s; SET16(eax,p_); cf=of=((p_>>8)!=0); }", rd(o, 8));
    }
    if (B("idiv") || B("div")) {
        const char *o = ops[0]; char *de = T("rt_divide_error(0x%xu)", a); int id = B("idiv");
        if (w == 32 && id) return T("{ int64_t n_=(int64_t)(((uint64_t)edx<<32)|eax); int32_t d_=(int32_t)%s; if(!d_) %s; else { eax=(uint32_t)(n_/d_); edx=(uint32_t)(n_%%d_); } }", rd(o, 32), de);
        if (w == 32) return T("{ uint64_t n_=((uint64_t)edx<<32)|eax; uint32_t d_=%s; if(!d_) %s; else { eax=(uint32_t)(n_/d_); edx=(uint32_t)(n_%%d_); } }", rd(o, 32), de);
        if (w == 16 && id) return T("{ int32_t n_=(int32_t)(((edx&0xFFFFu)<<16)|(eax&0xFFFFu)); int32_t d_=(int16_t)%s; if(!d_) %s; else { SET16(eax,(uint32_t)(n_/d_)); SET16(edx,(uint32_t)(n_%%d_)); } }", rd(o, 16), de);
        if (w == 16) return T("{ uint32_t n_=((edx&0xFFFFu)<<16)|(eax&0xFFFFu); uint32_t d_=%s; if(!d_) %s; else { SET16(eax,n_/d_); SET16(edx,n_%%d_); } }", rd(o, 16), de);
        if (id) return T("{ int32_t n_=(int16_t)(eax&0xFFFFu); int32_t d_=(int8_t)%s; if(!d_) %s; else { SET8L(eax,(uint32_t)(n_/d_)); SET8H(eax,(uint32_t)(n_%%d_)); } }", rd(o, 8), de);
        return T("{ uint32_t n_=eax&0xFFFFu; uint32_t d_=%s; if(!d_) %s; else { SET8L(eax,n_/d_); SET8H(eax,n_%%d_); } }", rd(o, 8), de);
    }
    unsup(); return 0;
}

int prog_fix(prog_t *P, uint32_t src, uint32_t *dst) {
    size_t lo = 0, hi = P->nfix;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (P->fix_src[m] < src) lo = m + 1; else hi = m; }
    if (lo < P->nfix && P->fix_src[lo] == src) { if (dst) *dst = P->fix_dst[lo]; return 1; }
    return 0;
}

static int ends_block(const char *mn) {   /* re.match(r'(?:\S+\s)*(jmp|ret|retl|lret|iret)$', mn) */
    const char *l = strrchr(mn, ' '); l = l ? l + 1 : mn;
    return !strcmp(l, "jmp") || !strcmp(l, "ret") || !strcmp(l, "retl") || !strcmp(l, "lret") || !strcmp(l, "iret");
}

void lift_function(prog_t *P, int fid, uint32_t start, const uint32_t *addrs, int na, sb_t *out, int *nunsup)
{
    Fn F = {P, fid, start, start}, *s = &F;
    for (int i = 0; i < na; i++) P->body[addrs[i] - P->base] = fid;
    /* segment-aware function: loads %ds itself */
    segaware = 0;
    for (int i = 0; i < na; i++) {
        ins_t *I = P->ins[addrs[i] - P->base]; size_t L = strlen(I->ops);
        if ((L >= 4 && !strcmp(I->ops + L - 4, ",%ds")) || (!strcmp(I->ops, "%ds") && strstr(I->mn, "pop")) || !strcmp(I->mn, "lds")) { segaware = 1; break; }
    }
    char **code = malloc(na * sizeof *code), **tails = malloc(na * sizeof *tails);
    for (int i = 0; i < na; i++) {
        uint32_t a = addrs[i];
        if (setjmp(unsup_jb)) { (*nunsup)++; code[i] = strdup(T("rt_unimpl(0x%xu,\"unsupported\");", a)); continue; }
        code[i] = strdup(lift1(s, a));
    }
    for (int i = 0; i < na; i++) {
        uint32_t a = addrs[i]; ins_t *I = P->ins[a - P->base]; uint32_t nxt = a + I->n;
        int has_next = i + 1 < na; uint32_t following = has_next ? addrs[i + 1] : 0;
        s->cur = a;
        tails[i] = strdup((!ends_block(I->mn) && (!has_next || following != nxt)) ? target(s, nxt) : "");
    }
    sb_printf(out, "void %s%06x(void) {\n  FLAGS_LOCAL;\n  TRACE_FN(0x%xu);", g_fpfx, start, start);
    if (na && addrs[0] != start) { P->label[start - P->base] = fid; sb_printf(out, "\n  goto L_%x;", start); }
    for (int i = 0; i < na; i++) {
        uint32_t a = addrs[i]; ins_t *I = P->ins[a - P->base];
        if (P->label[a - P->base] == fid) sb_printf(out, "\n L_%x:;", a);
        sb_printf(out, "\n  /* %06x: ", a);
        /* (mn + ' ' + ops).replace('*' '/', '* /') */
        char *cm = T("%s %s", I->mn, I->ops);
        for (char *p = cm; *p; p++) { if (p[0] == '*' && p[1] == '/') sb_add(out, "* "); else sb_addn(out, p, 1); }
        sb_add(out, " */ "); sb_add(out, code[i]);
        if (tails[i][0]) { sb_add(out, "\n  "); sb_add(out, tails[i]); }
        free(code[i]); free(tails[i]);
    }
    sb_add(out, "\n}");
    free(code); free(tails);
    tmp_reset();
}
