/* sharptext.c - redraws the game's text with the real fonts at full window resolution.
 *
 * The game's text is MS Sans Serif (MY1..MY5.FNT) and Lucida Sans (MY6/MY7.FNT) bitmap fonts at
 * 640x480. textlayer.c records every letter the game draws and works out which are still on
 * screen exactly as drawn; here those letters are erased from the 640x480 picture (filled with
 * the surrounding background) and drawn again on top of the scaled picture with the TrueType
 * versions of the same fonts: Microsoft Sans Serif (micross.ttf) and Lucida Sans Unicode
 * (l_10646.ttf), both part of Windows. Each letter stays in its original cell, so layouts don't
 * change. Anything not recognised keeps its original pixels.
 *
 * Fonts are looked for in the game folder, next to the program and in the Windows fonts folder.
 * Without them (e.g. Linux without Windows), free look-alikes are used if installed: Liberation
 * Sans or Arimo for MS Sans Serif, DejaVu Sans (and Bold) for Lucida Sans. Measured against the
 * game's own bitmap fonts their proportions are close, and measure() sizes them the same way.
 */
#include "rt_internal.h"
#include "textlayer.h"
#include "sharptext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CM_TTF
#include <SDL_ttf.h>

#define NFONTS 7
static char path_sans[1024], path_lucida[1024], path_lucida_bold[1024];
static TTF_Font *ttf[NFONTS]; static int ttf_size[NFONTS];
static float px_per_scale[NFONTS];   /* TrueType size (pixels) per unit of window scale, measured */
static struct { SDL_Texture *t; int w, h; } cache[NFONTS][256];
static int ready;

/* the fonts are Windows "ANSI" (code page 1252) */
static const uint16_t cp1252_80[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178 };
static uint16_t to_unicode(int ch) { return ch >= 0x80 && ch < 0xA0 ? cp1252_80[ch - 0x80] : (uint16_t)ch; }

static int find_font(char *out, size_t n, const char *game_dir, const char *const names[]) {
    char dirs[7][1024]; int nd = 0;
    if (game_dir) snprintf(dirs[nd++], 1024, "%s/", game_dir);
    char *base = SDL_GetBasePath(); if (base) { snprintf(dirs[nd++], 1024, "%s", base); SDL_free(base); }
    const char *windir = getenv("WINDIR"); if (windir) snprintf(dirs[nd++], 1024, "%s/Fonts/", windir);
    const char *home = getenv("HOME");
    if (home) { snprintf(dirs[nd++], 1024, "%s/.local/share/fonts/", home); snprintf(dirs[nd++], 1024, "%s/.fonts/", home); }
    snprintf(dirs[nd++], 1024, "/usr/share/fonts/TTF/");
    for (int d = 0; d < nd; d++) for (int i = 0; names[i]; i++) {
        snprintf(out, n, "%s%s", dirs[d], names[i]);
        FILE *f = fopen(out, "rb"); if (f) { fclose(f); return 1; }
    }
    out[0] = 0; return 0;
}

/* first of these files that exists */
static int find_file(char *out, size_t n, const char *const paths[]) {
    for (int i = 0; paths[i]; i++) { FILE *f = fopen(paths[i], "rb"); if (f) { fclose(f); snprintf(out, n, "%s", paths[i]); return 1; } }
    out[0] = 0; return 0;
}
/* ask fontconfig (if installed) for a family; accepted only if the file it gives is that family */
static int find_fc(char *out, size_t n, const char *pattern, const char *file_has) {
#ifndef _WIN32
    char cmd[256]; snprintf(cmd, sizeof cmd, "fc-match -f '%%{file}' '%s' 2>/dev/null", pattern);
    FILE *pp = popen(cmd, "r"); if (!pp) return 0;
    char buf[1024] = ""; size_t k = fread(buf, 1, sizeof buf - 1, pp); buf[k] = 0; pclose(pp);
    const char *base = strrchr(buf, '/'); base = base ? base + 1 : buf;
    if (buf[0] && strstr(base, file_has)) { FILE *f = fopen(buf, "rb"); if (f) { fclose(f); snprintf(out, n, "%s", buf); return 1; } }
#else
    (void)pattern; (void)file_has;
#endif
    out[0] = 0; return 0;
}
/* free stand-ins, at their usual places on Arch/CachyOS, Debian/Ubuntu, Fedora and openSUSE,
   then the ones macOS comes with */
