#pragma once
#include "rt.h"
#include <stddef.h>
struct gfunc { uint32_t addr; gfn fn; };
extern const struct gfunc g_funcs[];
extern const unsigned g_nfuncs;
gfn rt_lookup(uint32_t addr);
int  rt_load_image(const char *dir);
void rt_init(void);

/* ---- more than one program (rt/editor.c): the game, and the data editor that came with it ----
 * Each has its own function tables (gen/: g_funcs.., e_funcs..) and its own state: guest memory,
 * registers, descriptors, DOS (files, memory, vectors), mouse handler, screen and palette. A module's
 * state is a list of variables; swapping it with a buffer twice gives the first program back. */
struct rt_var { void *p; size_t n; };
size_t rt_vars_size(const struct rt_var *v, int n);
void   rt_vars_swap(const struct rt_var *v, int n, unsigned char *buf);
void rt_set_program(const struct gfunc *f, unsigned nf, const uint32_t *ret, unsigned nret);
size_t rt_state_size(void);   void rt_state_swap(void *buf);     /* rt.c: memory, registers, descriptors, FPU */
size_t dos_state_size(void);  void dos_state_swap(void *buf);    /* dos.c */
size_t input_state_size(void); void input_state_swap(void *buf); /* input.c (not the pointer's position) */
size_t video_state_size(void); void video_state_swap(void *buf); /* video.c */
void dos_close_files(void);
void editor_startup(void);           /* rt/editor.c: keeps the state programs start with (first thing in main) */
int  editor_available(void);         /* the Database Editor can run (gen/ has it, CM2ED.EXE is there) */
extern int g_editor_running;
