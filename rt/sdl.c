/* sdl.c - SDL2 window, mouse and keyboard backend for the recompiled CM 97/98.
 *
 * The game draws into the emulated 640x480x8 VESA framebuffer; this file shows it in a
 * resizable window (aspect-correct), and feeds mouse/keyboard to input.c.
 *
 * Input is collected from SDL at any time (so the window stays responsive) but handed to
 * the game only at its input polls (host_poll), so a session can be recorded and replayed
 * exactly: CM_RECORD=<file> writes "<poll> <event>" lines that headless.c can replay.
 *
 * F1 opens the options menu (all of the settings below; the game pauses while it is open).
 * Display: F11 or Alt+Enter toggles fullscreen; F12 cycles the scaling mode (integer ->
 * nearest -> smooth); F10 switches between sharp and original text (sharptext.c). All are
 * remembered in cm9798.cfg next to the program.
 *
 * Env: CM_INPUTLOG=1 (print input), CM_RECORD=<file> (record session, deterministic clock).
 */
#ifndef SDL_MAIN_HANDLED
#define SDL_MAIN_HANDLED
#endif
#include "rt_internal.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif
#include <string.h>
#include "icon_data.h"
#include "database.h"
#include "sound.h"
#include "sharptext.h"
#include "textlayer.h"
#include "backup.h"
#include "autosave.h"

int video_frame(const uint8_t **pixels, int *w, int *h, int *stride, uint8_t pal[256][3]);
void input_key(uint16_t k); void input_mouse_move(int x, int y); void input_mouse_button(int b, int down);
void input_dispatch_mouse(void); int input_key_ready(void);
extern unsigned long g_polls;
extern volatile uint32_t g_int_count; extern uint32_t g_trace_ring[256]; extern unsigned g_trace_pos;

static SDL_Window *win; static SDL_Renderer *ren; static SDL_Texture *tex;
static int tex_w, tex_h; static uint32_t last_present, last_pump, start_ticks;
static int inputlog; static FILE *rec;
static uint32_t rgb[1024 * 768];

/* ---------------- scaling / fullscreen settings ----------------
   integer: largest whole-number multiple that fits (640x480 -> 1920x1440 on a 1440p screen),
            sharp square pixels, black borders around it
   nearest: fills the window keeping 4:3, sharp but pixels may be uneven sizes
   smooth:  fills the window keeping 4:3, bilinear filtered (the old behaviour) */
enum { SCALE_INTEGER, SCALE_NEAREST, SCALE_SMOOTH, SCALE_MODES };
static const char *scale_names[SCALE_MODES] = { "integer", "nearest", "smooth" };
static int scale_mode = SCALE_INTEGER, want_fullscreen = 0, match_speed = 1, text_sharp = 1;
static char cfg_path[1024];
extern int g_show_stars, g_stat_colours, g_star_mode;
extern int g_player_size;                     /* overrides.c: players a new game keeps (0-2) */
static const char *const player_size_names[3] = { "small", "medium", "full" };

static void cfg_load(void) {
    extern const char *g_game_dir;                /* settings live in the game folder, next to CM2E16.EXE */
    snprintf(cfg_path, sizeof cfg_path, "%s/cm9798.cfg", g_game_dir);
    FILE *f = fopen(cfg_path, "r"); if (!f) return;
    char line[128], v[64];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " scale = %63s", v) == 1)
            for (int i = 0; i < SCALE_MODES; i++) if (!strcmp(v, scale_names[i])) scale_mode = i;
        if (sscanf(line, " fullscreen = %63s", v) == 1) want_fullscreen = atoi(v) != 0;
        if (sscanf(line, " fast_matches = %63s", v) == 1) match_speed = atoi(v) ? 3 : 1;   /* older setting */
        if (sscanf(line, " match_speed = %63s", v) == 1) { match_speed = atoi(v); if (match_speed < 1 || match_speed > 3) match_speed = 1; }
        if (sscanf(line, " text = %63s", v) == 1) text_sharp = strcmp(v, "original") != 0;
        if (sscanf(line, " backups = %63s", v) == 1) g_backup_keep = atoi(v);
        if (sscanf(line, " database = %63s", v) == 1) db_set_choice(v);
        if (sscanf(line, " autosave = %63s", v) == 1) g_autosave = !strcmp(v, "on") || !strcmp(v, "1");
        if (sscanf(line, " stars = %63s", v) == 1) { g_show_stars = strcmp(v, "off") != 0 && strcmp(v, "0") != 0;
            g_star_mode = !g_show_stars ? 0 : !strcmp(v, "overall") ? 1 : 2; }
        if (sscanf(line, " stat_colours = %63s", v) == 1) g_stat_colours = strcmp(v, "off") != 0 && strcmp(v, "0") != 0;
        if (sscanf(line, " sound_volume = %63s", v) == 1) { g_sound_volume = atoi(v); if (g_sound_volume < 0) g_sound_volume = 0; if (g_sound_volume > 100) g_sound_volume = 100; }
        if (sscanf(line, " sound = %63s", v) == 1) g_sound = strcmp(v, "off") != 0 && strcmp(v, "0") != 0;
        if (sscanf(line, " backgrounds = %63s", v) == 1) g_cm2_backgrounds = !SDL_strcasecmp(v, "cm2");
        if (sscanf(line, " players = %63s", v) == 1) for (int i = 0; i < 3; i++) if (!SDL_strcasecmp(v, player_size_names[i])) g_player_size = i;
    }
    fclose(f);
}
static void cfg_save(void) {
    FILE *f = fopen(cfg_path, "w"); if (!f) return;
    fprintf(f, "# Championship Manager 97/98 settings (F11 = fullscreen, F12 = scaling)\n");
    fprintf(f, "# scale = integer | nearest | smooth\nscale = %s\nfullscreen = %d\n", scale_names[scale_mode], want_fullscreen);
    fprintf(f, "# match commentary speed: 1 = normal, 2 = double, 3 = triple (buttons on the match screen, or Space)\nmatch_speed = %d\n", match_speed);
    fprintf(f, "# text: sharp (the real fonts at full resolution) | original (the 640x480 bitmap fonts); F10 switches\ntext = %s\n",
            text_sharp ? "sharp" : "original");
    fprintf(f, "# save backups kept per slot (in the game folder's backups folder); 0 = off\nbackups = %d\n", g_backup_keep);
    fprintf(f, "# database for new games: a folder under databases/ (Change Database on the Options screen)\ndatabase = %s\n", db_choice());
    fprintf(f, "# autosave at the start of every in-game week, into autosaves/slotN (on | off)\nautosave = %s\n", g_autosave ? "on" : "off");
    fprintf(f, "# player screen: star rating in the position bar - club (how good he'd be for your club, like the newer\n"
               "# Football Manager games) | overall | off - and stats coloured by how good they are (on | off)\n"
               "stars = %s\nstat_colours = %s\n", !g_show_stars ? "off" : g_star_mode == 1 ? "overall" : "club", g_stat_colours ? "on" : "off");
    fprintf(f, "# backgrounds: 9798 (97/98's pictures) | cm2 (Championship Manager 2's: STADPIC1-5 in turn, a\n"
               "# new one each day, and the clubs' own pictures; from a CM2 database's pictures folder)\n"
               "backgrounds = %s\n", g_cm2_backgrounds ? "cm2" : "9798");
    fprintf(f, "# match sounds: CM2's crowd, cheers and whistles (its SOUNDS folder, copied by the setup) (on | off), and their volume (0-100)\nsound = %s\nsound_volume = %d\n", g_sound ? "on" : "off", g_sound_volume);
    fprintf(f, "# players a new game keeps (the slider on Choose Leagues to Run): small (9900, as the original) |\n"
               "# medium (14000) | full (20000: the whole 97/98 database) - with five or more leagues\nplayers = %s\n",
            player_size_names[g_player_size >= 0 && g_player_size <= 2 ? g_player_size : 2]);
    fclose(f);
}
void host_set_database(int start_year) { (void)start_year; cfg_save(); }
int  host_match_speed(void) { return match_speed; }
void host_set_match_speed(int speed) { if (speed >= 1 && speed <= 3 && speed != match_speed) { match_speed = speed; cfg_save(); } }

static void update_title(void) {
    int ow, oh; char t[160];
    if (SDL_GetRendererOutputSize(ren, &ow, &oh) != 0) { ow = 640; oh = 480; }
    if (scale_mode == SCALE_INTEGER) {
        int k = ow / 640 < oh / 480 ? ow / 640 : oh / 480; if (k < 1) k = 1;
        snprintf(t, sizeof t, "Championship Manager 97/98  [%dx integer, %dx%d]", k, 640 * k, 480 * k);
    } else snprintf(t, sizeof t, "Championship Manager 97/98  [%s]", scale_names[scale_mode]);
    size_t l = strlen(t) - 1;                                     /* ...append the text mode inside the brackets */
    snprintf(t + l, sizeof t - l, ", %s text]", !text_sharp ? "original" : sharp_available() ? "sharp" : "original - fonts not found");
    SDL_SetWindowTitle(win, t);
}
static void apply_scaling(void) {
    /* the filter is taken from this hint when a texture is created, so drop the texture and let
       present() make a new one (works with any SDL2 DLL, unlike SDL_SetTextureScaleMode) */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_mode == SCALE_SMOOTH ? "linear" : "nearest");
    SDL_RenderSetIntegerScale(ren, scale_mode == SCALE_INTEGER ? SDL_TRUE : SDL_FALSE);
    if (tex) { SDL_DestroyTexture(tex); tex = NULL; }
    update_title();
}
static void apply_fullscreen(void) {
    SDL_SetWindowFullscreen(win, want_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
}
/* window size: the largest whole multiple of 640x480 that fits on the desktop with its frame */
static void initial_window_size(int *w, int *h) {
    SDL_Rect r; int k = 2;
    if (SDL_GetDisplayUsableBounds(0, &r) == 0) {
        k = 1;
        while (640 * (k + 1) <= r.w - 16 && 480 * (k + 1) <= r.h - 48) k++;
    }
    *w = 640 * k; *h = 480 * k;
}

/* ---------------- OS mouse pointer (the game's own graphics, drawn by the window system) ---------------- */
int guest_pointer(uint32_t argb[11 * 11], int *shape);
static SDL_Cursor *os_cursor; static uint32_t os_cursor_key; static int os_cursor_visible = -1;
static void update_pointer(void) {
    uint32_t img[121]; int shape = 0;
    int vis = guest_pointer(img, &shape);
    if (vis) {
        int ww, wh, ow, oh; float sx, sy;
        SDL_GetWindowSize(win, &ww, &wh); SDL_GetRendererOutputSize(ren, &ow, &oh);
        SDL_RenderGetScale(ren, &sx, &sy);
        float fs = sx * (ow > 0 ? (float)ww / ow : 1.0f);      /* game pixel size in window units */
        int scale = (int)(fs + 0.5f); if (scale < 1) scale = 1; if (scale > 8) scale = 8;
        uint32_t key = 2166136261u;                                  /* shape + colours + scale */
        for (int i = 0; i < 121; i++) { key ^= img[i]; key *= 16777619u; }
        key ^= (uint32_t)(shape * 31 + scale);
        if (key != os_cursor_key || !os_cursor) {
            static uint32_t big[11 * 8 * 11 * 8];
            int n = 11 * scale;
            for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) big[y * n + x] = img[(y / scale) * 11 + x / scale];
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormatFrom(big, n, n, 32, n * 4, SDL_PIXELFORMAT_ARGB8888);
            if (surf) {
                SDL_Cursor *c = SDL_CreateColorCursor(surf, 0, 0);   /* hotspot: top-left, as in the game */
                SDL_FreeSurface(surf);
                if (c) { SDL_SetCursor(c); if (os_cursor) SDL_FreeCursor(os_cursor); os_cursor = c; os_cursor_key = key; }
            }
        }
    }
    if (vis != os_cursor_visible) { SDL_ShowCursor(vis ? SDL_ENABLE : SDL_DISABLE); os_cursor_visible = vis; }
}

