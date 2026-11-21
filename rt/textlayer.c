/* textlayer.c - records every letter the game draws, so the window backend can redraw it sharp.
 *
 * The game draws all text through one routine (0x1A68C): for each character it copies the
 * glyph's set pixels, in one colour, from a Windows bitmap font (MY1..MY7.FNT, i.e. MS Sans
 * Serif 8-18pt and Lucida Sans 24pt) into its off-screen copy of the screen (pointer at
 * 0xA47680, 640 bytes per row), which is then copied to the display.
 *
 * patches.txt calls hook_text_glyph() as each character is about to be drawn (character in
 * EAX, pen position in 0xADFE62/64, font index in 0x0F41E4, colour in 0xADFE5C). The routine
 * itself is overridden (rt/overrides.c) so that, once the whole string is drawn, each new
 * glyph's cell is snapshotted exactly as the game left it.
 *
 * At display time text_visible() keeps only glyphs whose cells still look exactly like that
 * snapshot, apart from pixels covered by newer letters (drop shadows, overlapping lines).
 * Anything else the game drew over a letter since (a new screen, a scrolled list, a sprite)
 * makes it fall back to the original pixels, so sharp text can never be left behind.
 */
#include "rt_internal.h"
#include "textlayer.h"
#include <string.h>

#define A(x) ((uint32_t)(x) + 0x01000000u)
#define TXT_BUFPTR   A(0xA47680)   /* dword: off-screen screen copy the text is drawn into */
#define TXT_FONTS    A(0xA476D8)   /* dword[7]: loaded MYn.FNT images */
#define TXT_FONTIDX  A(0x0F41E4)   /* word: current font 0..6 */
#define TXT_PEN_X    A(0xADFE62)   /* word */
#define TXT_PEN_Y    A(0xADFE64)   /* word */
#define TXT_COLOUR   A(0xADFE5C)   /* byte: palette index */

#define SW 640
#define SH 480
#define MAX_GLYPHS 8192
#define CELL_MAX   (40 * 40)
#define STALE_PRESENTS 600           /* drop letters not seen on screen for this many frames */

/* live: 0 = gone, 1 = drawn in the current call (no snapshot yet), 2 = live */
struct tglyph g_glyphs[MAX_GLYPHS];
static uint8_t snaps[MAX_GLYPHS][CELL_MAX];
static uint8_t inkmask[MAX_GLYPHS][CELL_MAX];
static uint32_t seen[MAX_GLYPHS];
static uint32_t callid[MAX_GLYPHS], calls;          /* which call of the text routine drew it */
int g_nglyphs;
static int row_head[SH], row_next[MAX_GLYPHS];   /* glyphs by top row, for same-line replacement */
static int rows_ready;
static int recording;
static uint32_t call_buf, present_no;
uint8_t g_text_cover[SH][SW];                    /* this frame: pixel is ink of a shown letter */
int8_t g_text_dx[MAX_GLYPHS], g_text_dy[MAX_GLYPHS]; /* this frame: where it was found, relative to where drawn */
#define SHIFT_MAX     3              /* a held button copies its picture 2 pixels right and down */
#define SHIFT_RECENT  30             /* only look for letters that were on screen moments ago */

int text_font_info(int font, struct tfont *f) {
    if (font < 0 || font > 6) return 0;
    uint32_t base = RD32(TXT_FONTS + 4 * font);
    if (!base || base >= 0x4000000u) return 0;
    f->base = base;
    f->points = (int)RD16(base + 0x44);
    f->ascent = (int)RD16(base + 0x4A);
    f->height = (int)RD16(base + 0x58);
    f->first = (int)RD8(base + 0x5F);
    f->last = (int)RD8(base + 0x60);
    return f->height > 0 && f->height <= 40;
}

/* glyph pixel (x, y) set? (Windows 2.x FNT: 8-pixel-wide byte columns, each `height` rows) */
int text_glyph_bit(const struct tfont *f, int ch, int x, int y) {
    uint32_t e = f->base + 0x76 + 4 * (uint32_t)(ch - f->first);
    uint32_t off = RD16(e + 2);
    uint8_t b = RD8(f->base + off + (uint32_t)(x >> 3) * (uint32_t)f->height + (uint32_t)y);
    return (b >> (7 - (x & 7))) & 1;
}
int text_glyph_width(const struct tfont *f, int ch) {
    return (int)RD16(f->base + 0x76 + 4 * (uint32_t)(ch - f->first));
}

