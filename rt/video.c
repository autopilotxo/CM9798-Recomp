/* video.c - VESA BIOS (VBE 1.2/2.0 subset) + VGA emulation for the recompiled CM2E16.
 *
 * The game draws 640x480x8 (mode 0x101) through a banked 64 KB window at A000:0000.
 * Recompiled code writes that window as plain guest memory (M + 0xA0000), so a bank
 * switch copies the window out to the full framebuffer and the new bank back in.
 * The display backend (headless.c here, sdl.c on Windows) calls video_frame() to get
 * the current picture as 8-bit indices + palette.
 */
#include "rt_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define WIN_LIN   0x000A0000u
#define WIN_SIZE  0x10000u
#define FB_SIZE   (1024u * 1024u)
#define VBE_SCRATCH 0x0009F000u          /* real-mode buffer for VbeInfo strings / mode list */

extern uint8_t vga_dac[256][3];
static uint8_t fb[FB_SIZE];
static uint32_t cur_bank, cur_mode = 3, width = 80, height = 25, pitch = 0;
static int trace;
int video_dirty;

static void win_flush(void) { if (cur_mode >= 0x100) memcpy(fb + cur_bank * WIN_SIZE, M + WIN_LIN, WIN_SIZE); }
static void win_load(void)  { if (cur_mode >= 0x100) memcpy(M + WIN_LIN, fb + cur_bank * WIN_SIZE, WIN_SIZE); }

/* -------- for the display backend -------- */
int video_width(void)  { return cur_mode >= 0x100 ? (int)width : cur_mode == 0x13 ? 320 : 640; }
int video_height(void) { return cur_mode >= 0x100 ? (int)height : cur_mode == 0x13 ? 200 : 480; }
int video_frame(const uint8_t **pixels, int *w, int *h, int *stride, uint8_t pal[256][3]) {
    if (cur_mode < 0x100 && cur_mode != 0x13) return 0;
    if (cur_mode == 0x13) { *pixels = M + WIN_LIN; *w = 320; *h = 200; *stride = 320; }
    else { win_flush(); *pixels = fb; *w = (int)width; *h = (int)height; *stride = (int)pitch; }
    for (int i = 0; i < 256; i++) for (int c = 0; c < 3; c++)
        pal[i][c] = (uint8_t)((vga_dac[i][c] << 2) | (vga_dac[i][c] >> 4));
    return 1;
}

/* -------- VBE -------- */
struct vregs { uint32_t ax, bx, cx, dx, di, es_lin; };

static const uint16_t modes[] = { 0x100, 0x101, 0xFFFF };

static void vbe_info(uint32_t dst) {
    memset(M + dst, 0, 256);
    memcpy(M + dst, "VESA", 4);
    WR16(dst + 4, 0x0102);                                       /* VBE 1.2: banked only, as in 1997 */
    memcpy(M + VBE_SCRATCH, "CM9798 recomp VBE", 18);
    WR32(dst + 6, (VBE_SCRATCH >> 4) << 16);                     /* OEM string far ptr */
    WR32(dst + 10, 0);                                           /* capabilities: 6-bit DAC */
    uint32_t ml = VBE_SCRATCH + 0x40;
    for (unsigned i = 0; i < sizeof modes / 2; i++) WR16(ml + 2 * i, modes[i]);
    WR32(dst + 14, ((VBE_SCRATCH >> 4) << 16) | 0x40);           /* mode list far ptr */
    WR16(dst + 18, 16);                                          /* 16 x 64 KB = 1 MB */
}

static int mode_dims(uint32_t m, uint32_t *w, uint32_t *h) {
    if (m == 0x100) { *w = 640; *h = 400; return 1; }
    if (m == 0x101) { *w = 640; *h = 480; return 1; }
    return 0;
}

static void vbe_modeinfo(uint32_t mode, uint32_t dst) {
    uint32_t w, h; memset(M + dst, 0, 256);
    if (!mode_dims(mode & 0x1FF, &w, &h)) return;
    WR16(dst + 0x00, 0x001B);       /* supported, optional info, colour, graphics */
    M[dst + 0x02] = 0x07;           /* window A: exists, readable, writable */
    M[dst + 0x03] = 0x00;
    WR16(dst + 0x04, 64);           /* granularity KB */
    WR16(dst + 0x06, 64);           /* window size KB */
    WR16(dst + 0x08, 0xA000);
    WR16(dst + 0x0A, 0);
    WR32(dst + 0x0C, 0);            /* no window function: game uses INT 10h 4F05 */
    WR16(dst + 0x10, w);            /* bytes per scanline */
    WR16(dst + 0x12, w); WR16(dst + 0x14, h);
    M[dst + 0x16] = 8; M[dst + 0x17] = 16; M[dst + 0x18] = 1; M[dst + 0x19] = 8;
    M[dst + 0x1A] = 1; M[dst + 0x1B] = 4; M[dst + 0x1C] = 0; M[dst + 0x1D] = 0; M[dst + 0x1E] = 1;
}

