/* rt.h - runtime for statically recompiled CM2E16 (x86-32 guest -> C, any 64-bit host).
   Guest memory is a flat 32-bit address space inside M; all guest pointers stay 32-bit. */
#ifndef CM_RT_H
#define CM_RT_H
#include <stdint.h>
#include <string.h>
#include <math.h>

#define GUEST_MEM_SIZE (112u<<20)  /* 64MB as the game had it, and above that the data relocations.txt moves */
extern uint8_t *M;
extern uint32_t eax,ecx,edx,ebx,esp,ebp,esi,edi;
extern uint32_t g_cf,g_zf,g_sf,g_of,g_df;
extern uint32_t g_ret;   /* address the last guest return went to */
/* segments: 0 ES 1 CS 2 SS 3 DS 4 FS 5 GS. Bases come from the emulated descriptor table. */
extern uint32_t sel_[6], segbase_[6];
void rt_set_seg(int idx, uint32_t sel);

/* memory (unaligned-safe, little-endian host assumed: x86-64 / ARM64) */
static inline uint32_t RD8 (uint32_t a){ return M[a]; }
static inline uint32_t RD16(uint32_t a){ uint16_t v; memcpy(&v,M+a,2); return v; }
static inline uint32_t RD32(uint32_t a){ uint32_t v; memcpy(&v,M+a,4); return v; }
static inline void WR8 (uint32_t a,uint32_t v){ M[a]=(uint8_t)v; }
static inline void WR16(uint32_t a,uint32_t v){ uint16_t t=(uint16_t)v; memcpy(M+a,&t,2); }
static inline void WR32(uint32_t a,uint32_t v){ memcpy(M+a,&v,4); }
static inline float  RDF32(uint32_t a){ float v; memcpy(&v,M+a,4); return v; }
static inline double RDF64(uint32_t a){ double v; memcpy(&v,M+a,8); return v; }
static inline void WRF32(uint32_t a,float v){ memcpy(M+a,&v,4); }
static inline void WRF64(uint32_t a,double v){ memcpy(M+a,&v,8); }

/* partial registers */
#define SET8L(r,v)  ((r)=((r)&0xFFFFFF00u)|((uint32_t)(v)&0xFFu))
#define SET8H(r,v)  ((r)=((r)&0xFFFF00FFu)|(((uint32_t)(v)&0xFFu)<<8))
#define SET16(r,v)  ((r)=((r)&0xFFFF0000u)|((uint32_t)(v)&0xFFFFu))

#define PUSH32(v) do{ uint32_t _v=(v); esp-=4; WR32(esp,_v);}while(0)
#define PUSH16(v) do{ uint32_t _v=(v); esp-=2; WR16(esp,_v);}while(0)
static inline uint32_t POP32(void){ uint32_t v=RD32(esp); esp+=4; return v; }
static inline uint32_t POP16(void){ uint32_t v=RD16(esp); esp+=2; return v; }

/* flags: each generated function keeps CF/ZF/SF/OF in locals, spilled around calls */
#define FLAGS_LOCAL uint32_t cf=g_cf,zf=g_zf,sf=g_sf,of=g_of
#define SPILL  (g_cf=cf,g_zf=zf,g_sf=sf,g_of=of)
#define RELOAD (cf=g_cf,zf=g_zf,sf=g_sf,of=g_of)
#define MSK(w) ((w)==32?0xFFFFFFFFu:((1u<<(w))-1u))
#define MSB(w,x) (((x)>>((w)-1))&1u)
#define SZF(w,r) (zf=((r)==0), sf=MSB(w,r))

/* runtime services implemented in rt.c / shims */
typedef void (*gfn)(void);
void     rt_call(uint32_t target);
int      rt_is_retaddr(uint32_t a);           /* indirect call/jmp to guest code */
void     rt_int(uint32_t n);                 /* software interrupt  */
uint32_t rt_in(uint32_t port,int size);
void     rt_out(uint32_t port,uint32_t v,int size);
void     rt_sys(uint32_t addr,const char *what); /* lar/lsl/verr/sidt/far ret ... */
void     rt_unimpl(uint32_t addr,const char *what);
void     rt_divide_error(uint32_t addr);
/* string ops; rep: 0 none, 1 rep/repe, 2 repne. flags via g_* */
void rt_movs(int size,int rep); void rt_stos(int size,int rep);
void rt_lods(int size,int rep); void rt_scas(int size,int rep); void rt_cmps(int size,int rep);
/* shifts/rotates needing carry chains */
uint32_t rt_rot(int op,int w,uint32_t a,uint32_t n); /* op 0 rol 1 ror 2 rcl 3 rcr; uses/sets g_cf,g_of */
uint32_t rt_eflags(void); void rt_set_eflags(uint32_t f);

/* x87 */
extern long double fst_[8]; extern int ftop; extern uint32_t fsw, fcw;
#define ST(i) fst_[(ftop+(i))&7]
static inline void FPUSH(long double v){ ftop=(ftop-1)&7; fst_[ftop]=v; }
static inline long double FPOP(void){ long double v=fst_[ftop]; ftop=(ftop+1)&7; return v; }
void     fpu_compare(long double a,long double b);  /* sets C0/C2/C3 in fsw */
int64_t  fpu_round(long double v);                   /* per fcw RC */
uint32_t fpu_status(void);
void     fpu_setcw(void);   /* mirror the guest's precision setting onto the host FPU */
long double fpu_ld80(uint32_t a); void fpu_st80(uint32_t a,long double v);
void fpu_save(uint32_t a); void fpu_restore(uint32_t a);
#ifdef CALLTRACE
extern uint32_t g_trace_ring[256]; extern unsigned g_trace_pos;
extern uint32_t g_break_fn, g_watch_addr; void rt_break(uint32_t a); void rt_watch(uint32_t a);
#define TRACE_FN(a) (g_trace_ring[g_trace_pos++ & 255] = (a), ((a) == g_break_fn ? rt_break(a) : (void)0), (g_watch_addr ? rt_watch(a) : (void)0))
#else
#define TRACE_FN(a) ((void)0)
#endif
void rt_dump_trace(void);
/* asynchronous "interrupt" delivery at loop back-edges */
extern uint32_t g_backedge_ctr; void rt_backedge(void);
#define BACKEDGE do { if (++g_backedge_ctr >= 16384u) rt_backedge(); } while (0)
#endif
