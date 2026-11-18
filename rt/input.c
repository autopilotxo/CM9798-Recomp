/* input.c - keyboard (INT 16h / DOS console) and mouse (INT 33h) emulation.
 * Backends feed events with input_key / input_mouse_*; the game's INT 33h AX=000Ch
 * event handler is called at safe points (when the game polls), like a driver IRQ would.
 */
#include "rt_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define AH ((eax >> 8) & 0xFF)
#define AL (eax & 0xFF)
#define AX (eax & 0xFFFF)

/* ---------- keyboard ---------- */
static uint16_t kq[64]; static unsigned kq_head, kq_tail;
void input_key(uint16_t scan_ascii) { if (((kq_tail + 1) & 63) != kq_head) { kq[kq_tail] = scan_ascii; kq_tail = (kq_tail + 1) & 63; } }
int  input_key_ready(void) { return kq_head != kq_tail; }
/* forget keys pressed but not yet read by the game */
void input_flush_keys(void);
static uint16_t key_peek(void) { return kq[kq_head]; }
static uint16_t key_pop(void) { uint16_t k = kq[kq_head]; kq_head = (kq_head + 1) & 63; return k; }

extern void host_poll(void);
extern void host_wait(void);          /* backend: block until an event arrives (or exit) */

uint16_t input_getkey_blocking(void) {
    while (!input_key_ready()) host_wait();
    return key_pop();
}

void kbd_int16(void) {
    switch (AH) {
    case 0x00: case 0x10: SET16(eax, input_getkey_blocking()); break;
    case 0x01: case 0x11: if (input_key_ready()) { g_zf = 0; SET16(eax, key_peek()); } else g_zf = 1; break;
    case 0x02: case 0x12: SET8L(eax, 0); break;
    default: break;
    }
}

/* DOS console input, called from int21() for AH=01/06/07/08/0B */
static int pending_ext = -1;
uint32_t dos_console_read(int blocking) {
    if (pending_ext >= 0) { uint32_t c = (uint32_t)pending_ext; pending_ext = -1; return c; }
    if (!blocking && !input_key_ready()) return 0x100;
    uint16_t k = input_getkey_blocking();
    if ((k & 0xFF) == 0 || (k & 0xFF) == 0xE0) { pending_ext = k >> 8; return 0; }
    return k & 0xFF;
}
int dos_console_ready(void) { return pending_ext >= 0 || input_key_ready(); }

/* ---------- mouse ---------- */
static int mx = 320, my = 240, mbuttons, minx = 0, maxx = 639, miny = 0, maxy = 479;
static uint32_t h_mask, h_off, h_sel;
static unsigned press_cnt[3], rel_cnt[3]; static int press_x[3], press_y[3], rel_x[3], rel_y[3];
struct mev { uint16_t mask, buttons; int16_t x, y; };
static struct mev evq[64]; static unsigned ev_head, ev_tail;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static void queue_event(uint16_t mask) {
    if (((ev_tail + 1) & 63) == ev_head) return;
    evq[ev_tail] = (struct mev){ mask, (uint16_t)mbuttons, (int16_t)mx, (int16_t)my }; ev_tail = (ev_tail + 1) & 63;
}
/* backends pass screen pixels; map them onto the range the game set with INT 33h 07h/08h */
extern int video_width(void), video_height(void);
void input_mouse_move(int px, int py) {
    int sw = video_width(), sh = video_height();
    int x = minx + (int)((long)px * (maxx - minx + 1) / (sw > 0 ? sw : 640));
    int y = miny + (int)((long)py * (maxy - miny + 1) / (sh > 0 ? sh : 480));
    x = clampi(x, minx, maxx); y = clampi(y, miny, maxy);
    if (x != mx || y != my) { mx = x; my = y; queue_event(0x01); }
}
void input_mouse_button(int b, int down) {
    if (b < 0 || b > 2) return;
    int bit = 1 << b;
    if (down) { mbuttons |= bit; press_cnt[b]++; press_x[b] = mx; press_y[b] = my; queue_event((uint16_t)(0x02 << (2 * b))); }
    else { mbuttons &= ~bit; rel_cnt[b]++; rel_x[b] = mx; rel_y[b] = my; queue_event((uint16_t)(0x04 << (2 * b))); }
}