/* Frames are shown (a) at points where the game is waiting (input polls, clock reads): by then
   it has finished drawing; (b) when the picture has been unchanged for one frame interval (a
   held button, a static screen); (c) as a slow fallback inside long calculations. Half-drawn
   screens ("lag frames") therefore don't reach the window. */
#define PRESENT_IDLE_MS     16
#define PRESENT_FALLBACK_MS 250

/* ignore keys/clicks for a moment after the window opens: the Enter used to launch the
   program must not reach the game, where Enter means "Quit Program" */
#define STARTUP_GRACE_MS 1500

/* ---------------- pending input (filled any time, applied at polls) ---------------- */
enum { EV_MOVE, EV_DOWN, EV_UP, EV_KEY };
struct pev { int kind, a, b; };
static struct pev pend[512]; static unsigned p_head, p_tail;
static void push_ev(int kind, int a, int b) {
    if (kind == EV_MOVE && p_tail != p_head) {                 /* coalesce consecutive moves */
        struct pev *last = &pend[(p_tail - 1) & 511];
        if (last->kind == EV_MOVE) { last->a = a; last->b = b; return; }
    }
    if (((p_tail + 1) & 511) == p_head) return;
    pend[p_tail] = (struct pev){ kind, a, b }; p_tail = (p_tail + 1) & 511;
}
static void apply_pending(void) {
    while (p_head != p_tail) {
        struct pev e = pend[p_head]; p_head = (p_head + 1) & 511;
        switch (e.kind) {
        case EV_MOVE: input_mouse_move(e.a, e.b); break;
        case EV_DOWN: input_mouse_button(e.a, 1); break;
        case EV_UP:   input_mouse_button(e.a, 0); break;
        case EV_KEY:  input_key((uint16_t)e.a); break;
        }
        static const char *nm[] = { "move", "down", "up", "key" };
        if (rec) { if (e.kind == EV_MOVE) fprintf(rec, "%lu move %d %d\n", g_polls, e.a, e.b);
                   else fprintf(rec, "%lu %s %d\n", g_polls, nm[e.kind], e.a); fflush(rec); }
        if (inputlog && e.kind != EV_MOVE) fprintf(stderr, "[input] poll %lu: %s %d\n", g_polls, nm[e.kind], e.a);
    }
}

/* ---------------- watchdog ---------------- */
int g_menu_open;                   /* options menu is open (the game is paused) */
static int bk_open;                /* Restore Backup panel is open (the game is paused too) */
static int dbp_open;               /* Choose Database panel is open (likewise) */
static int rp_open;                /* a report (the Database Editor's Test Database) is open (likewise) */
static int watchdog(void *arg) {
    (void)arg; uint32_t last = g_int_count; int quiet = 0, reports = 0;
    for (;;) {
        SDL_Delay(1000);
        uint32_t now = g_int_count;
        if (now != last || g_menu_open || bk_open || dbp_open || rp_open) { last = now; quiet = 0; continue; }
        if (++quiet == 3 && reports < 5) {
            reports++;
            fprintf(stderr, "\n[watchdog] no DOS/BIOS calls for 3 s - the game seems stuck (poll %lu, trace position %u)\n", g_polls, g_trace_pos);
            fprintf(stderr, "[watchdog] guest EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n",
                    eax, ebx, ecx, edx, esi, edi, ebp, esp);
            rt_dump_trace();
            if (rec) fflush(rec);
            quiet = -5;
        }
    }
    return 0;
}

/* audio: CM2's match sounds (sound.c), mixed in SDL's audio thread */
static void audio_cb(void *u, Uint8 *stream, int len) { (void)u; sound_mix((int16_t *)stream, len / 4, *(int *)u ? *(int *)u : 44100); }
static int audio_rate;
static void open_audio(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return; }
    SDL_AudioSpec want, have; SDL_zero(want);
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    want.callback = audio_cb; want.userdata = &audio_rate;
    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!dev) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return; }
    audio_rate = have.freq;
    SDL_PauseAudioDevice(dev, 0);
}
void host_init(void) {
    SDL_SetMainReady();
    /* app ID: Wayland takes the taskbar icon from the matching launcher entry (cm9798.desktop) */
    SDL_SetHint("SDL_APP_ID", "cm9798");                 /* SDL3 / sdl2-compat (Wayland) */
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "cm9798");  /* SDL2 on Wayland */
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "cm9798");      /* X11 */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError()); exit(1);
    }
    cfg_load();
    open_audio();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, scale_mode == SCALE_SMOOTH ? "linear" : "nearest");
    int ww, wh; initial_window_size(&ww, &wh);
    win = SDL_CreateWindow("Championship Manager 97/98", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           ww, wh, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError()); exit(1); }
    {   /* window / taskbar icon */
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)icon_rgba, ICON_W, ICON_H, 32, ICON_W * 4,
                                                             SDL_PIXELFORMAT_RGBA32);
        if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); }
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_ShowCursor(SDL_DISABLE);                 /* shown again by update_pointer() when the game wants a pointer */
    SDL_RenderSetLogicalSize(ren, 640, 480);     /* mouse events arrive in game pixels */
    SDL_SetWindowMinimumSize(win, 640, 480);
    { extern const char *g_game_dir; sharp_init(g_game_dir); }
    apply_scaling(); if (want_fullscreen) apply_fullscreen();
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren);
    inputlog = getenv("CM_INPUTLOG") != NULL;
    if (getenv("CM_RECORD")) {
        rec = fopen(getenv("CM_RECORD"), "w");
        if (rec) fprintf(stderr, "recording input to %s (deterministic clock)\n", getenv("CM_RECORD"));
    }
    start_ticks = SDL_GetTicks();
    SDL_DetachThread(SDL_CreateThread(watchdog, "watchdog", NULL));
}

/* ---- F9: save the game's own 640x480 picture as screenshots/cm9798_<date>_<time>.bmp next to the program ---- */
void host_notify(const char *text);
/* F9: a screenshot of what's on screen, as drawn (the sharp text, star ratings and the rest the port adds
   on top of the game's picture), at the window's resolution, without the window's black bars. Taken in
   present() just before the picture is shown, and before any note in the corner is drawn. */
static int shot_pending;
static void present(void);
static void save_screenshot(void) { shot_pending = 1; present(); }
static void shot_capture(int ox, int oy, int pw, int ph) {
    shot_pending = 0;
    if (pw <= 0 || ph <= 0) return;
    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, pw, ph, 24, SDL_PIXELFORMAT_RGB24);
    if (!s) return;
    SDL_Rect r = { ox, oy, pw, ph };
    if (SDL_RenderReadPixels(ren, &r, SDL_PIXELFORMAT_RGB24, s->pixels, s->pitch) != 0) {
        fprintf(stderr, "screenshot: %s\n", SDL_GetError()); SDL_FreeSurface(s); return; }
    extern const char *g_game_dir; char dir[1100], path[1300];
    snprintf(dir, sizeof dir, "%s/screenshots", g_game_dir);
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    time_t now = time(NULL); struct tm *t = localtime(&now);
    snprintf(path, sizeof path, "%s/cm9798_%04d-%02d-%02d_%02d%02d%02d.bmp", dir,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    int ok = SDL_SaveBMP(s, path) == 0;
    SDL_FreeSurface(s);
    fprintf(stderr, "screenshot: %s%s\n", path, ok ? "" : " (failed)");
    if (ok) host_notify("Screenshot saved");
}

/* ---- short notes in the corner of the screen ("Autosaved - Monday 1 September 1997") ---- */
#define NOTE_MS      3000
#define NOTE_FADE_MS 500
static char toast_text[160]; static uint32_t toast_start; static int toast_on, toast_in_title;
static void update_title(void);
void host_notify(const char *text) {
    snprintf(toast_text, sizeof toast_text, "%s", text);
    toast_start = SDL_GetTicks(); toast_on = 1;
    toast_in_title = !sharp_available();               /* no fonts: show it in the window title instead */
    if (toast_in_title) { char t[256]; snprintf(t, sizeof t, "Championship Manager 97/98  -  %s", text); SDL_SetWindowTitle(win, t); }
}
/* 0 = no note on screen, else its opacity */
static int toast_alpha(void) {
    if (!toast_on) return 0;
    uint32_t t = SDL_GetTicks() - toast_start;
    if (t >= NOTE_MS) { toast_on = 0; if (toast_in_title) update_title(); return 0; }
    if (toast_in_title) return 0;
    return t < NOTE_MS - NOTE_FADE_MS ? 255 : (int)(255 * (NOTE_MS - t) / NOTE_FADE_MS) + 1;
}

/* ---- match speed buttons: |> normal, |>|> double, |>|>|> triple, above "Attacking Possession Last 5 Mins" ----
   drawn into the game's 640x480 picture so they scale with it; shown only while that label is on screen */
void match_speed_apply(void);                         /* overrides.c */
static SDL_Rect speed_btn[3]; static int speed_ui, speed_swallow_up;
static void fill(uint32_t *px, int w, int x0, int y0, int x1, int y1, uint32_t c) {
    for (int y = y0; y <= y1; y++) for (int x = x0; x <= x1; x++) px[y * w + x] = c;
}
static void speed_buttons(uint32_t *px, const uint8_t *p, int w, int h, int st) {
    int x0, y0, x1, y1;
    speed_ui = w == 640 && h == 480 && text_find(p, st, "Attacking Possession", &x0, &y0, &x1, &y1);
    if (!speed_ui) return;
    const int bw = 30, bh = 16, gap = 4;
    (void)x0; (void)x1;
    int left = w / 2 - bw / 2 - (bw + gap), top = y0 - 4 - bh;     /* middle button centred on the screen */
    if (top < 0) { speed_ui = 0; return; }
    for (int i = 0; i < 3; i++) {
        SDL_Rect r = { left + i * (bw + gap), top, bw, bh }; speed_btn[i] = r;
        int on = match_speed == i + 1, X0 = r.x, Y0 = r.y, X1 = r.x + r.w - 1, Y1 = r.y + r.h - 1;
        fill(px, w, X0, Y0, X1, Y1, 0xFF000000u);                                  /* outline */
        fill(px, w, X0 + 1, Y0 + 1, X1 - 1, Y1 - 1, on ? 0xFFAAAAAAu : 0xFF6E6E6Eu);  /* face */
        fill(px, w, X0 + 1, Y0 + 1, X1 - 1, Y0 + 1, 0xFFDCDCDCu);                  /* bevel: light top-left */
        fill(px, w, X0 + 1, Y0 + 1, X0 + 1, Y1 - 1, 0xFFDCDCDCu);
        fill(px, w, X0 + 1, Y1 - 1, X1 - 1, Y1 - 1, 0xFF3C3C3Cu);                  /* dark bottom-right */
        fill(px, w, X1 - 1, Y0 + 1, X1 - 1, Y1 - 1, 0xFF3C3C3Cu);
        uint32_t ic = on ? 0xFFFFFF00u : 0xFF141414u;                               /* i+1 "play" triangles */
        const int tw = 5, th = 4; int n = i + 1, tx = X0 + (r.w - n * tw) / 2, cy = Y0 + r.h / 2;
        for (int k = 0; k < n; k++) for (int c = 0; c < tw; c++) {
            int half = th - (c * th) / tw;
            fill(px, w, tx + k * tw + c, cy - half, tx + k * tw + c, cy + half - 1, ic);
        }
    }
}
/* "Choose Leagues to Run": leagues the chosen database doesn't have are greyed out and can't be
   clicked (database.cfg "leagues = ..."; database.c) */