static void rebuild_rows(void) {
    for (int y = 0; y < SH; y++) row_head[y] = -1;
    for (int i = g_nglyphs - 1; i >= 0; i--) { row_next[i] = row_head[g_glyphs[i].y]; row_head[g_glyphs[i].y] = i; }
    rows_ready = 1;
}
static void compact(void) {
    int n = 0;
    for (int i = 0; i < g_nglyphs; i++) {
        struct tglyph *g = &g_glyphs[i];
        if (!g->live || (g->live == 2 && present_no - seen[i] > STALE_PRESENTS)) continue;
        if (n != i) {
            g_glyphs[n] = *g; seen[n] = seen[i]; callid[n] = callid[i];
            memcpy(snaps[n], snaps[i], (size_t)g->w * g->h); memcpy(inkmask[n], inkmask[i], (size_t)g->w * g->h);
        }
        n++;
    }
    g_nglyphs = n;
    rebuild_rows();
}

/* the routine is about to draw character EAX at the pen position */
void hook_text_glyph(void) {
    if (!recording) return;
    struct tfont f;
    int font = (int16_t)RD16(TXT_FONTIDX), ch = (int)(eax & 0xFF);
    if (!text_font_info(font, &f) || ch < f.first || ch > f.last) return;
    int x = (int16_t)RD16(TXT_PEN_X), y = (int16_t)RD16(TXT_PEN_Y), w = text_glyph_width(&f, ch), h = f.height;
    if (w <= 0 || w * h > CELL_MAX || x < 0 || y < 0 || x + w > SW || y + h > SH) return;
    if (!rows_ready) rebuild_rows();
    if (g_nglyphs >= MAX_GLYPHS) { compact(); if (g_nglyphs >= MAX_GLYPHS) return; }
    /* text drawn on the same line in the same font replaces the letters it overlaps */
    for (int j = row_head[y]; j >= 0; j = row_next[j]) {
        struct tglyph *o = &g_glyphs[j];
        if (o->live && o->font == font && o->x < x + w && x < o->x + o->w) o->live = 0;
    }
    int i = g_nglyphs++;
    struct tglyph *g = &g_glyphs[i];
    g->x = (int16_t)x; g->y = (int16_t)y; g->w = (uint8_t)w; g->h = (uint8_t)h;
    g->ch = (uint8_t)ch; g->font = (uint8_t)font; g->colour = (uint8_t)RD8(TXT_COLOUR);
    g->live = 1;
    seen[i] = present_no; callid[i] = calls;
    row_next[i] = row_head[y]; row_head[y] = i;
}

/* wrapped around the game's text routine (overrides.c) */
static int call_first;
void text_call_begin(void) {
    call_buf = RD32(TXT_BUFPTR);
    /* only text drawn into the full-screen buffer maps to screen positions */
    recording = call_buf != 0 && call_buf + SW * SH <= 0x4000000u;
    if (recording && g_nglyphs > MAX_GLYPHS * 3 / 4) compact();
    call_first = g_nglyphs; calls++;
}
void text_call_end(void) {
    if (!recording) return;
    recording = 0;
    for (int i = call_first; i < g_nglyphs; i++) {
        struct tglyph *g = &g_glyphs[i]; struct tfont f;
        if (g->live != 1) continue;
        if (!text_font_info(g->font, &f)) { g->live = 0; continue; }
        int x0 = g->w, y0 = g->h, x1 = -1, y1 = -1;
        for (int y = 0; y < g->h; y++) {
            const uint8_t *src = M + call_buf + (uint32_t)(g->y + y) * SW + (uint32_t)g->x;
            memcpy(snaps[i] + y * g->w, src, g->w);
            for (int x = 0; x < g->w; x++) {
                uint8_t ink = (uint8_t)(text_glyph_bit(&f, g->ch, x, y) && src[x] == g->colour);
                inkmask[i][y * g->w + x] = ink;
                if (ink) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
            }
        }
        if (x1 < 0) { g->live = 0; continue; }                  /* a space: nothing to redraw */
        /* only the ink and a 1-pixel border must stay unchanged: other drawing over the empty
           parts of a tall cell (a tab over the top of a score box) doesn't matter */
        g->bx0 = (uint8_t)(x0 > 0 ? x0 - 1 : 0); g->by0 = (uint8_t)(y0 > 0 ? y0 - 1 : 0);
        g->bx1 = (uint8_t)(x1 < g->w - 1 ? x1 + 1 : x1); g->by1 = (uint8_t)(y1 < g->h - 1 ? y1 + 1 : y1);
        g->live = 2;
    }
}

