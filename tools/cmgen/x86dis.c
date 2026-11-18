/* x86dis.c - 32-bit x86 decoder that writes GNU objdump's AT&T text (objdump -D -mi386), for the
   instructions CM2E16.EXE uses, so the recompiler sees exactly the disassembly it was developed on.
   x86_dis() returns the instruction length (0 = can't decode) and the mnemonic/operands split the
   way make_gen.py splits objdump's line (a leading prefix word joined to the next word). */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "x86dis.h"

static const char *R32[8] = {"eax","ecx","edx","ebx","esp","ebp","esi","edi"};
static const char *R16[8] = {"ax","cx","dx","bx","sp","bp","si","di"};
static const char *R8[8]  = {"al","cl","dl","bl","ah","ch","dh","bh"};
static const char *SR[8]  = {"es","cs","ss","ds","fs","gs","?","?"};
static const char *CC[16] = {"o","no","b","ae","e","ne","be","a","s","ns","p","np","l","ge","le","g"};

typedef struct {
    const uint8_t *p; int n, avail; uint32_t addr;
    int opsz16, adsz16, seg, rep, lock;        /* prefixes: seg = 0..5 or -1; rep = 0, 0xf2, 0xf3 */
    int used66, used67, usedseg, notrack;
    uint8_t pfx[16]; int npfx;
    int modrm, mod, reg, rm, hasmodrm;
    char mem[64];                               /* the r/m operand when it is memory */
    int err;
} D;

static int u8(D *d) { if (d->n >= d->avail) { d->err = 1; return 0; } return d->p[d->n++]; }
static int32_t s8(D *d) { return (int8_t)u8(d); }
static uint32_t u16(D *d) { uint32_t a = u8(d); return a | (uint32_t)u8(d) << 8; }
static uint32_t u32(D *d) { uint32_t a = u16(d); return a | u16(d) << 16; }

static void disp_signed(char *b, int32_t v) {
    if (v < 0) { if (v == INT32_MIN) strcpy(b, "-0x80000000"); else sprintf(b, "-0x%x", (unsigned)-v); }
    else sprintf(b, "0x%x", (unsigned)v);
}

static void segpfx(D *d, char *b) {           /* "%es:" when a segment prefix applies to memory */
    if (d->seg >= 0) { sprintf(b, "%%%s:", SR[d->seg]); d->usedseg = 1; } else b[0] = 0;
}

static void modrm(D *d) {
    int m = u8(d); d->hasmodrm = 1; d->modrm = m; d->mod = m >> 6; d->reg = (m >> 3) & 7; d->rm = m & 7;
    if (d->mod == 3) return;
    if (d->adsz16) { d->err = 1; return; }    /* 16-bit addressing: not used with ModRM here */
    int base = d->rm, havesib = 0, idx = 4, scale = 0;
    if (d->rm == 4) { int s = u8(d); havesib = 1; scale = s >> 6; idx = (s >> 3) & 7; base = s & 7; }
    int32_t disp = 0;
    int havebase = 1;
    if (d->mod == 0 && base == 5) { havebase = 0; disp = (int32_t)u32(d); }
    else if (d->mod == 1) disp = s8(d);
    else if (d->mod == 2) disp = (int32_t)u32(d);
    int haveindex = idx != 4;
    int needindex = havesib && !havebase && !haveindex;
    int havedisp = havebase || needindex || (havesib && (haveindex || scale != 0));
    char sp[8], db[24] = "", ib[40] = "";
    segpfx(d, sp);
    if (d->mod != 0 || base == 5) {
        if (havedisp) disp_signed(db, disp); else sprintf(db, "0x%x", (unsigned)disp);
    }
    if (havebase || haveindex || needindex) {
        char *q = ib; *q++ = '(';
        if (havebase) q += sprintf(q, "%%%s", R32[base]);
        if (havesib && (scale != 0 || needindex || haveindex || (havebase && base != 4)))
            q += sprintf(q, ",%%%s,%d", haveindex ? R32[idx] : "eiz", 1 << scale);
        *q++ = ')'; *q = 0;
    }
    snprintf(d->mem, sizeof d->mem, "%s%s%s", sp, db, ib);
}