static SDL_Rect grey_rows[12]; static int n_grey_rows;
static void grey_leagues(uint32_t *px, const uint8_t *p, int w, int h, int st, int sharp) {
    static const char *const leagues[] = { "English League", "Italian League", "Spanish League", "German League",
        "French League", "Scottish League", "Dutch League", "Portuguese League", "Belgian League", NULL };
    int x0, y0, x1, y1, idx[40];
    n_grey_rows = 0;
    if (w != 640 || h != 480 || !text_find(p, st, "Choose Leagues to Run", &x0, &y0, &x1, &y1)) return;
    for (int l = 0; leagues[l]; l++) {
        if (db_league_allowed(leagues[l])) continue;
        int n = text_find_glyphs(p, st, leagues[l], &x0, &y0, &x1, &y1, idx, 40);
        if (!n) continue;
        if (n_grey_rows < 12) { SDL_Rect r = { 150, y0 - 6, 340, y1 - y0 + 12 }; grey_rows[n_grey_rows++] = r; }
        for (int k = 0; k < n; k++) {
            int i = idx[k]; struct tglyph *g = &g_glyphs[i];
            g_text_dim[i] = 1;
            if (sharp) continue;                               /* sharp_draw dims it */
            for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++) {
                int X = g->x + x, Y = g->y + y;
                if (X < 0 || Y < 0 || X >= w || Y >= h || !text_is_ink(i, x, y)) continue;
                uint32_t c = px[Y * w + X];
                px[Y * w + X] = 0xFF000000u | ((((c >> 16) & 255) * 2 / 5) << 16) | ((((c >> 8) & 255) * 2 / 5) << 8) | ((c & 255) * 2 / 5);
            }
        }
    }
}
static int grey_hit(int x, int y) {
    for (int i = 0; i < n_grey_rows; i++) { SDL_Rect r = grey_rows[i];
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return 1; }
    return 0;
}
/* Player screen: each stat's number coloured by how good it is - 1-7 red, 8-14 yellow, 15-20 green;
   the other way round for Aggression and Injury Proneness, where high is bad. */
static void stat_colours(uint32_t *px, const uint8_t *p, int w, int h, int st, int sharp) {
    static const char *const labels[] = { "Adaptability", "Aggression", "Creativity", "Determination", "Dribbling",
        "Flair", "Heading", "Influence", "Injury proneness", "Off the ball", "Pace", "Passing", "Positioning",
        "Set pieces", "Shooting", "Stamina", "Strength", "Tackling", "Technique", "Aerial ability", "Rushing out",
        "Shot stopping", "Handling", "Kicking", "Reflexes", "One on ones", "Communication", "Throwing", NULL };
    int x0, y0, x1, y1, idx[24];
    if (!g_stat_colours || w != 640 || h != 480 || !text_find(p, st, "Adaptability", &x0, &y0, &x1, &y1) ||
        !text_find(p, st, "Born", &x0, &y0, &x1, &y1)) return;                 /* the player screen's Playing Skills */
    static int vis[8192]; int nv = text_visible(p, st, w, h, vis, 8192);
    for (int l = 0; labels[l]; l++) {
        int n = text_find_glyphs(p, st, labels[l], &x0, &y0, &x1, &y1, idx, 24);
        if (!n) continue;
        int row = g_glyphs[idx[0]].y, lim = x0 < 320 ? 320 : 640;
        int dig[4], nd = 0;                                             /* the number on the same line */
        for (int k = 0; k < nv && nd < 4; k++) { struct tglyph *g = &g_glyphs[vis[k]];
            if (g->y == row && g->x > x1 && g->x < lim && g->ch >= '0' && g->ch <= '9') dig[nd++] = vis[k]; }
        if (!nd || nd > 2) continue;
        if (nd == 2 && g_glyphs[dig[1]].x < g_glyphs[dig[0]].x) { int t = dig[0]; dig[0] = dig[1]; dig[1] = t; }
        int v = 0; for (int k = 0; k < nd; k++) v = v * 10 + (g_glyphs[dig[k]].ch - '0');
        if (v < 1 || v > 20) continue;
        int bad = !strcmp(labels[l], "Aggression") || !strcmp(labels[l], "Injury proneness");
        int band = v <= 7 ? 0 : v <= 14 ? 1 : 2; if (bad) band = 2 - band;
        uint32_t c = band == 0 ? 0xFFFF4040u : band == 1 ? 0xFFFFFF00u : 0xFF40E040u;
        for (int k = 0; k < nd; k++) {
            int i = dig[k]; struct tglyph *g = &g_glyphs[i];
            g_text_tint[i] = c;
            if (sharp) continue;                                       /* sharp_draw colours it */
            for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++) {
                int X = g->x + x, Y = g->y + y;
                if (X < 0 || Y < 0 || X >= w || Y >= h || !text_is_ink(i, x, y)) continue;
                px[Y * w + X] = c;
            }
        }
    }
}
/* Options screen: "Using 97/98 Database" / "Using 95/96 Database" just above "Quit Program" */
int cm_start_year(void);
static char db_text[128]; static int db_x, db_y, db_w, db_on;   /* (database names can be long) */
static void db_label(uint32_t *px, const uint8_t *p, int w, int h, int st, int sharp) {
    int x0, y0, x1, y1; struct tfont f;
    db_on = w == 640 && h == 480 && text_find(p, st, "Quit Program", &x0, &y0, &x1, &y1) && text_font_info(1, &f);
    if (!db_on) return;
    snprintf(db_text, sizeof db_text, "Using %s Database", db_name());
    int tw = 0;
    for (const char *c = db_text; *c; c++) if ((uint8_t)*c >= f.first && (uint8_t)*c <= f.last) tw += text_glyph_width(&f, (uint8_t)*c);
    db_x = w / 2 - tw / 2; db_y = 426; db_w = tw;
    if (sharp && sharp_available()) return;                /* drawn sharp over the picture instead */
    for (int pass = 0; pass < 2; pass++) {                 /* drop shadow, then the text */
        uint32_t c = pass ? 0xFFE6E6E6u : 0xFF000000u; int gx = db_x + (pass ? 0 : 1), gy = db_y + (pass ? 0 : 1);
        for (const char *s = db_text; *s; s++) {
            int ch = (uint8_t)*s; if (ch < f.first || ch > f.last) continue;
            int gw = text_glyph_width(&f, ch);
            for (int y = 0; y < f.height; y++) for (int x = 0; x < gw; x++)
                if (text_glyph_bit(&f, ch, x, y) && gx + x < w && gy + y < h) px[(gy + y) * w + gx + x] = c;
            gx += gw;
        }
    }
}
static int speed_hit(int x, int y) {
    if (!speed_ui) return 0;
    for (int i = 0; i < 3; i++) { SDL_Rect r = speed_btn[i];
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return i + 1; }
    return 0;
}

/* ---- F1: the port's options menu ----
   Drawn over the dimmed game picture in the game's style; the game is paused while it is open
   (menu_run() is a loop of its own, entered from pump()). Layout is in the game's 640x480 units. */
static int menu_mx = -1, menu_my = -1;
extern int g_autosave, g_backup_keep;
/* two tabs: Graphics (rows 0-3) and Gameplay (rows 4-9) */
enum { M_WINDOW, M_SCALING, M_TEXT, M_BACKGROUNDS, M_SOUND, M_VOLUME, M_STARS, M_COLOURS, M_AUTOSAVE, M_BACKUPS, M_DONE, M_TAB, M_ROWS };
static const char *const menu_opts[M_ROWS][4] = {
    { "Window", "Fullscreen" }, { "Integer", "Nearest", "Smooth" }, { "Sharp", "Original" }, { "CM 97/98", "CM 2" }, { "On", "Off" }, { "slider" },
    { "Your club", "Overall", "Off" }, { "On", "Off" }, { "On", "Off" }, { "Off", "3", "5", "10" }, { 0 }, { 0 } };
static const int backup_counts[4] = { 0, 3, 5, 10 };
static const char *const menu_names[M_ROWS] = { "Window", "Scaling", "Text", "Backgrounds", "Match sounds", "Volume", "Star ratings", "Coloured stats",
                                                "Autosave", "Backups per slot", "", "" };
static const char *const menu_tabs[2] = { "Graphics", "Gameplay" };
static int menu_tab;                               /* 0 = Graphics, 1 = Gameplay */
int g_show_stars = 1, g_stat_colours = 1;          /* player screen: star rating, coloured stats */
#define MENU_X0 110
#define MENU_X1 530
#define MENU_Y0 48
#define MENU_H  386
static int row_tab(int r) { return r <= M_BACKGROUNDS ? 0 : 1; }
static int menu_nopts(int r) { int n = 0; while (n < 4 && menu_opts[r][n]) n++; return n; }
/* "CM 2" backgrounds need CM2's STADPIC1-5 (copied by the setup's CM2 import): greyed out without them.
   The cm9798.cfg setting is kept either way, so it takes effect once the pictures are there. */
static int cm2_bg_ok;
static int snd_ok;                          /* CM2's match sounds found (sound.c) */
static int vol_drag;                        /* dragging the Volume slider */
static int menu_disabled(int r, int i) { return (r == M_BACKGROUNDS && i == 1 && !cm2_bg_ok) || (r == M_SOUND && i == 0 && !snd_ok) || (r == M_VOLUME && !snd_ok); }
static int menu_current(int r) {
    switch (r) {
    case M_WINDOW: return want_fullscreen ? 1 : 0;
    case M_SCALING: return scale_mode;
    case M_TEXT: return text_sharp ? 0 : 1;
    case M_BACKGROUNDS: return g_cm2_backgrounds && cm2_bg_ok ? 1 : 0;   /* (what is actually shown) */
    case M_SOUND: return g_sound && snd_ok ? 0 : 1;
    case M_STARS: return !g_show_stars ? 2 : g_star_mode == 1 ? 1 : 0;
    case M_COLOURS: return g_stat_colours ? 0 : 1;
    case M_AUTOSAVE: return g_autosave ? 0 : 1;
    case M_BACKUPS: for (int i = 0; i < 4; i++) if (backup_counts[i] == g_backup_keep) return i; return -1;
    }
    return -1;
}
/* top of row r within its tab; a section heading sits above the first row of each section */
static int menu_row_y(int r) {
    int y = MENU_Y0 + 76 + 22;
    if (row_tab(r) == 0) return y + (r - M_WINDOW) * 24;
    if (r >= M_STARS) y += 22;
    if (r >= M_AUTOSAVE) y += 22;
    return y + (r - M_SOUND) * 24;
}
static SDL_Rect menu_button(int r, int i) {
    if (r == M_DONE) { SDL_Rect d = { MENU_X0 + 10, MENU_Y0 + MENU_H - 36, MENU_X1 - MENU_X0 - 20, 26 }; return d; }
    if (r == M_TAB) { int w = (MENU_X1 - MENU_X0 - 36) / 2; SDL_Rect t = { MENU_X0 + 16 + i * (w + 4), MENU_Y0 + 44, w, 22 }; return t; }
    int n = menu_nopts(r), x0 = MENU_X0 + 170, x1 = MENU_X1 - 16, gap = 4, bw = (x1 - x0 - gap * (n - 1)) / n;
    SDL_Rect b = { x0 + i * (bw + gap), menu_row_y(r), bw, 18 }; return b;
}
static void menu_choose(int r, int i) {
    if (menu_disabled(r, i)) return;
    switch (r) {
    case M_TAB: menu_tab = i; return;
    case M_WINDOW: want_fullscreen = i; apply_fullscreen(); break;
    case M_SCALING: scale_mode = i; apply_scaling(); break;
    case M_TEXT: text_sharp = i == 0; update_title(); break;
    case M_BACKGROUNDS: g_cm2_backgrounds = i == 1; break;   /* (the next picture the game loads) */
    case M_SOUND: g_sound = i == 0; break;
    case M_VOLUME: return;                     /* the slider: menu_run() */
    case M_STARS: g_show_stars = i != 2; g_star_mode = i == 0 ? 2 : i == 1 ? 1 : 0; break;
    case M_COLOURS: g_stat_colours = i == 0; break;
    case M_AUTOSAVE: g_autosave = i == 0; break;
    case M_BACKUPS: g_backup_keep = backup_counts[i]; break;
    case M_DONE: g_menu_open = 0; return;
    }
    cfg_save();
}
static int menu_hit(int x, int y, int *row, int *opt) {
    for (int r = 0; r < M_ROWS; r++) {
        if (r < M_DONE && row_tab(r) != menu_tab) continue;
        int n = r == M_DONE ? 1 : r == M_TAB ? 2 : menu_nopts(r);
        for (int i = 0; i < n; i++) { SDL_Rect b = menu_button(r, i);
            if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) { *row = r; *opt = i; return 1; } }
    }
    return 0;
}