/* call the game's registered mouse handler like the driver's IRQ would */
static int in_callback;
void input_dispatch_mouse(void) {
    if (in_callback || !h_off) { ev_head = ev_tail; return; }
    while (ev_head != ev_tail) {
        struct mev e = evq[ev_head]; ev_head = (ev_head + 1) & 63;
        if (!(e.mask & h_mask)) continue;
        uint32_t s[8] = { eax, ebx, ecx, edx, esi, edi, ebp, esp }, sf[5] = { g_cf, g_zf, g_sf, g_of, g_df };
        uint32_t ss[6]; memcpy(ss, sel_, sizeof ss);
        in_callback = 1;
        eax = e.mask; ebx = e.buttons; ecx = (uint32_t)e.x; edx = (uint32_t)e.y; esi = 0; edi = 0;
        esp -= 64;                                   /* red zone below the interrupted code */
        PUSH32(0x170); PUSH32(0xFFFFFFF0u);          /* far return: CS, sentinel EIP */
        rt_call(h_off);
        in_callback = 0;
        eax = s[0]; ebx = s[1]; ecx = s[2]; edx = s[3]; esi = s[4]; edi = s[5]; ebp = s[6]; esp = s[7];
        g_cf = sf[0]; g_zf = sf[1]; g_sf = sf[2]; g_of = sf[3]; g_df = sf[4];
        for (int i = 0; i < 6; i++) rt_set_seg(i, ss[i]);
    }
}

void mouse_int33(void) {
    switch (AX) {
    case 0x0000: SET16(eax, 0xFFFF); SET16(ebx, 2); h_off = 0; break;
    case 0x0001: case 0x0002: break;
    case 0x0003: SET16(ebx, mbuttons); SET16(ecx, mx); SET16(edx, my); break;
    case 0x0004: mx = clampi((int)(int16_t)ecx, minx, maxx); my = clampi((int)(int16_t)edx, miny, maxy); break;
    case 0x0005: { int b = ebx & 3; SET16(eax, mbuttons); SET16(ebx, press_cnt[b]); SET16(ecx, press_x[b]); SET16(edx, press_y[b]); press_cnt[b] = 0; break; }
    case 0x0006: { int b = ebx & 3; SET16(eax, mbuttons); SET16(ebx, rel_cnt[b]); SET16(ecx, rel_x[b]); SET16(edx, rel_y[b]); rel_cnt[b] = 0; break; }
    case 0x0007: minx = (int16_t)ecx; maxx = (int16_t)edx; if (minx > maxx) { int t = minx; minx = maxx; maxx = t; }
        if (getenv("CM_TRACE")) fprintf(stderr, "[mouse] x range %d..%d\n", minx, maxx); break;
    case 0x0008: miny = (int16_t)ecx; maxy = (int16_t)edx; if (miny > maxy) { int t = miny; miny = maxy; maxy = t; }
        if (getenv("CM_TRACE")) fprintf(stderr, "[mouse] y range %d..%d\n", miny, maxy); break;
    case 0x000B: SET16(ecx, 0); SET16(edx, 0); break;
    case 0x000C: h_mask = ecx & 0xFFFF; h_off = edx; h_sel = sel_[0];
        if (getenv("CM_TRACE")) fprintf(stderr, "[mouse] event handler %04X:%08X mask %04X\n", h_sel, h_off, h_mask); break;
    case 0x000F: case 0x001A: case 0x001D: break;
    case 0x0024: SET16(ebx, 0x0626); SET16(ecx, 0x0400); break;
    default: break;
    }
}
int input_mouse_x(void) { return mx; }
int input_mouse_y(void) { return my; }

void input_flush_keys(void) { kq_head = kq_tail; pending_ext = -1; }