static void set_bank(uint32_t b) {
    if (b == cur_bank) return;
    win_flush(); cur_bank = b & 15; win_load(); video_dirty = 1;
}

static void vbe(struct vregs *r) {
    uint32_t fn = r->ax & 0xFF;
    r->ax = 0x004F;                                              /* default: success */
    switch (fn) {
    case 0x00: vbe_info(r->es_lin + r->di); break;
    case 0x01: vbe_modeinfo(r->cx & 0xFFFF, r->es_lin + r->di); break;
    case 0x02: { uint32_t m = r->bx & 0x1FF, w, h;
        if (!mode_dims(m, &w, &h)) { r->ax = 0x014F; break; }
        cur_mode = m; width = w; height = h; pitch = w; cur_bank = 0;
        if (!(r->bx & 0x8000)) { memset(fb, 0, FB_SIZE); memset(M + WIN_LIN, 0, WIN_SIZE); }
        if (trace) fprintf(stderr, "[vbe] set mode %03X (%ux%u)\n", m, w, h);
        video_dirty = 1; break; }
    case 0x03: r->bx = cur_mode; break;
    case 0x05: if ((r->bx & 0xFF00) == 0x0100) r->dx = cur_bank; else set_bank(r->dx & 0xFFFF); break;
    case 0x06: r->bx = pitch; r->cx = width; r->dx = 16; break;
    case 0x07: r->bx = 0; r->cx = 0; r->dx = 0; break;
    case 0x08: r->bx = 0x0600; break;                            /* DAC width 6 */
    case 0x09: { uint32_t n = r->cx & 0xFFFF, first = r->dx & 0xFF, src = r->es_lin + r->di;
        for (uint32_t i = 0; i < n && first + i < 256; i++) {
            vga_dac[first + i][2] = M[src + 4 * i] & 0x3F; vga_dac[first + i][1] = M[src + 4 * i + 1] & 0x3F;
            vga_dac[first + i][0] = M[src + 4 * i + 2] & 0x3F; }
        video_dirty = 1; break; }
    default: if (trace) fprintf(stderr, "[vbe] function 4F%02X not supported\n", fn); r->ax = 0x014F; break;
    }
}

static void vga_bios(struct vregs *r) {
    uint32_t ah = (r->ax >> 8) & 0xFF, al = r->ax & 0xFF;
    switch (ah) {
    case 0x00: cur_mode = al & 0x7F; M[0x449] = (uint8_t)cur_mode; cur_bank = 0;
        if (trace) fprintf(stderr, "[vga] set mode %02X\n", cur_mode); video_dirty = 1; break;
    case 0x0F: r->ax = (80 << 8) | (cur_mode < 0x100 ? cur_mode : 0x62); r->bx &= 0x00FF; break;
    case 0x10: if (al == 0x12) {                                 /* set block of DAC registers */
            uint32_t first = r->bx & 0xFF, n = r->cx & 0xFFFF, src = r->es_lin + r->dx;
            for (uint32_t i = 0; i < n && first + i < 256; i++)
                for (int c = 0; c < 3; c++) vga_dac[first + i][c] = M[src + 3 * i + c] & 0x3F;
            video_dirty = 1; }
        else if (al == 0x10) { uint32_t i = r->bx & 0xFF;
            vga_dac[i][0] = (r->dx >> 8) & 0x3F; vga_dac[i][1] = (r->cx >> 8) & 0x3F; vga_dac[i][2] = r->cx & 0x3F; }
        break;
    case 0x4F: vbe(r); break;
    default: if (trace) fprintf(stderr, "[vga] INT 10h AX=%04X ignored\n", r->ax & 0xFFFF); break;
    }
}

/* protected-mode INT 10h (DOS/4GW reflects it to the BIOS; ES is a selector) */
void video_int10(void) {
    trace = getenv("CM_TRACE") != NULL;
    struct vregs r = { eax & 0xFFFF, ebx & 0xFFFF, ecx & 0xFFFF, edx & 0xFFFF, edi, segbase_[0] };
    vga_bios(&r);
    SET16(eax, r.ax); SET16(ebx, r.bx); SET16(ecx, r.cx); SET16(edx, r.dx);
}

/* DPMI 0300h: simulate real-mode interrupt with a register structure (ES is a segment) */
void rm_interrupt(uint32_t n, uint32_t rr) {
    trace = getenv("CM_TRACE") != NULL;
    if (n == 0x10) {
        struct vregs r = { RD16(rr + 0x1C), RD16(rr + 0x10), RD16(rr + 0x18), RD16(rr + 0x14), RD16(rr + 0x00),
                           (uint32_t)RD16(rr + 0x22) << 4 };
        vga_bios(&r);
        WR16(rr + 0x1C, r.ax); WR16(rr + 0x10, r.bx); WR16(rr + 0x18, r.cx); WR16(rr + 0x14, r.dx);
        return;
    }
    if (trace) fprintf(stderr, "[dpmi] real-mode INT %02X AX=%04X ignored\n", n, RD16(rr + 0x1C));
}

uint32_t vbe_lfb_linear(void) { return WIN_LIN; }