/* drawing, in output pixels: the 640x480 picture lies at (ox, oy), k output pixels per unit */
static float mk; static int mox, moy;
static SDL_Rect mrect(int x, int y, int w, int h) {
    SDL_Rect r = { mox + (int)(x * mk), moy + (int)(y * mk), (int)((x + w) * mk) - (int)(x * mk), (int)((y + h) * mk) - (int)(y * mk) };
    return r;
}
static void mfill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SDL_Rect q = mrect(x, y, w, h); SDL_SetRenderDrawColor(ren, r, g, b, a); SDL_RenderFillRect(ren, &q);
}
static void mbevel(SDL_Rect b, uint8_t face) {
    mfill(b.x, b.y, b.w, b.h, 0, 0, 0, 255);
    mfill(b.x + 1, b.y + 1, b.w - 2, b.h - 2, face, face, face, 255);
    mfill(b.x + 1, b.y + 1, b.w - 2, 1, 220, 220, 220, 255); mfill(b.x + 1, b.y + 1, 1, b.h - 2, 220, 220, 220, 255);
    mfill(b.x + 1, b.y + b.h - 2, b.w - 2, 1, 60, 60, 60, 255); mfill(b.x + b.w - 2, b.y + 1, 1, b.h - 2, 60, 60, 60, 255);
}
/* text: the real font if available, else the game's own 10pt bitmap font, scaled */
static void mtext(const char *t, int x, int y, int w, int h, int size, SDL_Color c, int bold, int align, int shadow) {
    SDL_Rect q = mrect(x, y, w, h); int px = (int)(size * mk + 0.5f);
    if (shadow) { SDL_Color sc = { 0, 0, 0, 255 }; int o = (int)(mk + 0.5f);
        if (sharp_label(ren, t, q.x + o, q.y + o, q.w, q.h, px, sc, bold, align)) {} }
    if (sharp_label(ren, t, q.x, q.y, q.w, q.h, px, c, bold, align)) return;
    struct tfont f; if (!text_font_info(1, &f)) return;
    int tw = 0; for (const char *s = t; *s; s++) if ((uint8_t)*s >= f.first && (uint8_t)*s <= f.last) tw += text_glyph_width(&f, (uint8_t)*s);
    int gx = align ? x + (w - tw) / 2 : x, gy = y + (h - f.height) / 2;
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
    for (const char *s = t; *s; s++) {
        int ch = (uint8_t)*s; if (ch < f.first || ch > f.last) continue;
        int gw = text_glyph_width(&f, ch);
        for (int yy = 0; yy < f.height; yy++) for (int xx = 0; xx < gw; xx++)
            if (text_glyph_bit(&f, ch, xx, yy)) { SDL_Rect pr = mrect(gx + xx, gy + yy, 1, 1); SDL_RenderFillRect(ren, &pr); }
        gx += gw;
    }
}
static void menu_draw(float k, int ox, int oy, int pw, int ph) {
    mk = k; mox = ox; moy = oy;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect all = { ox, oy, pw, ph }; SDL_SetRenderDrawColor(ren, 0, 0, 0, 140); SDL_RenderFillRect(ren, &all);   /* dim the game */
    const SDL_Color yellow = { 255, 255, 40, 255 }, white = { 255, 255, 255, 255 }, grey = { 215, 215, 215, 255 },
                    dark = { 0, 0, 0, 255 }, dim = { 200, 200, 200, 255 }, greyed = { 110, 110, 110, 255 };
    int W = MENU_X1 - MENU_X0;
    mfill(MENU_X0, MENU_Y0, W, MENU_H, 0, 0, 0, 255);
    mfill(MENU_X0 + 1, MENU_Y0 + 1, W - 2, MENU_H - 2, 52, 52, 52, 245);
    mfill(MENU_X0 + 1, MENU_Y0 + 1, W - 2, 33, 0, 72, 0, 255);                          /* title bar, like the date header */
    mfill(MENU_X0 + 1, MENU_Y0 + 34, W - 2, 1, 0, 0, 0, 255);
    mtext("Recomp Options", MENU_X0, MENU_Y0, W, 35, 19, yellow, 1, 1, 1);
    for (int t = 0; t < 2; t++) {                                       /* the tabs */
        SDL_Rect b = menu_button(M_TAB, t); int on = t == menu_tab;
        int hover = menu_mx >= b.x && menu_mx < b.x + b.w && menu_my >= b.y && menu_my < b.y + b.h;
        mbevel(b, on ? 170 : hover ? 120 : 96);
        mtext(menu_tabs[t], b.x, b.y, b.w, b.h, 11, on ? dark : dim, 1, 1, 0);
    }
    const char *sections[4] = { "Display", "Match", "Player screen", "Saves" }; int srow[4] = { M_WINDOW, M_SOUND, M_STARS, M_AUTOSAVE };
    for (int sct = 0; sct < 4; sct++) {
        if (row_tab(srow[sct]) != menu_tab) continue;
        int y = menu_row_y(srow[sct]) - 22;
        mtext(sections[sct], MENU_X0 + 18, y, 200, 15, 11, white, 1, 0, 0);
        mfill(MENU_X0 + 18, y + 16, W - 34, 1, 110, 110, 110, 255);
    }
    for (int r = 0; r < M_DONE; r++) {
        if (row_tab(r) != menu_tab) continue;
        mtext(menu_names[r], MENU_X0 + 26, menu_row_y(r), 140, 18, 10, grey, 0, 0, 0);
        int cur = menu_current(r);
        if (r == M_VOLUME) {                        /* a slider: track, the filled part, a knob, and the % */
            SDL_Rect b = menu_button(r, 0); int off = menu_disabled(r, 0);
            int tw = b.w - 44, ty = b.y + b.h / 2 - 3, fill = tw * g_sound_volume / 100;
            mfill(b.x, ty, tw, 6, 0, 0, 0, 255);
            mfill(b.x + 1, ty + 1, tw - 2, 4, 40, 40, 40, 255);
            if (!off) mfill(b.x + 1, ty + 1, fill > 2 ? fill - 2 : 0, 4, 200, 200, 40, 255);
            SDL_Rect k = { b.x + fill - 5, b.y + 1, 10, b.h - 2 };
            if (k.x < b.x) k.x = b.x;
            if (k.x + k.w > b.x + tw) k.x = b.x + tw - k.w;
            mbevel(k, off ? 64 : vol_drag ? 200 : 170);
            char pc[8]; snprintf(pc, sizeof pc, "%d%%", g_sound_volume);
            mtext(pc, b.x + tw + 4, b.y, 40, b.h, 10, off ? greyed : white, 0, 1, 0);
            continue;
        }
        for (int i = 0; i < menu_nopts(r); i++) {
            SDL_Rect b = menu_button(r, i); int on = i == cur, off = menu_disabled(r, i);
            int hover = !off && menu_mx >= b.x && menu_mx < b.x + b.w && menu_my >= b.y && menu_my < b.y + b.h;
            mbevel(b, off ? 64 : on ? 170 : hover ? 120 : 96);
            mtext(menu_opts[r][i], b.x, b.y, b.w, b.h, 10, off ? greyed : on ? dark : dim, on, 1, 0);
        }
    }
    SDL_Rect done = menu_button(M_DONE, 0);
    int hover = menu_mx >= done.x && menu_mx < done.x + done.w && menu_my >= done.y && menu_my < done.y + done.h;
    mbevel(done, hover ? 120 : 100);
    mtext("Done", done.x, done.y, done.w, done.h, 13, white, 0, 1, 1);
}

/* ---- "Choose Leagues to Run": the Players slider above Done ----
   Small | Medium | Full: how many of the database's players a new game keeps (overrides.c,
   player_limit). In the game's 640x480 units; clicks on it are the port's, not the game's. */
#define PS_X0 200
#define PS_X1 440
#define PS_Y  407                                  /* the track's row (knob 18 high) */
static int players_ui, players_drag;
static int ps_stop_x(int i) { return PS_X0 + (PS_X1 - PS_X0) * i / 2; }
static int players_hit(int x, int y) { return players_ui && x >= 158 && x < 482 && y >= 384 && y < 443; }
static void players_set(int x) {
    int best = 0; for (int i = 1; i < 3; i++) if (abs(x - ps_stop_x(i)) < abs(x - ps_stop_x(best))) best = i;
    if (best != g_player_size) { g_player_size = best; present(); }
}
static void players_draw(float k, int ox, int oy) {
    mk = k; mox = ox; moy = oy;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    const SDL_Color white = { 230, 230, 230, 255 }, yellow = { 255, 255, 40, 255 }, dim = { 170, 170, 170, 255 };
    static const char *const names[3] = { "Small", "Medium", "Full" };
    static const char *const counts[3] = { "up to 9,900 players, as the original", "up to 14,000 players", "up to 20,000 players - the whole database" };
    int cur = g_player_size >= 0 && g_player_size <= 2 ? g_player_size : 2;
    char cap[96]; snprintf(cap, sizeof cap, "Players: %s, %s", names[cur], counts[cur]);
    mfill(158, 384, 324, 59, 0, 0, 0, 150);                                        /* a dark panel, for reading */
    mtext(cap, 0, 387, 640, 16, 11, white, 0, 1, 1);
    mfill(PS_X0, PS_Y + 6, PS_X1 - PS_X0, 6, 0, 0, 0, 255);                       /* the track */
    mfill(PS_X0 + 1, PS_Y + 7, PS_X1 - PS_X0 - 2, 4, 40, 40, 40, 255);
    if (cur) mfill(PS_X0 + 1, PS_Y + 7, ps_stop_x(cur) - PS_X0 - 1, 4, 200, 200, 40, 255);
    for (int i = 0; i < 3; i++) {                                                    /* the stops and their names */
        mfill(ps_stop_x(i) - 1, PS_Y + 3, 3, 12, 0, 0, 0, 255);
        mtext(names[i], ps_stop_x(i) - 40, PS_Y + 19, 80, 14, 10, i == cur ? yellow : dim, i == cur, 1, 1);
    }
    SDL_Rect kn = { ps_stop_x(cur) - 6, PS_Y, 12, 18 };
    mbevel(kn, players_drag ? 200 : 170);
}

/* ---- Restore Backup panel: each saved game's backups (backup.c), newest first; Restore copies the
   chosen one over the saved game, after backing up what's there now. Opened by the Options screen's
   "Restore Backup" box (overrides.c); the game waits while it is open. Layout in the game's 640x480 units, like the F1 menu. ---- */