static const char *const free_sans[] = {
    "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf",
    "/usr/share/fonts/liberation-sans-fonts/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/Arimo-Regular.ttf",
    "/usr/share/fonts/truetype/croscore/Arimo-Regular.ttf",
    "/usr/share/fonts/google-arimo/Arimo-Regular.ttf",
    "/usr/share/fonts/google-arimo-fonts/Arimo-Regular.ttf",
    "/System/Library/Fonts/Supplemental/Microsoft Sans Serif.ttf",   /* macOS ships the original */
    "/Library/Fonts/Microsoft Sans Serif.ttf",
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",                    /* SteamOS and others: Noto */
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf",
    "/usr/share/fonts/noto/NotoSans[wdth,wght].ttf",
    NULL };
static const char *const free_lucida[] = {
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/DejaVuSans.ttf",
    "/System/Library/Fonts/LucidaGrande.ttc",                        /* macOS: Lucida's sister font */
    "/System/Library/Fonts/Supplemental/Verdana.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
    NULL };
static const char *const free_lucida_bold[] = {
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/google-noto/NotoSans-Bold.ttf",
    NULL };

int sharp_init(const char *game_dir) {
    static const char *const sans[] = { "micross.ttf", "MICROSS.TTF", "Micross.ttf", NULL };
    static const char *const lucida[] = { "l_10646.ttf", "L_10646.TTF", "LSANS.TTF", "lsans.ttf", NULL };
    static const char *const lucida_bold[] = { "LSANSD.TTF", "lsansd.ttf", NULL };
    if (TTF_Init() != 0) { fprintf(stderr, "sharp text: SDL_ttf failed: %s\n", TTF_GetError()); return 0; }
    find_font(path_sans, sizeof path_sans, game_dir, sans);
    find_font(path_lucida, sizeof path_lucida, game_dir, lucida);
    find_font(path_lucida_bold, sizeof path_lucida_bold, game_dir, lucida_bold);
    /* stand-ins: by name in the game folder, next to the program and in the player's own fonts folders
       (where a SteamOS player, whose system folders can't be changed, can put them); then where the
       distributions put them; then whatever fontconfig has */
    static const char *const sans_free[] = { "LiberationSans-Regular.ttf", "Arimo-Regular.ttf", "NotoSans-Regular.ttf", NULL };
    static const char *const lucida_free[] = { "DejaVuSans.ttf", "NotoSans-Regular.ttf", NULL };
    static const char *const lucida_free_bold[] = { "DejaVuSans-Bold.ttf", "NotoSans-Bold.ttf", NULL };
    int sub_sans = 0, sub_lucida = 0;
    if (!path_sans[0] && (find_font(path_sans, sizeof path_sans, game_dir, sans_free) ||
                          find_file(path_sans, sizeof path_sans, free_sans) ||
                          find_fc(path_sans, sizeof path_sans, "Liberation Sans:style=Regular", "LiberationSans") ||
                          find_fc(path_sans, sizeof path_sans, "Arimo:style=Regular", "Arimo") ||
                          find_fc(path_sans, sizeof path_sans, "Noto Sans:style=Regular", "NotoSans") ||
                          find_fc(path_sans, sizeof path_sans, "sans-serif:style=Regular", ""))) sub_sans = 1;
    if (!path_lucida[0]) {
        int own = find_font(path_lucida, sizeof path_lucida, game_dir, lucida_free);
        if (own || find_file(path_lucida, sizeof path_lucida, free_lucida) ||
            find_fc(path_lucida, sizeof path_lucida, "DejaVu Sans:style=Book", "DejaVuSans") ||
            find_fc(path_lucida, sizeof path_lucida, "Noto Sans:style=Regular", "NotoSans")) {
            sub_lucida = 1;                /* its own bold, not Lucida's */
            if (!find_font(path_lucida_bold, sizeof path_lucida_bold, game_dir, lucida_free_bold) &&
                !find_file(path_lucida_bold, sizeof path_lucida_bold, free_lucida_bold) &&
                !find_fc(path_lucida_bold, sizeof path_lucida_bold, "DejaVu Sans:style=Bold", "DejaVuSans-Bold"))
                find_fc(path_lucida_bold, sizeof path_lucida_bold, "Noto Sans:style=Bold", "NotoSans-Bold");
        } else if (path_sans[0]) {         /* none of those: the sans stand-in for both */
            snprintf(path_lucida, sizeof path_lucida, "%s", path_sans); sub_lucida = 1;
        }
    }
    fprintf(stderr, "sharp text: MS Sans Serif -> %s%s\n", path_sans[0] ? path_sans : "(micross.ttf not found)", sub_sans ? " (stand-in)" : "");
    fprintf(stderr, "sharp text: Lucida Sans   -> %s%s\n", path_lucida[0] ? path_lucida : "(l_10646.ttf not found)", sub_lucida ? " (stand-in)" : "");
    ready = path_sans[0] != 0;
    return ready;
}
int sharp_available(void) { return ready; }

