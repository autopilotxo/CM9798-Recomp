/* textlayer.h - letters the game has drawn, for redrawing them sharp (see textlayer.c) */
#pragma once
#include <stdint.h>

struct tglyph { int16_t x, y; uint8_t w, h, ch, font, colour, live;
                uint8_t bx0, by0, bx1, by1; };   /* area checked: the ink plus 1 pixel, within the cell */
struct tfont  { uint32_t base; int points, ascent, height, first, last; };

extern struct tglyph g_glyphs[];
int  text_font_info(int font, struct tfont *f);        /* font 0..6 = MY1..MY7.FNT */
int  text_glyph_bit(const struct tfont *f, int ch, int x, int y);
int  text_glyph_width(const struct tfont *f, int ch);
int  text_visible(const uint8_t *screen, int stride, int w, int h, int *out, int max);
int  text_find(const uint8_t *screen, int stride, const char *s, int *x0, int *y0, int *x1, int *y1);
int  text_find_glyphs(const uint8_t *screen, int stride, const char *s, int *x0, int *y0, int *x1, int *y1, int *idx, int max);
extern uint8_t g_text_dim[];          /* glyph drawn dimmed this frame (greyed-out choice) */
extern uint32_t g_text_tint[];        /* glyph drawn in this colour this frame (0xFFRRGGBB; 0 = its own) */
void text_dim_clear(void);            /* clears both */
int  text_is_ink(int i, int x, int y);     /* pixel of glyph i's cell is its ink */
extern uint8_t g_text_cover[480][640];
extern int8_t g_text_dx[], g_text_dy[];      /* after text_visible: glyph found moved by this much */   /* after text_visible: ink of the letters it returned */

void hook_text_glyph(void);       /* patches.txt */
void text_call_begin(void);       /* around the game's text routine (overrides.c) */
void text_call_end(void);
void text_reset(void);            /* forget every letter */
void text_use_editor(int editor); /* the Database Editor's addresses (rt/editor.c), or the game's */