#define BK_X0 60
#define BK_X1 580
#define BK_Y0 52
#define BK_H  376
#define BK_ROWS 11
#define BK_ROW_H 19
#define BK_MAX 200
static void present(void); static int shortcut_key(const SDL_KeyboardEvent *k); int host_ask(const char *title, const char *text);
static int bk_slot = 1, bk_sel = -1, bk_top, bk_n, bk_count[8], bk_mx = -1, bk_my = -1;
static struct backup_info bk_list[BK_MAX];
static char bk_names[8][40];
static void bk_load(void) {
    for (int s = 1; s <= 7; s++) {
        struct backup_info tmp[BK_MAX];
        bk_count[s] = backup_list(s, tmp, BK_MAX);
        if (!backup_slot_name(s, bk_names[s], sizeof bk_names[s])) bk_names[s][0] = 0;
    }
    bk_n = backup_list(bk_slot, bk_list, BK_MAX);
    for (int i = 0; i < bk_n; i++) backup_describe(bk_slot, &bk_list[i]);   /* the last match in each */
    if (bk_sel >= bk_n) bk_sel = -1;
    if (bk_top > bk_n - BK_ROWS) bk_top = bk_n - BK_ROWS; if (bk_top < 0) bk_top = 0;
}
static SDL_Rect bk_slot_btn(int s) {             /* s = 1..7 */
    int x0 = BK_X0 + 112, x1 = BK_X1 - 16, gap = 4, bw = (x1 - x0 - gap * 6) / 7;
    SDL_Rect r = { x0 + (s - 1) * (bw + gap), BK_Y0 + 46, bw, 20 }; return r;
}
static SDL_Rect bk_row(int i) { SDL_Rect r = { BK_X0 + 16, BK_Y0 + 120 + i * BK_ROW_H, BK_X1 - BK_X0 - 32, BK_ROW_H - 1 }; return r; }
static SDL_Rect bk_btn(int i) {                  /* 0 = Restore, 1 = Close */
    int w = (BK_X1 - BK_X0 - 30) / 2;
    SDL_Rect r = { BK_X0 + 10 + i * (w + 10), BK_Y0 + BK_H - 36, w, 26 }; return r;
}
static int in_rect(SDL_Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

static void bk_draw(float k, int ox, int oy, int pw, int ph) {
    mk = k; mox = ox; moy = oy;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect all = { ox, oy, pw, ph }; SDL_SetRenderDrawColor(ren, 0, 0, 0, 140); SDL_RenderFillRect(ren, &all);
    const SDL_Color yellow = { 255, 255, 40, 255 }, white = { 255, 255, 255, 255 }, grey = { 215, 215, 215, 255 },
                    dark = { 0, 0, 0, 255 }, dim = { 200, 200, 200, 255 }, faint = { 120, 120, 120, 255 }, hint = { 170, 170, 170, 255 };
    int W = BK_X1 - BK_X0;
    mfill(BK_X0, BK_Y0, W, BK_H, 0, 0, 0, 255);
    mfill(BK_X0 + 1, BK_Y0 + 1, W - 2, BK_H - 2, 52, 52, 52, 245);
    mfill(BK_X0 + 1, BK_Y0 + 1, W - 2, 33, 0, 72, 0, 255);
    mfill(BK_X0 + 1, BK_Y0 + 34, W - 2, 1, 0, 0, 0, 255);
    mtext("Restore Backup", BK_X0, BK_Y0, W, 35, 19, yellow, 1, 1, 1);
    mtext("Saved Game", BK_X0 + 18, BK_Y0 + 47, 90, 18, 10, grey, 0, 0, 0);
    for (int s = 1; s <= 7; s++) {
        SDL_Rect b = bk_slot_btn(s); int on = s == bk_slot, any = bk_count[s] > 0;
        mbevel(b, on ? 170 : in_rect(b, bk_mx, bk_my) && any ? 120 : 96);
        char t[4]; snprintf(t, sizeof t, "%d", s);
        mtext(t, b.x, b.y, b.w, b.h, 10, on ? dark : any ? dim : faint, on, 1, 0);
    }
    char line[160];
    if (bk_names[bk_slot][0]) snprintf(line, sizeof line, "Saved Game %d: %s", bk_slot, bk_names[bk_slot]);
    else snprintf(line, sizeof line, "Saved Game %d: (empty)", bk_slot);
    mtext(line, BK_X0 + 18, BK_Y0 + 74, W - 36, 16, 10, white, 1, 0, 0);
    mtext("Backup made", BK_X0 + 22, BK_Y0 + 100, 220, 15, 9, grey, 1, 0, 0);
    mtext("Last match", BK_X0 + 22 + 160, BK_Y0 + 100, 220, 15, 9, grey, 1, 0, 0);
    mfill(BK_X0 + 16, BK_Y0 + 116, W - 32, 1, 110, 110, 110, 255);
    if (!bk_n) mtext("No backups of this saved game yet.", BK_X0 + 22, BK_Y0 + 124, W - 44, 16, 10, dim, 0, 0, 0);
    for (int i = 0; i < BK_ROWS && bk_top + i < bk_n; i++) {
        int j = bk_top + i; SDL_Rect r = bk_row(i);
        if (j == bk_sel) mfill(r.x, r.y, r.w, r.h, 170, 170, 170, 255);
        else if (in_rect(r, bk_mx, bk_my)) mfill(r.x, r.y, r.w, r.h, 80, 80, 80, 255);
        SDL_Color c = j == bk_sel ? dark : white;
        mtext(bk_list[j].when, r.x + 6, r.y, 150, r.h, 10, c, 0, 0, 0);
        if (bk_list[j].last[0]) {                       /* without the competition if it wouldn't fit */
            char t[96]; snprintf(t, sizeof t, "%s", bk_list[j].last); struct tfont f;
            if (text_font_info(1, &f)) {
                int tw = 0; for (const char *q = t; *q; q++) if ((uint8_t)*q >= f.first && (uint8_t)*q <= f.last) tw += text_glyph_width(&f, (uint8_t)*q);
                char *br = strstr(t, " ("); if (tw > r.w - 176 && br) *br = 0;
            }
            mtext(t, r.x + 166, r.y, r.w - 172, r.h, 10, c, 0, 0, 0);
        }
    }
    if (bk_n > BK_ROWS) {
        snprintf(line, sizeof line, "%d-%d of %d  (scroll for more)", bk_top + 1, bk_top + BK_ROWS < bk_n ? bk_top + BK_ROWS : bk_n, bk_n);
        mtext(line, BK_X0 + 22, BK_Y0 + 122 + BK_ROWS * BK_ROW_H, W - 36, 14, 8, hint, 0, 0, 0);
    }
    mtext("Restore copies the chosen backup over the saved game. What's in the saved game now is",
          BK_X0 + 18, BK_Y0 + BK_H - 74, W - 36, 14, 8, hint, 0, 0, 0);
    mtext("backed up first, so a restore can always be undone the same way.",
          BK_X0 + 18, BK_Y0 + BK_H - 60, W - 36, 14, 8, hint, 0, 0, 0);
    for (int i = 0; i < 2; i++) {
        SDL_Rect b = bk_btn(i); int usable = i == 1 || bk_sel >= 0;
        mbevel(b, usable && in_rect(b, bk_mx, bk_my) ? 120 : 100);
        mtext(i ? "Close" : "Restore", b.x, b.y, b.w, b.h, 13, usable ? white : faint, 0, 1, usable);
    }
}

static void bk_restore_chosen(void) {
    if (bk_sel < 0 || bk_sel >= bk_n) return;
    struct backup_info b = bk_list[bk_sel];
    char q[400], before[40];
    snprintf(before, sizeof before, "%s", bk_names[bk_slot]);
    snprintf(q, sizeof q, "Copy the backup from %s over Saved Game %d%s%s%s?\n\n"
             "What's in Saved Game %d now is backed up first.", b.when, bk_slot,
             before[0] ? " (" : "", before, before[0] ? ")" : "", bk_slot);
    present();
    if (!host_ask("Restore Backup", q)) return;
    int r = backup_restore(bk_slot, b.name);
    bk_sel = -1; bk_top = 0; bk_load();
    char note[160];
    if (r < 0) snprintf(note, sizeof note, "Saved Game %d could not be restored - see the log", bk_slot);
    else snprintf(note, sizeof note, "Saved Game %d restored (backup from %s)", bk_slot, b.when);
    host_notify(note);
}

static void bk_run(void) {
    bk_open = 1; bk_mx = bk_my = -1; bk_sel = -1; bk_top = 0;
    bk_load();
    if (!bk_count[bk_slot]) for (int s = 1; s <= 7; s++) if (bk_count[s]) { bk_slot = s; bk_load(); break; }
    SDL_SetCursor(SDL_GetDefaultCursor()); SDL_ShowCursor(SDL_ENABLE);
    present();
    while (bk_open) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) do {
            switch (e.type) {
            case SDL_QUIT:
                fprintf(stderr, "window closed - exiting\n");
                if (rec) fclose(rec);
                SDL_Quit(); exit(0);
            case SDL_KEYDOWN:
                if (shortcut_key(&e.key)) break;
                if (e.key.repeat) break;
                if (e.key.keysym.sym == SDLK_ESCAPE) bk_open = 0;
                else if (e.key.keysym.sym == SDLK_UP && bk_n) { bk_sel = bk_sel <= 0 ? 0 : bk_sel - 1; }
                else if (e.key.keysym.sym == SDLK_DOWN && bk_n) { bk_sel = bk_sel < 0 ? 0 : bk_sel + 1 < bk_n ? bk_sel + 1 : bk_sel; }
                else if ((e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) && bk_sel >= 0) bk_restore_chosen();
                else if (e.key.keysym.sym >= SDLK_1 && e.key.keysym.sym <= SDLK_7) { bk_slot = e.key.keysym.sym - SDLK_0; bk_sel = -1; bk_top = 0; bk_load(); }
                if (bk_sel >= 0) { if (bk_sel < bk_top) bk_top = bk_sel; if (bk_sel >= bk_top + BK_ROWS) bk_top = bk_sel - BK_ROWS + 1; }
                break;
            case SDL_MOUSEMOTION: bk_mx = e.motion.x; bk_my = e.motion.y; break;
            case SDL_MOUSEWHEEL:
                bk_top -= e.wheel.y * 2;
                if (bk_top > bk_n - BK_ROWS) bk_top = bk_n - BK_ROWS; if (bk_top < 0) bk_top = 0;
                break;
            case SDL_MOUSEBUTTONDOWN: {
                if (e.button.button != SDL_BUTTON_LEFT) break;
                int x = e.button.x, y = e.button.y;
                for (int s = 1; s <= 7; s++) if (in_rect(bk_slot_btn(s), x, y)) { bk_slot = s; bk_sel = -1; bk_top = 0; bk_load(); }
                for (int i = 0; i < BK_ROWS && bk_top + i < bk_n; i++) if (in_rect(bk_row(i), x, y)) {
                    int j = bk_top + i;
                    if (j == bk_sel && e.button.clicks >= 2) bk_restore_chosen(); else bk_sel = j;
                }
                if (in_rect(bk_btn(0), x, y) && bk_sel >= 0) bk_restore_chosen();
                if (in_rect(bk_btn(1), x, y)) bk_open = 0;
                if (x < BK_X0 || x >= BK_X1 || y < BK_Y0 || y >= BK_Y0 + BK_H) bk_open = 0;   /* a click outside closes it */
                break; }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_title();
                break;
            default: break;
            }
        } while (bk_open && SDL_PollEvent(&e));
        present();
    }
    os_cursor_key = 0; os_cursor_visible = -1;
    SDL_PumpEvents(); SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);
    present();
}

/* ---- Choose Database panel: Change Database on the Options screen (overrides.c). One row per
   database (databases/<folder>, database.c): its name and the day its first season starts. ---- */