static const char *font_path(int f, int *bold) {
    *bold = 0;
    if (f < 5) return path_sans[0] ? path_sans : NULL;
    if (f == 6 && path_lucida_bold[0]) return path_lucida_bold;
    *bold = f == 6;
    return path_lucida[0] ? path_lucida : NULL;
}

static void drop_font(int f) {
    for (int c = 0; c < 256; c++) if (cache[f][c].t) { SDL_DestroyTexture(cache[f][c].t); cache[f][c].t = NULL; }
    if (ttf[f]) { TTF_CloseFont(ttf[f]); ttf[f] = NULL; }
    ttf_size[f] = 0;
}
/* Size each TrueType font to match the bitmap font it replaces, by measurement rather than its
   nominal point size (the Lucida files claim 24pt but are drawn about 18pt): the capital-H
   height must match, and the average letter must not be wider than the original's, so every
   letter fits its original cell. */
static float measure(int f) {
    struct tfont tf; int bold; const char *p = font_path(f, &bold);
    if (!p || !text_font_info(f, &tf)) return 0;
    int cap = 0, sumw = 0, nw = 0;
    if ('H' >= tf.first && 'H' <= tf.last) {
        int top = -1, bot = -1, w = text_glyph_width(&tf, 'H');
        for (int y = 0; y < tf.height; y++) for (int x = 0; x < w; x++)
            if (text_glyph_bit(&tf, 'H', x, y)) { if (top < 0) top = y; bot = y; }
        if (top >= 0) cap = bot - top + 1;
    }
    for (int c = 'A'; c <= 'z'; c++) if ((c <= 'Z' || c >= 'a') && c >= tf.first && c <= tf.last) { sumw += text_glyph_width(&tf, c); nw++; }
    TTF_Font *ref = TTF_OpenFont(p, 100); if (!ref) return 0;
    if (bold) TTF_SetFontStyle(ref, TTF_STYLE_BOLD);
    int minx, maxx, miny, maxy, adv, tcap = 0, tsum = 0;
    if (TTF_GlyphMetrics(ref, 'H', &minx, &maxx, &miny, &maxy, &adv) == 0) tcap = maxy - miny;
    for (int c = 'A'; c <= 'z'; c++) if ((c <= 'Z' || c >= 'a') && TTF_GlyphMetrics(ref, (uint16_t)c, &minx, &maxx, &miny, &maxy, &adv) == 0) tsum += adv;
    TTF_CloseFont(ref);
    float by_cap = cap && tcap ? 100.0f * cap / tcap : 0, by_width = sumw && tsum ? 100.0f * sumw / tsum * 1.03f : 0;
    float r = by_cap && by_width ? (by_cap < by_width ? by_cap : by_width) : by_cap ? by_cap : by_width;
    if (r <= 0) r = tf.points * 96 / 72.0f;
    fprintf(stderr, "sharp text: font %d sized %.1f px (cap height %d, nominal %.1f)\n", f, r, cap, tf.points * 96 / 72.0f);
    return r;
}

static TTF_Font *get_font(int f, int px) {
    if (ttf[f] && ttf_size[f] == px) return ttf[f];
    drop_font(f);
    int bold; const char *p = font_path(f, &bold); if (!p) return NULL;
    ttf[f] = TTF_OpenFont(p, px);
    if (!ttf[f]) return NULL;
    if (bold) TTF_SetFontStyle(ttf[f], TTF_STYLE_BOLD);
    ttf_size[f] = px;
    return ttf[f];
}

/* this frame's letters to redraw */
static int vis[8192], nvis;

static int drawable(int font) { int b; return ready && font >= 0 && font < NFONTS && font_path(font, &b); }

/* The letters' original pixels are filled in from the background around them: the nearest pixel
   that isn't a letter's, left, right, up and down, blended by nearness. (Copying just the nearest
   one along the row smeared whatever was beside a letter across it: a light pixel of the stadium
   next to a black shadow became a white line the shape of the shadow's stroke, showing wherever
   the TrueType letter doesn't cover the old one exactly.) */
