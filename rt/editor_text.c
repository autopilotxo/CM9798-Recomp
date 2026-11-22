/* editor_text.c - the Database Editor's text routine (CM2ED.EXE 0x21254, the game's 0x1A68C), wrapped
 * like the game's (overrides.c) so sharp text can redraw what it draws (textlayer.c). A file of its own:
 * it is only linked when gen/ has the editor (ed_*.c call it), as it needs the generated original. */
#include "rt_internal.h"
#include "textlayer.h"
void e_1021254_orig(void);
void e_1021254(void) {
    text_call_begin();
    e_1021254_orig();
    text_call_end();
}