#define DB_X0 90
#define DB_X1 550
#define DB_Y0 80
#define DB_ROWS 10
#define DB_ROW_H 20
static int dbp_sel, dbp_top, dbp_n;
static int dbp_h(void) { int r = dbp_n < 3 ? 3 : dbp_n > DB_ROWS ? DB_ROWS : dbp_n; return 150 + r * DB_ROW_H; }
static int dbp_y0(void) { return 240 - dbp_h() / 2; }
static SDL_Rect dbp_row(int i) { SDL_Rect r = { DB_X0 + 16, dbp_y0() + 76 + i * DB_ROW_H, DB_X1 - DB_X0 - 32, DB_ROW_H - 1 }; return r; }
static SDL_Rect dbp_btn(int i) {                 /* 0 = Choose, 1 = Cancel */
    int w = (DB_X1 - DB_X0 - 30) / 2;
    SDL_Rect r = { DB_X0 + 10 + i * (w + 10), dbp_y0() + dbp_h() - 36, w, 26 }; return r;
}
static void dbp_date(int i, char *out, size_t n) {   /* "Wednesday 16th July 1997" */
    static const char *const wd[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const mo[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                      "September", "October", "November", "December" };
    const char *name; int d, m, y; out[0] = 0;
    if (!db_info(i, &name, &d, &m, &y) || m < 1 || m > 12) return;
    int yy = y, mm = m; if (mm < 3) { mm += 12; yy--; }                     /* Zeller: 0 = Saturday */
    int h = (d + 13 * (mm + 1) / 5 + yy % 100 + (yy % 100) / 4 + (yy / 100) / 4 + 5 * (yy / 100)) % 7;
    const char *suf = (d % 100 >= 11 && d % 100 <= 13) ? "th" : d % 10 == 1 ? "st" : d % 10 == 2 ? "nd" : d % 10 == 3 ? "rd" : "th";
    snprintf(out, n, "%s %d%s %s %d", wd[(h + 6) % 7], d, suf, mo[m - 1], y);
}
static const char *dbp_title = "Choose Database", *dbp_choose = "Choose";
/* the same panel for any short list (the Database Editor's Load Database): two columns of text */
static const char *const *lst_c1, *const *lst_c2; static const char *lst_h1, *lst_h2;
static void dbp_draw(float k, int ox, int oy, int pw, int ph) {
    mk = k; mox = ox; moy = oy;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect all = { ox, oy, pw, ph }; SDL_SetRenderDrawColor(ren, 0, 0, 0, 140); SDL_RenderFillRect(ren, &all);
    const SDL_Color yellow = { 255, 255, 40, 255 }, white = { 255, 255, 255, 255 }, grey = { 215, 215, 215, 255 },
                    dark = { 0, 0, 0, 255 }, hint = { 170, 170, 170, 255 };
    int W = DB_X1 - DB_X0, Y0 = dbp_y0(), H = dbp_h();
    mfill(DB_X0, Y0, W, H, 0, 0, 0, 255);
    mfill(DB_X0 + 1, Y0 + 1, W - 2, H - 2, 52, 52, 52, 245);
    mfill(DB_X0 + 1, Y0 + 1, W - 2, 33, 0, 72, 0, 255);
    mfill(DB_X0 + 1, Y0 + 34, W - 2, 1, 0, 0, 0, 255);
    mtext(dbp_title, DB_X0, Y0, W, 35, 19, yellow, 1, 1, 1);
    mtext(lst_c1 ? lst_h1 : "Database", DB_X0 + 22, Y0 + 52, 200, 15, 9, grey, 1, 0, 0);
    mtext(lst_c1 ? lst_h2 : "First day of a new game", DB_X0 + 22 + 220, Y0 + 52, 200, 15, 9, grey, 1, 0, 0);
    mfill(DB_X0 + 16, Y0 + 70, W - 32, 1, 110, 110, 110, 255);
    for (int i = 0; i < DB_ROWS && dbp_top + i < dbp_n; i++) {
        int j = dbp_top + i; SDL_Rect r = dbp_row(i); const char *name; int d, m, y; char date[64];
        if (lst_c1) { name = lst_c1[j]; snprintf(date, sizeof date, "%s", lst_c2[j]); }
        else { if (!db_info(j, &name, &d, &m, &y)) continue; dbp_date(j, date, sizeof date); }
        if (j == dbp_sel) mfill(r.x, r.y, r.w, r.h, 170, 170, 170, 255);
        else if (in_rect(r, bk_mx, bk_my)) mfill(r.x, r.y, r.w, r.h, 80, 80, 80, 255);
        SDL_Color c = j == dbp_sel ? dark : white;
        mtext(name, r.x + 6, r.y, 210, r.h, 10, c, 0, 0, 0);
        mtext(date, r.x + 226, r.y, r.w - 232, r.h, 10, c, 0, 0, 0);
    }
    if (dbp_n > DB_ROWS) {
        char line[64]; snprintf(line, sizeof line, "%d-%d of %d  (scroll for more)", dbp_top + 1,
                                dbp_top + DB_ROWS < dbp_n ? dbp_top + DB_ROWS : dbp_n, dbp_n);
        mtext(line, DB_X0 + 22, Y0 + 78 + DB_ROWS * DB_ROW_H, W - 36, 14, 8, hint, 0, 0, 0);
    }
    for (int i = 0; i < 2; i++) {
        SDL_Rect b = dbp_btn(i);
        mbevel(b, in_rect(b, bk_mx, bk_my) ? 120 : 100);
        mtext(i ? "Cancel" : dbp_choose, b.x, b.y, b.w, b.h, 13, white, 0, 1, 1);
    }
}
/* the panel: returns the database chosen (database.c's number), or -1 */
static int dbp_run_n(const char *title, const char *choose, int n, int was);
static int dbp_run(const char *title, const char *choose) { lst_c1 = 0; return dbp_run_n(title, choose, db_count(), db_selected()); }
int host_pick_list(const char *title, const char *choose, const char *head1, const char *head2,
                   const char *const *col1, const char *const *col2, int n, int sel) {
    lst_c1 = col1; lst_c2 = col2; lst_h1 = head1; lst_h2 = head2;
    int r = dbp_run_n(title, choose, n, sel);
    lst_c1 = 0; return r;
}
static int dbp_run_n(const char *title, const char *choose, int n, int was) {
    dbp_title = title; dbp_choose = choose;
    dbp_n = n;
    int chosen = -1;
    if (dbp_n < 1) return -1;
    dbp_sel = was >= 0 ? was : 0; dbp_top = dbp_sel >= DB_ROWS ? dbp_sel - DB_ROWS + 1 : 0;
    dbp_open = 1; bk_mx = bk_my = -1;
    SDL_SetCursor(SDL_GetDefaultCursor()); SDL_ShowCursor(SDL_ENABLE);
    present();
    while (dbp_open) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) do {
            switch (e.type) {
            case SDL_QUIT:
                fprintf(stderr, "window closed - exiting\n");
                if (rec) fclose(rec);
                SDL_Quit(); exit(0);
            case SDL_KEYDOWN:
                if (shortcut_key(&e.key) || e.key.repeat) break;
                if (e.key.keysym.sym == SDLK_ESCAPE) dbp_open = 0;
                else if (e.key.keysym.sym == SDLK_UP && dbp_sel > 0) dbp_sel--;
                else if (e.key.keysym.sym == SDLK_DOWN && dbp_sel + 1 < dbp_n) dbp_sel++;
                else if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) { chosen = dbp_sel; dbp_open = 0; }
                if (dbp_sel < dbp_top) dbp_top = dbp_sel; if (dbp_sel >= dbp_top + DB_ROWS) dbp_top = dbp_sel - DB_ROWS + 1;
                break;
            case SDL_MOUSEMOTION: bk_mx = e.motion.x; bk_my = e.motion.y; break;
            case SDL_MOUSEWHEEL:
                dbp_top -= e.wheel.y; if (dbp_top > dbp_n - DB_ROWS) dbp_top = dbp_n - DB_ROWS; if (dbp_top < 0) dbp_top = 0;
                break;
            case SDL_MOUSEBUTTONDOWN: {
                if (e.button.button != SDL_BUTTON_LEFT) break;
                int x = e.button.x, y = e.button.y;
                for (int i = 0; i < DB_ROWS && dbp_top + i < dbp_n; i++) if (in_rect(dbp_row(i), x, y)) {
                    int j = dbp_top + i;
                    if (j == dbp_sel && e.button.clicks >= 2) { chosen = j; dbp_open = 0; } else dbp_sel = j;
                }
                if (in_rect(dbp_btn(0), x, y)) { chosen = dbp_sel; dbp_open = 0; }
                if (in_rect(dbp_btn(1), x, y)) dbp_open = 0;
                if (x < DB_X0 || x >= DB_X1 || y < dbp_y0() || y >= dbp_y0() + dbp_h()) dbp_open = 0;
                break; }
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_title();
                break;
            default: break;
            }
        } while (dbp_open && SDL_PollEvent(&e));
        present();
    }
    os_cursor_key = 0; os_cursor_visible = -1;
    SDL_PumpEvents(); SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);
    present();
    return chosen;
}
/* ---- a report: lines of text to scroll through, and OK (the Database Editor's Test Database) ---- */
#define RP_X0 24
#define RP_X1 616
#define RP_Y0 40
#define RP_ROWS 18
#define RP_ROW_H 18
static const char *rp_title; static char **rp_lines; static int rp_n, rp_top;
static SDL_Rect rp_btn(void) { SDL_Rect r = { 320 - 90, RP_Y0 + 70 + RP_ROWS * RP_ROW_H + 12, 180, 28 }; return r; }
static int rp_bottom(void) { SDL_Rect b = rp_btn(); return b.y + b.h + 10; }
static void rp_draw(float k, int ox, int oy, int pw, int ph) {
    mk = k; mox = ox; moy = oy;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_Rect all = { ox, oy, pw, ph }; SDL_SetRenderDrawColor(ren, 0, 0, 0, 140); SDL_RenderFillRect(ren, &all);
    const SDL_Color yellow = { 255, 255, 40, 255 }, white = { 255, 255, 255, 255 }, red = { 255, 120, 110, 255 },
                    green = { 140, 230, 140, 255 }, hint = { 170, 170, 170, 255 };
    int W = RP_X1 - RP_X0, H = rp_bottom() - RP_Y0;
    mfill(RP_X0, RP_Y0, W, H, 0, 0, 0, 255);
    mfill(RP_X0 + 1, RP_Y0 + 1, W - 2, H - 2, 52, 52, 52, 245);
    mfill(RP_X0 + 1, RP_Y0 + 1, W - 2, 33, 0, 72, 0, 255);
    mfill(RP_X0 + 1, RP_Y0 + 34, W - 2, 1, 0, 0, 0, 255);
    mtext(rp_title, RP_X0, RP_Y0, W, 35, 19, yellow, 1, 1, 1);
    for (int i = 0; i < RP_ROWS && rp_top + i < rp_n; i++) {
        const char *t = rp_lines[rp_top + i];
        SDL_Color c = !strncmp(t, "No problems", 11) ? green : strstr(t, "problem") && t[0] != ' ' ? red : t[0] == ' ' || strstr(t, "note") ? white : hint;
        mtext(t, RP_X0 + 16, RP_Y0 + 46 + i * RP_ROW_H, W - 32, RP_ROW_H, 9, c, t[0] != ' ', 0, 0);
    }
    if (rp_n > RP_ROWS) {
        char l[64]; snprintf(l, sizeof l, "%d-%d of %d lines  (scroll for more)", rp_top + 1, rp_top + RP_ROWS < rp_n ? rp_top + RP_ROWS : rp_n, rp_n);
        mtext(l, RP_X0 + 16, RP_Y0 + 50 + RP_ROWS * RP_ROW_H, W - 32, 14, 8, hint, 0, 0, 0);
    }
    SDL_Rect b = rp_btn(); mbevel(b, in_rect(b, bk_mx, bk_my) ? 120 : 100);
    mtext("OK", b.x, b.y, b.w, b.h, 13, white, 0, 1, 1);
}
void host_report(const char *title, char **lines, int n) {
    rp_title = title; rp_lines = lines; rp_n = n; rp_top = 0; rp_open = 1; bk_mx = bk_my = -1;
    SDL_SetCursor(SDL_GetDefaultCursor()); SDL_ShowCursor(SDL_ENABLE);
    present();
    while (rp_open) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) do {
            switch (e.type) {
            case SDL_QUIT: fprintf(stderr, "window closed - exiting\n"); if (rec) fclose(rec); SDL_Quit(); exit(0);
            case SDL_KEYDOWN:
                if (shortcut_key(&e.key) || e.key.repeat) break;
                if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) rp_open = 0;
                else if (e.key.keysym.sym == SDLK_UP) rp_top--;
                else if (e.key.keysym.sym == SDLK_DOWN) rp_top++;
                else if (e.key.keysym.sym == SDLK_PAGEUP) rp_top -= RP_ROWS;
                else if (e.key.keysym.sym == SDLK_PAGEDOWN) rp_top += RP_ROWS;
                break;
            case SDL_MOUSEMOTION: bk_mx = e.motion.x; bk_my = e.motion.y; break;
            case SDL_MOUSEWHEEL: rp_top -= e.wheel.y * 3; break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT && in_rect(rp_btn(), e.button.x, e.button.y)) rp_open = 0;
                break;
            case SDL_WINDOWEVENT: if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_title(); break;
            default: break;
            }
            if (rp_top > rp_n - RP_ROWS) rp_top = rp_n - RP_ROWS;
            if (rp_top < 0) rp_top = 0;
        } while (rp_open && SDL_PollEvent(&e));
        present();
    }
    os_cursor_key = 0; os_cursor_visible = -1;
    SDL_PumpEvents(); SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);
    present();
}
/* Change Database: returns 1 if another database was chosen */
int host_choose_database(void) {
    int was = db_selected(), chosen = dbp_run("Choose Database", "Choose");
    if (chosen >= 0 && chosen != was) db_select(chosen);
    return chosen >= 0 && chosen != was;
}
/* Database Editor: which database to edit (the game's choice stays as it is); -1 = cancelled */
int host_pick_database(void) { return dbp_run("Edit Which Database?", "Edit"); }