/* operand text for r/m with width w (8/16/32) */
static const char *E(D *d, int w, char *b) {
    if (d->mod == 3) { sprintf(b, "%%%s", w == 8 ? R8[d->rm] : w == 16 ? R16[d->rm] : R32[d->rm]); return b; }
    return d->mem;
}
static const char *G(D *d, int w, char *b) {
    sprintf(b, "%%%s", w == 8 ? R8[d->reg] : w == 16 ? R16[d->reg] : R32[d->reg]); return b;
}
static const char *Rn(int w, int r) {
    static char b[4][8]; static int k; k = (k + 1) & 3;
    sprintf(b[k], "%%%s", w == 8 ? R8[r] : w == 16 ? R16[r] : R32[r]); return b[k];
}
static char sfx(int w) { return w == 8 ? 'b' : w == 16 ? 'w' : 'l'; }

int x86_dis(const uint8_t *p, int avail, uint32_t addr, char *mn_out, char *ops_out)
{
    D d0, *d = &d0; memset(d, 0, sizeof *d);
    d->p = p; d->avail = avail; d->addr = addr; d->seg = -1;
    char mn[32] = "", ops[96] = "", t1[16], t2[16];
    /* prefixes */
    for (;;) {
        if (d->n >= avail) return 0;
        int b = p[d->n];
        if (b == 0x66) d->opsz16 = 1;
        else if (b == 0x67) d->adsz16 = 1;
        else if (b == 0xf2 || b == 0xf3) d->rep = b;
        else if (b == 0xf0) d->lock = 1;
        else if (b == 0x26) d->seg = 0; else if (b == 0x2e) d->seg = 1; else if (b == 0x36) d->seg = 2;
        else if (b == 0x3e) d->seg = 3; else if (b == 0x64) d->seg = 4; else if (b == 0x65) d->seg = 5;
        else break;
        if (d->npfx < 15) d->pfx[d->npfx++] = b;
        d->n++;
    }
    int v = d->opsz16 ? 16 : 32;               /* operand size of 'v' operands */
    int op = u8(d), strop = 0, repword = 0;    /* repword: F3 prints "rep" (else "repz") */
    int w;
#define SETMN(s) strcpy(mn, s)
#define OPS(...) snprintf(ops, sizeof ops, __VA_ARGS__)
#define USE66 (d->used66 = 1)
    static const char *ALU[8] = {"add","or","adc","sbb","and","sub","xor","cmp"};
    static const char *SH[8] = {"rol","ror","rcl","rcr","shl","shr","shl","sar"};
    if (op < 0x40 && (op & 7) < 6) {
        const char *nm = ALU[op >> 3];
        switch (op & 7) {
        case 0: modrm(d); SETMN(nm); OPS("%s,%s", G(d,8,t1), E(d,8,t2)); break;
        case 1: modrm(d); USE66; SETMN(nm); OPS("%s,%s", G(d,v,t1), E(d,v,t2)); break;
        case 2: modrm(d); SETMN(nm); OPS("%s,%s", E(d,8,t2), G(d,8,t1)); break;
        case 3: modrm(d); USE66; SETMN(nm); OPS("%s,%s", E(d,v,t2), G(d,v,t1)); break;
        case 4: { int i = u8(d); SETMN(nm); OPS("$0x%x,%%al", i); break; }
        case 5: { uint32_t i = v == 16 ? u16(d) : u32(d); USE66; SETMN(nm); OPS("$0x%x,%s", i, v == 16 ? "%ax" : "%eax"); break; }
        }
    } else switch (op) {
    case 0x06: case 0x0e: case 0x16: case 0x1e: SETMN("push"); OPS("%%%s", SR[op >> 3]); break;
    case 0x07: case 0x17: case 0x1f: SETMN("pop"); OPS("%%%s", SR[op >> 3]); break;
    case 0x0f: {
        int o2 = u8(d);
        if (o2 >= 0x80 && o2 <= 0x8f) { int32_t r = (int32_t)u32(d); sprintf(mn, "j%s", CC[o2 & 15]); OPS("0x%x", addr + d->n + r); break; }
        if (o2 >= 0x90 && o2 <= 0x9f) { modrm(d); sprintf(mn, "set%s", CC[o2 & 15]); OPS("%s", E(d,8,t2)); break; }
        switch (o2) {
        case 0x00: { static const char *g6[8] = {"sldt","str","lldt","ltr","verr","verw",0,0};
            modrm(d); if (!g6[d->reg]) return 0; SETMN(g6[d->reg]);
            if (d->mod == 3) { if (d->reg < 2) { USE66; OPS("%s", Rn(v, d->rm)); } else OPS("%s", Rn(16, d->rm)); }
            else OPS("%s", d->mem);
            break; }
        case 0x01: { static const char *g7[8] = {"sgdtl","sidtl","lgdtl","lidtl","smsw",0,"lmsw","invlpg"};
            modrm(d); if (!g7[d->reg] || (d->mod == 3 && d->reg != 4 && d->reg != 6)) return 0; SETMN(g7[d->reg]);
            if (d->mod == 3) { if (d->reg == 4) { USE66; OPS("%s", Rn(v, d->rm)); } else OPS("%s", Rn(16, d->rm)); }
            else OPS("%s", d->mem);
            break; }
        case 0x02: case 0x03: modrm(d); USE66; SETMN(o2 == 2 ? "lar" : "lsl"); OPS("%s,%s", E(d,16,t2), G(d,v,t1)); break;
        case 0x06: SETMN("clts"); break;
        case 0x09: SETMN("wbinvd"); break;
        case 0x20: case 0x22: modrm(d); SETMN("mov");
            if (o2 == 0x20) OPS("%%cr%d,%s", d->reg, Rn(32, d->rm)); else OPS("%s,%%cr%d", Rn(32, d->rm), d->reg); break;
        case 0x21: case 0x23: modrm(d); SETMN("mov");
            if (o2 == 0x21) OPS("%%db%d,%s", d->reg, Rn(32, d->rm)); else OPS("%s,%%db%d", Rn(32, d->rm), d->reg); break;
        case 0xa0: case 0xa8: SETMN("push"); OPS("%%%s", o2 == 0xa0 ? "fs" : "gs"); break;
        case 0xa1: case 0xa9: SETMN("pop"); OPS("%%%s", o2 == 0xa1 ? "fs" : "gs"); break;
        case 0xa2: SETMN("cpuid"); break;
        case 0xa3: case 0xab: case 0xb3: case 0xbb: modrm(d); USE66;
            SETMN(o2 == 0xa3 ? "bt" : o2 == 0xab ? "bts" : o2 == 0xb3 ? "btr" : "btc"); OPS("%s,%s", G(d,v,t1), E(d,v,t2)); break;
        case 0xa4: case 0xac: { modrm(d); USE66; int i = u8(d); SETMN(o2 == 0xa4 ? "shld" : "shrd"); OPS("$0x%x,%s,%s", i, G(d,v,t1), E(d,v,t2)); break; }
        case 0xa5: case 0xad: modrm(d); USE66; SETMN(o2 == 0xa5 ? "shld" : "shrd"); OPS("%%cl,%s,%s", G(d,v,t1), E(d,v,t2)); break;
        case 0xaf: modrm(d); USE66; SETMN("imul"); OPS("%s,%s", E(d,v,t2), G(d,v,t1)); break;
        case 0xb2: case 0xb4: case 0xb5: modrm(d); if (d->mod == 3) return 0; USE66;
            SETMN(o2 == 0xb2 ? "lss" : o2 == 0xb4 ? "lfs" : "lgs"); OPS("%s,%s", d->mem, G(d,v,t1)); break;
        case 0xb6: case 0xb7: case 0xbe: case 0xbf: {
            modrm(d); USE66; int sw = (o2 & 1) ? 16 : 8;
            sprintf(mn, "mov%c%c%c", (o2 & 8) ? 's' : 'z', sfx(sw), sfx(v)); OPS("%s,%s", E(d,sw,t2), G(d,v,t1)); break; }
        case 0xba: { static const char *g8[8] = {0,0,0,0,"bt","bts","btr","btc"};
            modrm(d); if (!g8[d->reg]) return 0; USE66; int i = u8(d);
            if (d->mod == 3) { SETMN(g8[d->reg]); } else sprintf(mn, "%s%c", g8[d->reg], sfx(v));
            OPS("$0x%x,%s", i, E(d,v,t2)); break; }
        case 0xbc: case 0xbd: modrm(d); USE66; SETMN(o2 == 0xbc ? "bsf" : "bsr"); OPS("%s,%s", E(d,v,t2), G(d,v,t1)); break;
        default:
            if (o2 >= 0xc8 && o2 <= 0xcf) { SETMN("bswap"); OPS("%s", Rn(32, o2 & 7)); break; }
            return 0;
        }
        break; }
    case 0x27: SETMN("daa"); break; case 0x2f: SETMN("das"); break;
    case 0x37: SETMN("aaa"); break; case 0x3f: SETMN("aas"); break;
    case 0x60: USE66; SETMN(v == 16 ? "pushaw" : "pusha"); break;
    case 0x61: USE66; SETMN(v == 16 ? "popaw" : "popa"); break;
    case 0x68: { uint32_t i = v == 16 ? u16(d) : u32(d); USE66; SETMN(v == 16 ? "pushw" : "push"); OPS("$0x%x", i); break; }
    case 0x6a: { int32_t i = s8(d); USE66; SETMN(v == 16 ? "pushw" : "push"); OPS("$0x%x", v == 16 ? (unsigned)i & 0xffff : (unsigned)i); break; }
    case 0x69: case 0x6b: { modrm(d); USE66; uint32_t i;
        if (op == 0x69) i = v == 16 ? u16(d) : u32(d); else i = (uint32_t)s8(d);
        if (v == 16) i &= 0xffff;
        SETMN("imul"); OPS("$0x%x,%s,%s", i, E(d,v,t2), G(d,v,t1)); break; }
    case 0x84: case 0x85: case 0x86: case 0x87: modrm(d); w = (op & 1) ? v : 8; if (op & 1) USE66;
        SETMN(op < 0x86 ? "test" : "xchg"); OPS("%s,%s", G(d,w,t1), E(d,w,t2)); break;
    case 0x88: case 0x89: modrm(d); w = (op & 1) ? v : 8; if (op & 1) USE66; SETMN("mov"); OPS("%s,%s", G(d,w,t1), E(d,w,t2)); break;
    case 0x8a: case 0x8b: modrm(d); w = (op & 1) ? v : 8; if (op & 1) USE66; SETMN("mov"); OPS("%s,%s", E(d,w,t2), G(d,w,t1)); break;
    case 0x8c: modrm(d); if (d->reg > 5) return 0; SETMN("mov");
        if (d->mod == 3) { USE66; OPS("%%%s,%s", SR[d->reg], Rn(v, d->rm)); } else OPS("%%%s,%s", SR[d->reg], d->mem); break;
    case 0x8e: modrm(d); if (d->reg > 5) return 0; SETMN("mov");
        if (d->mod == 3) { USE66; OPS("%s,%%%s", Rn(v, d->rm), SR[d->reg]); } else OPS("%s,%%%s", d->mem, SR[d->reg]); break;
    case 0x8d: modrm(d); if (d->mod == 3) return 0; USE66; SETMN("lea"); OPS("%s,%s", d->mem, G(d,v,t1)); break;
    case 0x8f: modrm(d); if (d->reg) return 0; USE66; SETMN(v == 16 ? "popw" : "pop"); OPS("%s", E(d,v,t2)); break;
    case 0x90: if (d->rep == 0xf3) { SETMN("pause"); d->rep = 0; } else SETMN("nop"); break;
    case 0x98: USE66; SETMN(v == 16 ? "cbtw" : "cwtl"); break;
    case 0x99: USE66; SETMN(v == 16 ? "cwtd" : "cltd"); break;
    case 0x9b: SETMN("fwait"); break;
    case 0x9c: USE66; SETMN(v == 16 ? "pushfw" : "pushf"); break;
    case 0x9d: USE66; SETMN(v == 16 ? "popfw" : "popf"); break;
    case 0x9e: SETMN("sahf"); break; case 0x9f: SETMN("lahf"); break;
    case 0xa0: case 0xa1: case 0xa2: case 0xa3: {
        uint32_t a = u32(d); char sp[8]; segpfx(d, sp); w = (op & 1) ? v : 8; if (op & 1) USE66;
        SETMN("mov"); if (op < 0xa2) OPS("%s0x%x,%s", sp, a, Rn(w, 0)); else OPS("%s,%s0x%x", Rn(w, 0), sp, a); break; }
    case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf: {
        w = (op & 1) ? v : 8; if (op & 1) USE66; strop = 1;
        const char *si = d->adsz16 ? "si" : "esi", *di = d->adsz16 ? "di" : "edi";
        if (d->adsz16) d->used67 = 1;
        char ss[8]; if (d->seg >= 0) { sprintf(ss, "%%%s:", SR[d->seg]); d->usedseg = 1; } else strcpy(ss, "%ds:");
        switch (op & ~1) {
        case 0xa4: sprintf(mn, "movs%c", sfx(w)); OPS("%s(%%%s),%%es:(%%%s)", ss, si, di); repword = 1; break;
        case 0xa6: sprintf(mn, "cmps%c", sfx(w)); OPS("%%es:(%%%s),%s(%%%s)", di, ss, si); break;
        case 0xaa: SETMN("stos"); OPS("%s,%%es:(%%%s)", Rn(w, 0), di); repword = 1; break;
        case 0xac: SETMN("lods"); OPS("%s(%%%s),%s", ss, si, Rn(w, 0)); repword = 1; break;
        case 0xae: SETMN("scas"); OPS("%%es:(%%%s),%s", di, Rn(w, 0)); break;
        }
        break; }
    case 0xa8: { int i = u8(d); SETMN("test"); OPS("$0x%x,%%al", i); break; }
    case 0xa9: { uint32_t i = v == 16 ? u16(d) : u32(d); USE66; SETMN("test"); OPS("$0x%x,%s", i, Rn(v, 0)); break; }
    case 0xc0: case 0xc1: case 0xd0: case 0xd1: case 0xd2: case 0xd3: {
        modrm(d); w = (op & 1) ? v : 8; if (op & 1) USE66;
        if (d->mod == 3) SETMN(SH[d->reg]); else sprintf(mn, "%s%c", SH[d->reg], sfx(w));
        if (op <= 0xc1) { int i = u8(d); OPS("$0x%x,%s", i, E(d,w,t2)); }
        else if (op <= 0xd1) OPS("%s", E(d,w,t2));
        else OPS("%%cl,%s", E(d,w,t2));
        break; }
    case 0xc2: { uint32_t i = u16(d); SETMN("ret"); OPS("$0x%x", i); break; }
    case 0xc3: SETMN("ret"); break;
    case 0xc4: case 0xc5: modrm(d); if (d->mod == 3) return 0; USE66; SETMN(op == 0xc4 ? "les" : "lds"); OPS("%s,%s", d->mem, G(d,v,t1)); break;
    case 0xc6: case 0xc7: { modrm(d); if (d->reg) return 0; w = (op & 1) ? v : 8; if (op & 1) USE66;
        uint32_t i = w == 8 ? (uint32_t)u8(d) : w == 16 ? u16(d) : u32(d);
        if (d->mod == 3) SETMN("mov"); else sprintf(mn, "mov%c", sfx(w));
        OPS("$0x%x,%s", i, E(d,w,t2)); break; }
    case 0xc8: { uint32_t a = u16(d); int b = u8(d); SETMN("enter"); OPS("$0x%x,$0x%x", a, b); break; }
    case 0xc9: SETMN("leave"); break;
    case 0xca: { uint32_t i = u16(d); USE66; SETMN(v == 16 ? "lretw" : "lret"); OPS("$0x%x", i); break; }
    case 0xcb: USE66; SETMN(v == 16 ? "lretw" : "lret"); break;
    case 0xcc: SETMN("int3"); break;
    case 0xcd: { int i = u8(d); SETMN("int"); OPS("$0x%x", i); break; }
    case 0xce: SETMN("into"); break;
    case 0xcf: USE66; SETMN(v == 16 ? "iretw" : "iret"); break;
    case 0xd4: case 0xd5: { int i = u8(d); SETMN(op == 0xd4 ? "aam" : "aad"); OPS("$0x%x", i); break; }
    case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf: {
        modrm(d);
        static const char *MEM[8][8] = {
            {"fadds","fmuls","fcoms","fcomps","fsubs","fsubrs","fdivs","fdivrs"},
            {"flds",0,"fsts","fstps","fldenv","fldcw","fnstenv","fnstcw"},
            {"fiaddl","fimull","ficoml","ficompl","fisubl","fisubrl","fidivl","fidivrl"},
            {"fildl","fisttpl","fistl","fistpl",0,"fldt",0,"fstpt"},
            {"faddl","fmull","fcoml","fcompl","fsubl","fsubrl","fdivl","fdivrl"},
            {"fldl","fisttpll","fstl","fstpl","frstor",0,"fnsave","fnstsw"},
            {"fiadds","fimuls","ficoms","ficomps","fisubs","fisubrs","fidivs","fidivrs"},
            {"filds","fisttps","fists","fistps","fbld","fildll","fbstp","fistpll"}};
        static const char *AR[8] = {"fadd","fmul","fcom","fcomp","fsub","fsubr","fdiv","fdivr"};
        int g = op - 0xd8;
        if (d->mod != 3) { if (!MEM[g][d->reg]) return 0; SETMN(MEM[g][d->reg]); OPS("%s", d->mem); break; }
        int i = d->rm;
        switch (g) {
        case 0: SETMN(AR[d->reg]); if (d->reg == 2 || d->reg == 3) OPS("%%st(%d)", i); else OPS("%%st(%d),%%st", i); break;
        case 4: if (d->reg == 2 || d->reg == 3) return 0; SETMN(AR[d->reg]); OPS("%%st,%%st(%d)", i); break;
        case 6: if (d->reg == 3) { if (i != 1) return 0; SETMN("fcompp"); break; }
            if (d->reg == 2) return 0; sprintf(mn, "%sp", AR[d->reg]); OPS("%%st,%%st(%d)", i); break;
        case 1:
            if (d->reg == 0) { SETMN("fld"); OPS("%%st(%d)", i); break; }
            if (d->reg == 1) { SETMN("fxch"); OPS("%%st(%d)", i); break; }
            if (d->modrm == 0xd0) { SETMN("fnop"); break; }
            { static const char *E9[32] = {"fchs","fabs",0,0,"ftst","fxam",0,0,"fld1","fldl2t","fldl2e","fldpi","fldlg2","fldln2","fldz",0,
                "f2xm1","fyl2x","fptan","fpatan","fxtract","fprem1","fdecstp","fincstp","fprem","fyl2xp1","fsqrt","fsincos","frndint","fscale","fsin","fcos"};
              if (d->modrm < 0xe0 || !E9[d->modrm - 0xe0]) return 0; SETMN(E9[d->modrm - 0xe0]); }
            break;
        case 2: if (d->modrm == 0xe9) { SETMN("fucompp"); break; } return 0;
        case 3: if (d->modrm == 0xe2) { SETMN("fnclex"); break; } if (d->modrm == 0xe3) { SETMN("fninit"); break; } return 0;
        case 5: { static const char *R5[8] = {"ffree",0,"fst","fstp","fucom","fucomp",0,0};
            if (!R5[d->reg]) return 0; SETMN(R5[d->reg]); OPS("%%st(%d)", i); break; }
        case 7: if (d->modrm == 0xe0) { SETMN("fnstsw"); OPS("%%ax"); break; } return 0;
        }
        break; }
    case 0xe0: case 0xe1: case 0xe2: case 0xe3: { int32_t r = s8(d);
        SETMN(op == 0xe0 ? "loopne" : op == 0xe1 ? "loope" : op == 0xe2 ? "loop" : (d->adsz16 ? (d->used67 = 1, "jcxz") : "jecxz"));
        OPS("0x%x", addr + d->n + r); break; }
    case 0xe4: case 0xe5: { int i = u8(d); w = (op & 1) ? v : 8; if (op & 1) USE66; SETMN("in"); OPS("$0x%x,%s", i, Rn(w, 0)); break; }
    case 0xe6: case 0xe7: { int i = u8(d); w = (op & 1) ? v : 8; if (op & 1) USE66; SETMN("out"); OPS("%s,$0x%x", Rn(w, 0), i); break; }
    case 0xe8: case 0xe9: { int32_t r = (int32_t)u32(d); SETMN(op == 0xe8 ? "call" : "jmp"); OPS("0x%x", addr + d->n + r); break; }
    case 0xeb: { int32_t r = s8(d); SETMN("jmp"); OPS("0x%x", addr + d->n + r); break; }
    case 0xec: case 0xed: w = (op & 1) ? v : 8; if (op & 1) USE66; SETMN("in"); OPS("(%%dx),%s", Rn(w, 0)); break;
    case 0xee: case 0xef: w = (op & 1) ? v : 8; if (op & 1) USE66; SETMN("out"); OPS("%s,(%%dx)", Rn(w, 0)); break;
    case 0xf4: SETMN("hlt"); break; case 0xf5: SETMN("cmc"); break;
    case 0xf8: SETMN("clc"); break; case 0xf9: SETMN("stc"); break; case 0xfa: SETMN("cli"); break;
    case 0xfb: SETMN("sti"); break; case 0xfc: SETMN("cld"); break; case 0xfd: SETMN("std"); break;
    case 0xf6: case 0xf7: { static const char *G3[8] = {"test",0,"not","neg","mul","imul","div","idiv"};
        modrm(d); w = (op & 1) ? v : 8; if (op & 1) USE66; if (!G3[d->reg]) return 0;
        if (d->mod == 3) SETMN(G3[d->reg]); else sprintf(mn, "%s%c", G3[d->reg], sfx(w));
        if (d->reg == 0) { uint32_t i = w == 8 ? (uint32_t)u8(d) : w == 16 ? u16(d) : u32(d); OPS("$0x%x,%s", i, E(d,w,t2)); }
        else OPS("%s", E(d,w,t2));
        break; }
    case 0xfe: modrm(d); if (d->reg > 1) return 0;
        if (d->mod == 3) SETMN(d->reg ? "dec" : "inc"); else SETMN(d->reg ? "decb" : "incb");
        OPS("%s", E(d,8,t2)); break;
    case 0xff:
        if (d->seg == 3 && d->n < avail && (((p[d->n] >> 3) & 7) == 2 || ((p[d->n] >> 3) & 7) == 4)) { d->notrack = 1; d->seg = -1; }
        modrm(d);
        switch (d->reg) {
        case 0: case 1: USE66; if (d->mod == 3) SETMN(d->reg ? "dec" : "inc"); else sprintf(mn, "%s%c", d->reg ? "dec" : "inc", sfx(v));
            OPS("%s", E(d,v,t2)); break;
        case 2: case 4:
            SETMN(d->reg == 2 ? "call" : "jmp"); OPS("*%s", E(d,32,t2)); break;
        case 3: case 5: if (d->mod == 3) return 0; USE66; SETMN(d->reg == 3 ? "lcall" : "ljmp"); OPS("*%s", d->mem); break;
        case 6: USE66; SETMN(v == 16 ? "pushw" : "push"); OPS("%s", E(d,v,t2)); break;
        default: return 0;
        }
        break;
    case 0x80: case 0x81: case 0x82: case 0x83: {
        modrm(d); w = (op & 1) ? v : 8; if (op & 1) USE66;
        uint32_t i;
        if (op == 0x81) i = v == 16 ? u16(d) : u32(d);
        else if (op == 0x83) { i = (uint32_t)s8(d); if (v == 16) i &= 0xffff; }
        else i = (uint32_t)u8(d);
        if (d->mod == 3) SETMN(ALU[d->reg]); else sprintf(mn, "%s%c", ALU[d->reg], sfx(w));
        OPS("$0x%x,%s", i, E(d,w,t2)); break; }
    default:
        if (op >= 0x40 && op <= 0x4f) { USE66; SETMN(op < 0x48 ? "inc" : "dec"); OPS("%s", Rn(v, op & 7)); break; }
        if (op >= 0x50 && op <= 0x5f) { USE66; SETMN(op < 0x58 ? "push" : "pop"); OPS("%s", Rn(v, op & 7)); break; }
        if (op >= 0x70 && op <= 0x7f) { int32_t r = s8(d); sprintf(mn, "j%s", CC[op & 15]); OPS("0x%x", addr + d->n + r); break; }
        if (op >= 0x91 && op <= 0x97) { USE66; SETMN("xchg"); OPS("%s,%s", Rn(v, 0), Rn(v, op & 7)); break; }
        if (op >= 0xb0 && op <= 0xb7) { int i = u8(d); SETMN("mov"); OPS("$0x%x,%s", i, Rn(8, op & 7)); break; }
        if (op >= 0xb8 && op <= 0xbf) { uint32_t i = v == 16 ? u16(d) : u32(d); USE66; SETMN("mov"); OPS("$0x%x,%s", i, Rn(v, op & 7)); break; }
        return 0;
    }
    if (d->err || !mn[0]) return 0;
    /* fwait + a no-wait FPU instruction = one instruction (finit, fstsw, fstcw, fsave, ...) */
    /* (handled by the caller: see x86_dis_line) */
    /* prefix words objdump prints in front of the mnemonic, in byte order */
    char line[160] = ""; char *q = line;
    for (int k = 0; k < d->npfx; k++) {
        int b = d->pfx[k]; const char *wd = 0;
        if (b == 0x66 && !d->used66) wd = "data16";
        else if (b == 0x67 && !d->used67) wd = "addr16";
        else if (b == 0xf0) wd = "lock";
        else if (b == 0xf3) wd = (strop && repword) ? "rep" : "repz";
        else if (b == 0xf2) wd = "repnz";
        else if (b == 0x3e && d->notrack) wd = "notrack";
        else if (b != 0x66 && b != 0x67 && b != 0xf2 && b != 0xf3 && !d->usedseg) wd = SR[b == 0x26 ? 0 : b == 0x2e ? 1 : b == 0x36 ? 2 : b == 0x3e ? 3 : b == 0x64 ? 4 : 5];
        if (wd) q += sprintf(q, "%s ", wd);
    }
    q += sprintf(q, "%s", mn);
    if (ops[0]) sprintf(q, " %s", ops);
    /* make_gen.py: first word is the mnemonic; a prefix word is joined to the next word */
    static const char *PW[] = {"rep","repz","repe","repnz","repne","lock","data16","addr16","cs","ds","es","ss","fs","gs",0};
    char *sp = strchr(line, ' ');
    if (!sp) { strcpy(mn_out, line); ops_out[0] = 0; return d->n; }
    *sp = 0; strcpy(mn_out, line); const char *rest = sp + 1;
    for (;;) {
        int isp = 0; for (int k = 0; PW[k]; k++) if (!strcmp(mn_out, PW[k])) isp = 1;
        if (!isp || !*rest) break;
        const char *e = strchr(rest, ' ');
        size_t L = strlen(mn_out);
        if (!e) { sprintf(mn_out + L, " %s", rest); rest = ""; }
        else { mn_out[L] = ' '; memcpy(mn_out + L + 1, rest, e - rest); mn_out[L + 1 + (e - rest)] = 0; rest = e + 1; }
    }
    strcpy(ops_out, rest);
    return d->n;
}

/* the instruction at p as objdump shows it: fwait followed by fnstsw/fnstcw/fnsave/fnstenv/fninit/fnclex
   is one instruction (fstsw, fstcw, ...) */
int x86_dis_line(const uint8_t *p, int avail, uint32_t addr, char *mn, char *ops)
{
    if (avail >= 2 && p[0] == 0x9b) {
        char m2[48], o2[96];
        int n = x86_dis(p + 1, avail - 1, addr + 1, m2, o2);
        static const char *NW[][2] = {{"fnstsw","fstsw"},{"fnstcw","fstcw"},{"fnsave","fsave"},{"fnstenv","fstenv"},
                                      {"fninit","finit"},{"fnclex","fclex"},{0,0}};
        if (n) for (int k = 0; NW[k][0]; k++) if (!strcmp(m2, NW[k][0])) { strcpy(mn, NW[k][1]); strcpy(ops, o2); return n + 1; }
    }
    return x86_dis(p, avail, addr, mn, ops);
}