void sharp_erase(uint32_t *rgb, const uint8_t *p, int w, int h, int st, const uint32_t lut[256]) {
    nvis = 0;
    if (!ready) return;
    int n = text_visible(p, st, w, h, vis, 8192);
    for (int k = 0; k < n; k++) {
        struct tglyph *g = &g_glyphs[vis[k]];
        if (!drawable(g->font)) continue;
        vis[nvis++] = vis[k];
        for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++) {
            if (!text_is_ink(vis[k], x, y)) continue;
            int sx = g->x + g_text_dx[vis[k]] + x, sy = g->y + g_text_dy[vis[k]] + y;
            if (sx < 0 || sx >= w || sy < 0 || sy >= h) continue;
            static const int dir[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
            float r = 0, gr = 0, b = 0, wt = 0;
            for (int d4 = 0; d4 < 4; d4++) {
                int lim = dir[d4][0] ? g->w + 2 : g->h + 2;
                for (int d = 1; d <= lim; d++) {
                    int bx = sx + dir[d4][0] * d, by = sy + dir[d4][1] * d;
                    if (bx < 0 || bx >= w || by < 0 || by >= h) break;
                    if (g_text_cover[by][bx]) continue;
                    uint32_t c = lut[p[by * st + bx]]; float f = 1.0f / (float)d;
                    r += f * (float)((c >> 16) & 255); gr += f * (float)((c >> 8) & 255); b += f * (float)(c & 255); wt += f;
                    break;
                }
            }
            if (wt > 0) rgb[sy * w + sx] = (rgb[sy * w + sx] & 0xFF000000u) | ((uint32_t)(r / wt + 0.5f) << 16) | ((uint32_t)(gr / wt + 0.5f) << 8) | (uint32_t)(b / wt + 0.5f);
        }
    }
}

/* draw this frame's letters over the picture; the 640x480 picture fills (ox, oy, 640k x 480k) */
void sharp_draw(SDL_Renderer *ren, const uint8_t pal[256][3], float k, int ox, int oy) {
    if (!nvis) return;
    for (int i = 0; i < nvis; i++) {
        struct tglyph *g = &g_glyphs[vis[i]]; struct tfont tf;
        if (g->ch == ' ' || !text_font_info(g->font, &tf)) continue;
        if (!px_per_scale[g->font]) px_per_scale[g->font] = measure(g->font);
        int px = (int)(px_per_scale[g->font] * k + 0.5f); if (px < 4) px = 4;
        TTF_Font *font = get_font(g->font, px); if (!font) continue;
        if (!cache[g->font][g->ch].t) {
            SDL_Color white = { 255, 255, 255, 255 };
            SDL_Surface *s = TTF_RenderGlyph_Blended(font, to_unicode(g->ch), white);
            if (!s) continue;
            cache[g->font][g->ch].t = SDL_CreateTextureFromSurface(ren, s);
            cache[g->font][g->ch].w = s->w; cache[g->font][g->ch].h = s->h;
            SDL_FreeSurface(s);
            if (!cache[g->font][g->ch].t) continue;
            SDL_SetTextureBlendMode(cache[g->font][g->ch].t, SDL_BLENDMODE_BLEND);
        }
        SDL_Texture *t = cache[g->font][g->ch].t;
        const uint8_t *c = pal[g->colour];
        uint32_t tint = g_text_tint[vis[i]];
        if (tint) SDL_SetTextureColorMod(t, (tint >> 16) & 255, (tint >> 8) & 255, tint & 255);   /* recoloured */
        else if (g_text_dim[vis[i]]) SDL_SetTextureColorMod(t, c[0] * 2 / 5, c[1] * 2 / 5, c[2] * 2 / 5);   /* greyed out */
        else SDL_SetTextureColorMod(t, c[0], c[1], c[2]);
        /* same baseline as the bitmap letter, centred in its original cell */
        int gx = g->x + g_text_dx[vis[i]], gy = g->y + g_text_dy[vis[i]];   /* where it is on screen now */
        int base = oy + (int)((gy + tf.ascent) * k + 0.5f);
        SDL_Rect d;
        d.w = cache[g->font][g->ch].w; d.h = cache[g->font][g->ch].h;
        d.x = ox + (int)(gx * k + (g->w * k - d.w) / 2 + 0.5f);
        d.y = base - TTF_FontAscent(font);
        SDL_RenderCopy(ren, t, NULL, &d);
    }
}