void host_restore_backup(void) { bk_run(); }     /* overrides.c: the Options screen's Restore Backup */

/* ---- player screen: the star rating in the blue position bar (rt/stars.c) ----
   Drawn as shapes at the window's own resolution, so it is sharp at any size: a full star per two
   points, a half star (left half filled, right half outlined) for an odd one, then empty (outlined)
   ones up to five, right-aligned in
   the bar, which the game draws at (10,100)-(630,130). */
extern int g_star_player; extern char g_star_text[64];
int player_stars(uint32_t player);
static int star_n;                                 /* rating to show this frame (0 = none) */
static void star_check(const uint8_t *p, int w, int h, int st) {
    int x0, y0, x1, y1;
    star_n = 0;
    if (!g_show_stars || w != 640 || h != 480 || g_star_player < 0 || !g_star_text[0]) return;
    if (!text_find(p, st, g_star_text, &x0, &y0, &x1, &y1) || y0 < 98 || y1 > 132) return;
    star_n = player_stars((uint32_t)g_star_player);
}
static void star_poly(float cx, float cy, float R, float px[10], float py[10]) {
    for (int i = 0; i < 10; i++) {
        float a = -1.5707963f + i * 0.6283185f, r = i & 1 ? R * 0.40f : R;
        px[i] = cx + r * cosf(a); py[i] = cy + r * sinf(a);
    }
}
/* fill the star, only where x < xmax (scanlines, even-odd) */
static void star_fill(const float px[10], const float py[10], float xmax, uint8_t r, uint8_t g, uint8_t b) {
    float top = py[0], bot = py[0];
    for (int i = 1; i < 10; i++) { if (py[i] < top) top = py[i]; if (py[i] > bot) bot = py[i]; }
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for (int y = (int)top; y <= (int)bot + 1; y++) {
        float yc = y + 0.5f, xs[10]; int n = 0;
        for (int i = 0; i < 10; i++) {
            int j = (i + 1) % 10; float ya = py[i], yb = py[j];
            if ((ya <= yc && yb > yc) || (yb <= yc && ya > yc)) xs[n++] = px[i] + (yc - ya) * (px[j] - px[i]) / (yb - ya);
        }
        for (int a = 0; a < n; a++) for (int c = a + 1; c < n; c++) if (xs[c] < xs[a]) { float t = xs[a]; xs[a] = xs[c]; xs[c] = t; }
        for (int a = 0; a + 1 < n; a += 2) {
            float l = xs[a], rr = xs[a + 1] < xmax ? xs[a + 1] : xmax;
            if (rr > l) { SDL_Rect q = { (int)(l + 0.5f), y, (int)(rr + 0.5f) - (int)(l + 0.5f), 1 }; if (q.w > 0) SDL_RenderFillRect(ren, &q); }
        }
    }
}
static void star_outline(const float px[10], const float py[10], float thick, uint8_t r, uint8_t g, uint8_t b) {
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    int t = (int)(thick + 0.5f); if (t < 1) t = 1;
    for (int i = 0; i < 10; i++) { int j = (i + 1) % 10;
        for (int dx = 0; dx < t; dx++) for (int dy = 0; dy < t; dy++)
            SDL_RenderDrawLine(ren, (int)px[i] + dx - t / 2, (int)py[i] + dy - t / 2, (int)px[j] + dx - t / 2, (int)py[j] + dy - t / 2); }
}
static void star_draw(float k, int ox, int oy) {
    if (!star_n) return;
    int full = star_n / 2, half = star_n % 2, n = 5;      /* always five: full, then half, then empty */
    float R = 8.0f * k, gap = 18.0f * k, cy = oy + 116.0f * k, sh = k > 1 ? k : 1, t = k * 0.8f;
    for (int i = 0; i < n; i++) {
        float cx = ox + 612.0f * k - (n - 1 - i) * gap, px[10], py[10], qx[10], qy[10];
        star_poly(cx, cy, R, px, py); star_poly(cx + sh, cy + sh, R, qx, qy);
        if (i < full) { star_fill(qx, qy, 1e9f, 0, 0, 0); star_fill(px, py, 1e9f, 255, 204, 0); }
        else if (i == full && half) {                       /* left half filled, right half outlined */
            star_outline(qx, qy, t, 0, 0, 0); star_fill(qx, qy, cx + sh, 0, 0, 0);
            star_fill(px, py, cx, 255, 204, 0); star_outline(px, py, t, 255, 204, 0);
        } else { star_outline(qx, qy, t, 0, 0, 0); star_outline(px, py, t, 255, 204, 0); }   /* empty */
    }
}

static void present(void) {
    const uint8_t *p; int w, h, st; uint8_t pal[256][3];
    if (!video_frame(&p, &w, &h, &st, pal)) { SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren); SDL_RenderPresent(ren); return; }
    if (!tex || w != tex_w || h != tex_h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w; tex_h = h; SDL_RenderSetLogicalSize(ren, w, h);
    }
    uint32_t lut[256];
    for (int i = 0; i < 256; i++) lut[i] = 0xFF000000u | (pal[i][0] << 16) | (pal[i][1] << 8) | pal[i][2];
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) rgb[y * w + x] = lut[p[y * st + x]];
    int sharp = text_sharp && sharp_available();
    if (sharp) sharp_erase(rgb, p, w, h, st, lut);
    speed_buttons(rgb, p, w, h, st);
    db_label(rgb, p, w, h, st, sharp);
    star_check(p, w, h, st);
    text_dim_clear();
    grey_leagues(rgb, p, w, h, st, sharp);
    { int x0, y0, x1, y1; players_ui = w == 640 && h == 480 && text_find(p, st, "Choose Leagues to Run", &x0, &y0, &x1, &y1); }
    stat_colours(rgb, p, w, h, st, sharp);
    SDL_UpdateTexture(tex, NULL, rgb, w * 4);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255); SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    int toast = toast_alpha();
    if (sharp || toast || g_menu_open || bk_open || dbp_open || star_n || players_ui) {
        /* draw in real output pixels: where the scaled picture lies, as SDL placed it */
        int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
        float k = (float)ow / w < (float)oh / h ? (float)ow / w : (float)oh / h;
        if (scale_mode == SCALE_INTEGER) { k = (float)(int)k; if (k < 1) k = 1; }
        int ox = (ow - (int)(w * k)) / 2, oy = (oh - (int)(h * k)) / 2;
        SDL_RenderSetLogicalSize(ren, 0, 0);
        if (sharp) sharp_draw(ren, (const uint8_t (*)[3])pal, k, ox, oy);
        if (sharp && db_on) {                                  /* the database label, sharp */
            struct tfont f; if (text_font_info(1, &f)) {
                SDL_Color sh = { 0, 0, 0, 255 }, fg = { 230, 230, 230, 255 };
                /* centred on the screen by its own width (the sharp font's width differs from the bitmap's) */
                int px = (int)(13 * k + 0.5f), X = ox, Y = oy + (int)(db_y * k), W = (int)(w * k), H = (int)(f.height * k);
                sharp_label(ren, db_text, X + (int)(k + 0.5f), Y + (int)(k + 0.5f), W, H, px, sh, 0, 1);
                sharp_label(ren, db_text, X, Y, W, H, px, fg, 0, 1);
            }
        }
        star_draw(k, ox, oy);
        if (players_ui) players_draw(k, ox, oy);
        if (g_menu_open) menu_draw(k, ox, oy, (int)(w * k), (int)(h * k));
        if (bk_open) bk_draw(k, ox, oy, (int)(w * k), (int)(h * k));
        if (dbp_open) dbp_draw(k, ox, oy, (int)(w * k), (int)(h * k));
        if (rp_open) rp_draw(k, ox, oy, (int)(w * k), (int)(h * k));
        if (shot_pending) shot_capture(ox, oy, (int)(w * k), (int)(h * k));   /* (F9: before the note) */
        if (toast) sharp_draw_note(ren, toast_text, (uint8_t)toast, k, ox, oy, (int)(w * k), (int)(h * k));
        SDL_RenderSetLogicalSize(ren, w, h);
    }
    if (shot_pending) {                                             /* (nothing drawn on top: the picture alone) */
        int ow, oh; SDL_GetRendererOutputSize(ren, &ow, &oh);
        float k = (float)ow / w < (float)oh / h ? (float)ow / w : (float)oh / h;
        if (scale_mode == SCALE_INTEGER) { k = (float)(int)k; if (k < 1) k = 1; }
        SDL_RenderSetLogicalSize(ren, 0, 0);
        shot_capture((ow - (int)(w * k)) / 2, (oh - (int)(h * k)) / 2, (int)(w * k), (int)(h * k));
        SDL_RenderSetLogicalSize(ren, w, h);
    }
    SDL_RenderPresent(ren);
    if (!g_menu_open && !bk_open && !dbp_open) update_pointer();      /* the menus use the normal arrow */
}
/* cheap fingerprint of the current picture (pixels + palette) */
static uint64_t frame_hash(void) {
    const uint8_t *p; int w, h, st; uint8_t pal[256][3];
    if (!video_frame(&p, &w, &h, &st, pal)) return 0;
    uint64_t x = 1469598103934665603ull;
    for (int y = 0; y < h; y++) { const uint64_t *row = (const uint64_t *)(p + (size_t)y * st);
        for (int i = 0; i < w / 8; i++) { x ^= row[i]; x *= 1099511628211ull; } }
    for (int i = 0; i < 768; i++) { x ^= ((uint8_t *)pal)[i]; x *= 1099511628211ull; }
    return x;
}
static uint64_t presented_hash, settle_hash; static uint32_t last_settle_check;
void host_present(void) { present(); last_present = SDL_GetTicks(); presented_hash = frame_hash(); }
/* show the picture once it has stopped changing: catches screens the game holds without
   waiting on input or the clock (e.g. a pressed button), but never a half-drawn one */
static void present_if_settled(uint32_t now) {
    if (now - last_settle_check < PRESENT_IDLE_MS) return;
    last_settle_check = now;
    uint64_t h = frame_hash();
    if (h == settle_hash && h != presented_hash) host_present();
    settle_hash = h;
}

