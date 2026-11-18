/* sharptext.h - full-resolution text drawn with the game's real fonts (see sharptext.c) */
#pragma once
#include <stdint.h>
#include <SDL.h>

int  sharp_init(const char *game_dir);   /* finds the fonts; 0 = not available */
int  sharp_available(void);
/* erase the letters that will be redrawn from the 640x480 picture (before it is uploaded) */
void sharp_erase(uint32_t *rgb, const uint8_t *p, int w, int h, int st, const uint32_t lut[256]);
/* draw them over the scaled picture, which covers (ox, oy) .. + 640k x 480k output pixels */
void sharp_draw(SDL_Renderer *ren, const uint8_t pal[256][3], float k, int ox, int oy);
/* a short note in the bottom-right corner of the picture (pw x ph output pixels at ox, oy) */
void sharp_draw_note(SDL_Renderer *ren, const char *text, uint8_t alpha, float k, int ox, int oy, int pw, int ph);
/* a label for the port's own screens, in output pixels; 0 if the font isn't available */
int sharp_label(SDL_Renderer *ren, const char *text, int x, int y, int w, int h, int px, SDL_Color c, int bold, int align);