/* a short note in the bottom-right corner of the game picture, e.g. "Autosaved - ..." */
void sharp_draw_note(SDL_Renderer *ren, const char *text, uint8_t alpha, float k, int ox, int oy, int pw, int ph) {
    static TTF_Font *f; static int fsize;
    int px = (int)(13 * k + 0.5f);
    if (!f || fsize != px) { if (f) TTF_CloseFont(f); f = path_sans[0] ? TTF_OpenFont(path_sans, px) : NULL; fsize = px; }
    if (!f) return;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *s = TTF_RenderUTF8_Blended(f, text, white); if (!s) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    int tw = s->w, th = s->h; SDL_FreeSurface(s);
    if (!t) return;
    int pad = (int)(6 * k), margin = (int)(8 * k);
    SDL_Rect box = { ox + pw - margin - tw - 2 * pad, oy + ph - (int)(40 * k) - th - 2 * pad, tw + 2 * pad, th + 2 * pad };
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, (uint8_t)(alpha * 3 / 4));
    SDL_RenderFillRect(ren, &box);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, (uint8_t)(alpha / 3));
    SDL_RenderDrawRect(ren, &box);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(t, alpha);
    SDL_Rect d = { box.x + pad, box.y + pad, tw, th };
    SDL_RenderCopy(ren, t, NULL, &d);
    SDL_DestroyTexture(t);
}


/* ---- labels for the port's own screens (options menu) ----
   align: 0 = left, 1 = centred. Drawn with Microsoft Sans Serif; cached, since menus redraw often. */
static struct { char text[160]; int px, bold; SDL_Texture *t; int w, h; uint32_t used; } lcache[64];
static uint32_t lclock;
static SDL_Texture *label_texture(SDL_Renderer *ren, const char *text, int px, int bold, int *w, int *h) {
    int free_i = 0;
    for (int i = 0; i < 64; i++) {
        if (lcache[i].t && lcache[i].px == px && lcache[i].bold == bold && !strcmp(lcache[i].text, text)) {
            lcache[i].used = ++lclock; *w = lcache[i].w; *h = lcache[i].h; return lcache[i].t; }
        if (lcache[i].used < lcache[free_i].used) free_i = i;
    }
    static TTF_Font *f[2]; static int fpx[2];
    if (!f[bold] || fpx[bold] != px) {
        if (f[bold]) TTF_CloseFont(f[bold]);
        f[bold] = path_sans[0] ? TTF_OpenFont(path_sans, px) : NULL; fpx[bold] = px;
        if (f[bold] && bold) TTF_SetFontStyle(f[bold], TTF_STYLE_BOLD);
    }
    if (!f[bold]) return NULL;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *s = TTF_RenderUTF8_Blended(f[bold], text, white); if (!s) return NULL;
    SDL_Texture *t = SDL_CreateTextureFromSurface(ren, s);
    if (lcache[free_i].t) SDL_DestroyTexture(lcache[free_i].t);
    snprintf(lcache[free_i].text, sizeof lcache[free_i].text, "%s", text);
    lcache[free_i].px = px; lcache[free_i].bold = bold; lcache[free_i].t = t;
    lcache[free_i].w = *w = s->w; lcache[free_i].h = *h = s->h; lcache[free_i].used = ++lclock;
    SDL_FreeSurface(s);
    if (t) SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    return t;
}
int sharp_label(SDL_Renderer *ren, const char *text, int x, int y, int w, int h, int px, SDL_Color c, int bold, int align) {
    int tw, th; SDL_Texture *t = ready ? label_texture(ren, text, px, bold, &tw, &th) : NULL;
    if (!t) return 0;
    SDL_SetTextureColorMod(t, c.r, c.g, c.b); SDL_SetTextureAlphaMod(t, c.a);
    SDL_Rect d = { align ? x + (w - tw) / 2 : x, y + (h - th) / 2, tw, th };
    SDL_RenderCopy(ren, t, NULL, &d);
    return 1;
}

#else   /* built without SDL_ttf: original text only */
int sharp_label(SDL_Renderer *ren, const char *text, int x, int y, int w, int h, int px, SDL_Color c, int bold, int align) {
    (void)ren; (void)text; (void)x; (void)y; (void)w; (void)h; (void)px; (void)c; (void)bold; (void)align; return 0; }
void sharp_draw_note(SDL_Renderer *ren, const char *text, uint8_t alpha, float k, int ox, int oy, int pw, int ph) {
    (void)ren; (void)text; (void)alpha; (void)k; (void)ox; (void)oy; (void)pw; (void)ph; }
int  sharp_init(const char *game_dir) { (void)game_dir; return 0; }
int  sharp_available(void) { return 0; }
void sharp_erase(uint32_t *rgb, const uint8_t *p, int w, int h, int st, const uint32_t lut[256]) { (void)rgb; (void)p; (void)w; (void)h; (void)st; (void)lut; }
void sharp_draw(SDL_Renderer *ren, const uint8_t pal[256][3], float k, int ox, int oy) { (void)ren; (void)pal; (void)k; (void)ox; (void)oy; }
#endif