/* SDL key -> BIOS (scan << 8 | ascii) */
static uint16_t bios_key(SDL_Keysym k) {
    int shift = (k.mod & KMOD_SHIFT) != 0, ctrl = (k.mod & KMOD_CTRL) != 0, alt = (k.mod & KMOD_ALT) != 0;
    static const struct { SDL_Keycode sym; uint8_t scan; } sc[] = {
        {SDLK_a,0x1E},{SDLK_b,0x30},{SDLK_c,0x2E},{SDLK_d,0x20},{SDLK_e,0x12},{SDLK_f,0x21},{SDLK_g,0x22},{SDLK_h,0x23},
        {SDLK_i,0x17},{SDLK_j,0x24},{SDLK_k,0x25},{SDLK_l,0x26},{SDLK_m,0x32},{SDLK_n,0x31},{SDLK_o,0x18},{SDLK_p,0x19},
        {SDLK_q,0x10},{SDLK_r,0x13},{SDLK_s,0x1F},{SDLK_t,0x14},{SDLK_u,0x16},{SDLK_v,0x2F},{SDLK_w,0x11},{SDLK_x,0x2D},
        {SDLK_y,0x15},{SDLK_z,0x2C},{SDLK_1,0x02},{SDLK_2,0x03},{SDLK_3,0x04},{SDLK_4,0x05},{SDLK_5,0x06},{SDLK_6,0x07},
        {SDLK_7,0x08},{SDLK_8,0x09},{SDLK_9,0x0A},{SDLK_0,0x0B},{SDLK_MINUS,0x0C},{SDLK_EQUALS,0x0D},{SDLK_SPACE,0x39},
        {SDLK_COMMA,0x33},{SDLK_PERIOD,0x34},{SDLK_SLASH,0x35},{SDLK_SEMICOLON,0x27},{SDLK_QUOTE,0x28},
        {SDLK_LEFTBRACKET,0x1A},{SDLK_RIGHTBRACKET,0x1B},{SDLK_BACKSLASH,0x2B},{SDLK_BACKQUOTE,0x29} };
    static const char shifted_digits[] = ")!\"#$%^&*(";
    switch (k.sym) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return 0x1C0D;
    case SDLK_ESCAPE: return 0x011B;
    case SDLK_BACKSPACE: return 0x0E08;
    case SDLK_TAB: return shift ? 0x0F00 : 0x0F09;
    case SDLK_UP: return 0x4800; case SDLK_DOWN: return 0x5000; case SDLK_LEFT: return 0x4B00; case SDLK_RIGHT: return 0x4D00;
    case SDLK_HOME: return 0x4700; case SDLK_END: return 0x4F00; case SDLK_PAGEUP: return 0x4900; case SDLK_PAGEDOWN: return 0x5100;
    case SDLK_INSERT: return 0x5200; case SDLK_DELETE: return 0x5300;
    default: break;
    }
    if (k.sym >= SDLK_F1 && k.sym <= SDLK_F10) return (uint16_t)((0x3B + (k.sym - SDLK_F1) + (shift ? 0x19 : alt ? 0x2D : ctrl ? 0x23 : 0)) << 8);
    for (unsigned i = 0; i < sizeof sc / sizeof sc[0]; i++) if (sc[i].sym == k.sym) {
        int ch = (int)k.sym;
        if (alt) return (uint16_t)(sc[i].scan << 8);
        if (ch >= 'a' && ch <= 'z') { if (ctrl) ch = ch - 'a' + 1; else if (shift ^ ((k.mod & KMOD_CAPS) != 0)) ch -= 32; }
        else if (shift && ch >= '0' && ch <= '9') ch = shifted_digits[ch - '0'];
        else if (shift) { const char *a = "-=,./;'[]\\`", *b = "_+<>?:@{}|~"; const char *q = strchr(a, ch); if (q) ch = b[q - a]; }
        return (uint16_t)((sc[i].scan << 8) | (ch & 0xFF));
    }
    return 0;
}

/* collect SDL events into the pending queue (safe to call at any interrupt) */
/* F9 / F10 / F11 (Alt+Enter) / F12: work everywhere, menu open or not; 1 = handled */
static int shortcut_key(const SDL_KeyboardEvent *k) {
    SDL_Keycode s = k->keysym.sym;
    if ((s == SDLK_RETURN && (k->keysym.mod & KMOD_ALT)) || s == SDLK_F11) {     /* fullscreen */
        if (!k->repeat) { want_fullscreen = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP); apply_fullscreen(); cfg_save(); }
        return 1; }
    if (s == SDLK_F9) { if (!k->repeat) save_screenshot(); return 1; }                                   /* screenshot */
    if (s == SDLK_F10) { if (!k->repeat) { text_sharp = !text_sharp; cfg_save(); update_title(); present(); } return 1; }
    if (s == SDLK_F12) { if (!k->repeat) { scale_mode = (scale_mode + 1) % SCALE_MODES; apply_scaling(); cfg_save(); present(); } return 1; }
    return 0;
}

/* the menu's own event loop: the game waits here until the menu is closed */
/* the Volume slider: where the pointer is along its track (menu units) -> 0-100 */
static void vol_set(int x) {
    SDL_Rect b = menu_button(M_VOLUME, 0); int tw = b.w - 44;
    int v = (x - b.x) * 100 / (tw > 0 ? tw : 1);
    g_sound_volume = v < 0 ? 0 : v > 100 ? 100 : v;
}
static void menu_run(void) {
    g_menu_open = 1; menu_mx = menu_my = -1;
    cm2_bg_ok = cm2_backgrounds_available(); snd_ok = sound_available();
    SDL_SetCursor(SDL_GetDefaultCursor()); SDL_ShowCursor(SDL_ENABLE);
    present();
    while (g_menu_open) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) do {
            switch (e.type) {
            case SDL_QUIT:
                fprintf(stderr, "window closed - exiting (progress since your last save is lost, as in DOS)\n");
                if (rec) fclose(rec);
                SDL_Quit(); exit(0);
            case SDL_KEYDOWN:
                if (shortcut_key(&e.key)) break;
                if (!e.key.repeat && (e.key.keysym.sym == SDLK_F1 || e.key.keysym.sym == SDLK_ESCAPE ||
                                      e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER)) g_menu_open = 0;
                break;
            case SDL_MOUSEMOTION: menu_mx = e.motion.x; menu_my = e.motion.y; if (vol_drag) vol_set(menu_mx); break;
            case SDL_MOUSEBUTTONDOWN:
                if (e.button.button == SDL_BUTTON_LEFT) { int r, i; if (menu_hit(e.button.x, e.button.y, &r, &i)) {
                    if (r == M_VOLUME) { if (!menu_disabled(r, 0) && menu_tab == 1) { vol_drag = 1; vol_set(e.button.x); } }
                    else menu_choose(r, i); } }
                break;
            case SDL_MOUSEBUTTONUP:
                if (e.button.button == SDL_BUTTON_LEFT && vol_drag) { vol_drag = 0; cfg_save(); }
                break;
            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_title();
                break;
            default: break;
            }
        } while (g_menu_open && SDL_PollEvent(&e));
        present();
    }
    /* give the game its own pointer back, and swallow the click that closed the menu */
    os_cursor_key = 0; os_cursor_visible = -1;
    SDL_PumpEvents(); SDL_FlushEvents(SDL_MOUSEBUTTONDOWN, SDL_MOUSEBUTTONUP);
    present();
}

static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        int grace = SDL_GetTicks() - start_ticks < STARTUP_GRACE_MS;
        if (grace && (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP)) continue;
        switch (e.type) {
        case SDL_QUIT:
            fprintf(stderr, "window closed - exiting (progress since your last save is lost, as in DOS)\n");
            if (rec) fclose(rec);
            SDL_Quit(); exit(0);
        case SDL_MOUSEMOTION:
            if (players_drag) players_set(e.motion.x);
            push_ev(EV_MOVE, e.motion.x, e.motion.y); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: {
            /* the Players slider on Choose Leagues to Run is the port's */
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (e.type == SDL_MOUSEBUTTONUP && players_drag) { players_drag = 0; cfg_save(); present(); break; }
                if (e.type == SDL_MOUSEBUTTONDOWN && players_hit(e.button.x, e.button.y)) { players_drag = 1; players_set(e.button.x); present(); break; }
            }
            /* clicks on the match speed buttons are the port's, not the game's */
            if (e.button.button == SDL_BUTTON_LEFT) {
                if (e.type == SDL_MOUSEBUTTONUP && speed_swallow_up) { speed_swallow_up = 0; break; }
                int sp = e.type == SDL_MOUSEBUTTONDOWN ? speed_hit(e.button.x, e.button.y) : 0;
                if (sp) { host_set_match_speed(sp); match_speed_apply(); speed_swallow_up = 1; present(); break; }
            }
            if (grey_hit(e.button.x, e.button.y)) break;          /* a greyed-out league: not clickable */
            int b = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : e.button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
            push_ev(EV_MOVE, e.button.x, e.button.y);
            if (b >= 0) push_ev(e.type == SDL_MOUSEBUTTONDOWN ? EV_DOWN : EV_UP, b, 0);
            break; }
        case SDL_KEYDOWN:
            if (shortcut_key(&e.key)) break;
            if (e.key.keysym.sym == SDLK_F1) { if (!e.key.repeat) menu_run(); break; }   /* F1: options menu */
            if (e.key.repeat && (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER || e.key.keysym.sym == SDLK_ESCAPE)) break;
            { uint16_t k = bios_key(e.key.keysym); if (k) push_ev(EV_KEY, k, 0); }
            break;
        case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) update_title();
            if (e.window.event == SDL_WINDOWEVENT_EXPOSED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) present();
            break;
        default: break;
        }
    }
    last_pump = SDL_GetTicks();
}

/* the game polls keyboard/mouse: deliver pending input here (deterministic point) */
void host_poll(void) {
    uint32_t now = SDL_GetTicks();
    if (now - last_pump >= 4) pump();
    apply_pending();
    input_dispatch_mouse();
    if (now - last_present >= 16) host_present();
}

/* loop back-edge in guest code: deliver input asynchronously, like a hardware IRQ */
void host_async(void) {
    uint32_t now = SDL_GetTicks();
    if (now - last_pump >= 4) pump();
    apply_pending();
    input_dispatch_mouse();
    if (now - last_present >= PRESENT_FALLBACK_MS || (toast_on && now - last_present >= 33)) host_present();
    else present_if_settled(now);
}
/* the game read the clock (typically while waiting, e.g. between commentary lines) */
void host_idle(void) {
    uint32_t now = SDL_GetTicks();
    if (now - last_pump >= 4) pump();
    if (now - last_present >= PRESENT_IDLE_MS) host_present();
}
/* any interrupt: keep the window alive during long calculations (collect only) */
void host_tick(void) {
    uint32_t now = SDL_GetTicks();
    if (now - last_pump >= 30) { pump(); if (now - last_present >= PRESENT_FALLBACK_MS) host_present(); }
    if (toast_on && now - last_present >= 33) host_present(); else present_if_settled(now);
}
void host_sleep_ms(uint32_t ms) { SDL_Delay(ms); }

/* error/information dialog; works before SDL_Init and without a window */
void host_message(const char *title, const char *text) {
    fprintf(stderr, "%s\n%s\n", title, text);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, win);
}

/* the game is blocked waiting for a key */
void host_wait(void) {
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, 16)) SDL_PushEvent(&e);
    pump(); apply_pending(); input_dispatch_mouse(); host_present();
}

/* yes/no question (autosave.c: load the newer autosave?) */
int host_ask(const char *title, const char *text) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
    };
    SDL_MessageBoxData box = { SDL_MESSAGEBOX_INFORMATION, win, title, text, 2, buttons, NULL };
    int choice = 0;
    if (SDL_ShowMessageBox(&box, &choice) != 0) return 0;
    return choice == 1;
}
