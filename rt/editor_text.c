/* editor_text.c - the Database Editor's routines replaced by hand, as overrides.c does the game's: its text
 * routine (CM2ED.EXE 0x21254, the game's 0x1A68C), wrapped so sharp text can redraw what it draws
 * (textlayer.c), and its show_mouse_pointer (0x1A8F8, the game's 0x13714), which leaves the pointer to the
 * OS - drawn into the picture, it covered the letters under it, which then showed their original pixels.
 * A file of its own: it is only linked when gen/ has the editor (ed_*.c call these), as it needs the
 * generated originals. */
#include "rt_internal.h"
#include "textlayer.h"
void e_1021254_orig(void);
void e_1021254(void) {
    text_call_begin();
    e_1021254_orig();
    text_call_end();
}

/* the pointer: the bookkeeping only (it is drawn by the OS: overrides.c guest_pointer), except while an
   item is dragged, when the original draws it so the item follows the mouse */
extern int g_os_pointer;
void e_101a8f8_orig(void);
void e_101a8f8(void) {
    if (!g_os_pointer || (int16_t)RD16(0x0103AAD4u) != -1) { e_101a8f8_orig(); return; }   /* the item dragged, -1 = none */
    WR16(0x01143E88u, RD16(0x01143E8Eu));    /* drawn at = where the mouse handler put it */
    WR16(0x01143E8Au, RD16(0x01143E90u));
    g_ret = POP32();                          /* behave like the guest 'ret' */
}