/* glyph i's checked area, found at its position moved by (dx, dy)? */
static int match_at(int i, const uint8_t *screen, int stride, int dx, int dy) {
    struct tglyph *g = &g_glyphs[i]; const uint8_t *s = snaps[i];
    int X = g->x + dx, Y = g->y + dy;
    if (X + g->bx0 < 0 || Y + g->by0 < 0 || X + g->bx1 >= SW || Y + g->by1 >= SH) return 0;
    for (int y = g->by0; y <= g->by1; y++) {
        const uint8_t *row = screen + (size_t)(Y + y) * stride + X;
        if (!memcmp(row + g->bx0, s + y * g->w + g->bx0, (size_t)(g->bx1 - g->bx0 + 1))) continue;
        for (int x = g->bx0; x <= g->bx1; x++)                      /* differences only under newer ink? */
            if (row[x] != s[y * g->w + x] && !g_text_cover[Y + y][X + x]) return 0;
    }
    return 1;
}

/* Letters to redraw this frame, oldest first (so text is drawn over its own shadow).
   Also fills g_text_cover with the ink of every letter returned.
   A letter that isn't where it was drawn may have moved with its picture (a held button is copied
   2 pixels right and down), so the nearest positions are tried too. But a thin letter ("I", "t",
   "(") also matches some other stroke a pixel or two away, and a sharp copy drawn there is stray
   text on screen (a second, offset shadow behind a team name). So a moved letter counts only if:
   no newer letter covering it is still in place (a shadow stays under its text), and the other
   letters drawn with it (the same word) moved the same way - none in place, and at least one
   other alike, unless it is the only letter. */
static uint8_t in_place[SH][SW];
static void mark(uint8_t m[SH][SW], int i, int dx, int dy) {
    struct tglyph *g = &g_glyphs[i];
    for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++)
        if (inkmask[i][y * g->w + x]) {
            int X = g->x + dx + x, Y = g->y + dy + y;
            if (X >= 0 && X < SW && Y >= 0 && Y < SH) m[Y][X] = 1;
        }
}
static int under_placed(int i) {          /* does a newer letter still in place cover part of its cell? */
    struct tglyph *g = &g_glyphs[i];
    for (int y = g->by0; y <= g->by1; y++) for (int x = g->bx0; x <= g->bx1; x++)
        if (in_place[g->y + y][g->x + x]) return 1;
    return 0;
}
int text_visible(const uint8_t *screen, int stride, int w, int h, int *out, int max) {
    present_no++;
    if (w != SW || h != SH) return 0;
    memset(g_text_cover, 0, sizeof g_text_cover);
    memset(in_place, 0, sizeof in_place);
    int n = 0;
    for (int i = g_nglyphs - 1; i >= 0 && n < max; i--) {        /* newest first */
        struct tglyph *g = &g_glyphs[i];
        if (g->live != 2) continue;
        int dx = 0, dy = 0, ok = match_at(i, screen, stride, 0, 0);
        /* not where it was drawn: the game may have moved the picture under it (held button);
           search outwards, nearest positions first */
        if (!ok && present_no - seen[i] <= SHIFT_RECENT && !under_placed(i))
            for (int r = 1; r <= SHIFT_MAX && !ok; r++)
                for (int ty = -r; ty <= r && !ok; ty++)
                    for (int tx = -r; tx <= r && !ok; tx++)
                        if ((tx == -r || tx == r || ty == -r || ty == r) && match_at(i, screen, stride, tx, ty)) {
                            ok = 1; dx = tx; dy = ty;
                        }
        if (!ok) continue;
        g_text_dx[i] = (int8_t)dx; g_text_dy[i] = (int8_t)dy;
        if (!dx && !dy) mark(in_place, i, 0, 0);
        mark(g_text_cover, i, dx, dy);
        out[n++] = i;
    }
    /* moved letters must agree with the rest of their word (a call's letters are next to each other) */
    int kept = 0, dropped = 0;
    for (int a = 0; a < n; ) {
        int b = a; while (b < n && callid[out[b]] == callid[out[a]]) b++;
        int all = 0;                                              /* the call's letters still recorded */
        for (int i = out[a]; i >= 0 && callid[i] == callid[out[a]]; i--) if (g_glyphs[i].live == 2) all++;
        for (int i = out[a] + 1; i < g_nglyphs && callid[i] == callid[out[a]]; i++) if (g_glyphs[i].live == 2) all++;
        int placed = 0;
        for (int k = a; k < b; k++) if (!g_text_dx[out[k]] && !g_text_dy[out[k]]) placed++;
        for (int k = a; k < b; k++) {
            int i = out[k], dx = g_text_dx[i], dy = g_text_dy[i], alike = 0;
            if (dx || dy) {
                for (int q = a; q < b; q++) if (g_text_dx[out[q]] == dx && g_text_dy[out[q]] == dy) alike++;
                if (placed || (alike < 2 && all > 1)) { dropped++; continue; }
            }
            out[kept++] = i;
        }
        a = b;
    }
    n = kept;
    if (dropped) {                                                /* the cover without them */
        memset(g_text_cover, 0, sizeof g_text_cover);
        for (int k = 0; k < n; k++) mark(g_text_cover, out[k], g_text_dx[out[k]], g_text_dy[out[k]]);
    }
    for (int k = 0; k < n; k++) seen[out[k]] = present_no;
    for (int a = 0, b = n - 1; a < b; a++, b--) { int t = out[a]; out[a] = out[b]; out[b] = t; }
    return n;
}

int text_is_ink(int i, int x, int y) { return inkmask[i][y * g_glyphs[i].w + x]; }

/* Is `s` (spaces ignored) on screen right now, as drawn by the game? Gives its box.
   Used to recognise a screen by its text, e.g. the match screen by "Attacking Possession". */
static int ink_on_screen(int i, const uint8_t *screen, int stride) {
    struct tglyph *g = &g_glyphs[i];
    for (int y = g->by0; y <= g->by1; y++) for (int x = g->bx0; x <= g->bx1; x++)
        if (inkmask[i][y * g->w + x] && screen[(size_t)(g->y + y) * stride + g->x + x] != g->colour) return 0;
    return 1;
}
int text_find(const uint8_t *screen, int stride, const char *s, int *x0, int *y0, int *x1, int *y1) {
    return text_find_glyphs(screen, stride, s, x0, y0, x1, y1, NULL, 0);
}
/* as text_find, also giving the glyphs (indices into g_glyphs) of the match, at most max */
int text_find_glyphs(const uint8_t *screen, int stride, const char *s, int *x0, int *y0, int *x1, int *y1, int *idx, int max) {
    char want[128]; int n = 0, found = 0;
    for (; *s && n < 127; s++) if (*s != ' ') want[n++] = *s;
    if (!n) return 0;
    for (int i = 0; i < g_nglyphs; i++) {
        if (g_glyphs[i].live != 2 || g_glyphs[i].ch != (uint8_t)want[0]) continue;
        int k = 0, j = i, ok = 1, bx0 = 9999, by0 = 9999, bx1 = -1, by1 = -1;
        for (; j < g_nglyphs && k < n; j++) {
            struct tglyph *g = &g_glyphs[j];
            if (g->live != 2) continue;
            if (g->ch != (uint8_t)want[k] || g->y != g_glyphs[i].y || !ink_on_screen(j, screen, stride)) { ok = 0; break; }
            if (g->x < bx0) bx0 = g->x; if (g->x + g->w > bx1) bx1 = g->x + g->w;
            if (g->y + g->by0 < by0) by0 = g->y + g->by0; if (g->y + g->by1 > by1) by1 = g->y + g->by1;
            k++;
        }
        if (ok && k == n) {                                    /* keep the last (text over its shadow) */
            *x0 = bx0; *y0 = by0; *x1 = bx1; *y1 = by1; found = k;
            if (idx) { int m = 0; for (int q = i; q < j && m < max; q++) if (g_glyphs[q].live == 2) idx[m++] = q; found = m; }
        }
    }
    return found;
}

/* letters to draw dimmed this frame (greyed-out choices); cleared by text_dim_clear() */
uint8_t g_text_dim[MAX_GLYPHS];
uint32_t g_text_tint[MAX_GLYPHS];
void text_dim_clear(void) { memset(g_text_dim, 0, (size_t)g_nglyphs); memset(g_text_tint, 0, sizeof(uint32_t) * (size_t)g_nglyphs); }
