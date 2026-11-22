/* setup.c - the installer window: sets up a folder for the CM 97/98 port from the player's own
 * copy of the game, optionally adding Championship Manager 2 (install.c, cm2import.c).
 *
 * Folders can be typed or pasted (Ctrl+V), chosen with "Browse..." (the Windows folder dialog;
 * zenity or kdialog on Linux) or dropped onto the window - a dropped folder goes to the box it
 * belongs in (a CM 97/98 folder, a CM2 folder, a 96/97 European Leagues edition, or else the folder
 * to install to); a folder holding several of them (the CM2 CD's folders, say) fills in each.
 * Three tabs: Base Game (97/98 and where to set it up), CM2 (1995/96, with the six European Leagues
 * editions) and CM 96/97 (the 96/97 Update, with Italian Leagues and Liga Portuguesa) - each edition
 * adds its country's leagues in full to its season's database.
 *
 *   cm9798setup                         the window
 *   cm9798setup --cli <cm9798> <target> [<cm2> [<cm96/97> [<edition>...]]]   the same, without a window
 *                                       ("" skips one; editions in any order, up to seven)
 */
#include "install.h"
#include "cm2import.h"
#include "font_data.h"
#include "setupicon_data.h"
#define SDL_MAIN_HANDLED                     /* our own main() */
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <unistd.h>
#include <dirent.h>
#endif

#define W 760
#define H 700

static SDL_Window *win; static SDL_Renderer *ren;

/* ------------------------------------------------------------------ text */
typedef struct { SDL_Texture *t[224]; const unsigned char *px; const unsigned short *w; const unsigned int *off; int h; } Font;
static Font body, title;
static void font_init(Font *f, const unsigned char *px, const unsigned short *w, const unsigned int *off, int h) {
    f->px = px; f->w = w; f->off = off; f->h = h;
    for (int c = 0; c < 224; c++) {
        f->t[c] = NULL; if (!w[c]) continue;
        SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, w[c], h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (!s) continue;
        for (int y = 0; y < h; y++) for (int x = 0; x < w[c]; x++)
            ((Uint32 *)((Uint8 *)s->pixels + y * s->pitch))[x] = (Uint32)px[off[c] + y * w[c] + x] << 24 | 0xFFFFFFu;
        f->t[c] = SDL_CreateTextureFromSurface(ren, s);
        if (f->t[c]) SDL_SetTextureBlendMode(f->t[c], SDL_BLENDMODE_BLEND);
        SDL_FreeSurface(s);
    }
}
/* UTF-8 (what SDL gives) -> Latin-1, which the fonts have; anything else becomes '?' */
static int next_char(const char **s) {
    const unsigned char *p = (const unsigned char *)*s; int c = *p;
    if (c < 0x80) { *s += 1; return c; }
    if ((c & 0xE0) == 0xC0 && p[1]) { int u = (c & 0x1F) << 6 | (p[1] & 0x3F); *s += 2; return u < 256 ? u : '?'; }
    int n = (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    for (int i = 1; i < n && p[i]; i++) (*s)++;
    *s += 1; return '?';
}
static int text_w(const Font *f, const char *s) {
    int w = 0; while (*s) { int c = next_char(&s); if (c >= 32) w += f->w[c - 32]; }
    return w;
}
static void text(const Font *f, int x, int y, const char *s, SDL_Color col) {
    while (*s) {
        int c = next_char(&s); if (c < 32) continue;
        SDL_Texture *t = f->t[c - 32]; int w = f->w[c - 32];
        if (t) { SDL_SetTextureColorMod(t, col.r, col.g, col.b); SDL_Rect r = { x, y, w, f->h }; SDL_RenderCopy(ren, t, NULL, &r); }
        x += w;
    }
}
/* the end of a long path, so the part that matters shows */
static void text_fit(const Font *f, int x, int y, int maxw, const char *s, SDL_Color col) {
    if (text_w(f, s) <= maxw) { text(f, x, y, s, col); return; }
    const char *p = s; char buf[1300];
    while (*p) { next_char(&p); snprintf(buf, sizeof buf, "...%s", p); if (text_w(f, buf) <= maxw) break; }
    text(f, x, y, buf, col);
}

/* ------------------------------------------------------------------ colours, boxes */
static const SDL_Color c_text = { 235, 235, 235, 255 }, c_dim = { 150, 150, 150, 255 }, c_ok = { 120, 220, 120, 255 },
                       c_bad = { 240, 120, 100, 255 }, c_head = { 255, 230, 60, 255 };
static void fill(int x, int y, int w, int h, int r, int g, int b) { SDL_SetRenderDrawColor(ren, (Uint8)r, (Uint8)g, (Uint8)b, 255); SDL_Rect q = { x, y, w, h }; SDL_RenderFillRect(ren, &q); }
static void frame(int x, int y, int w, int h, int r, int g, int b) { SDL_SetRenderDrawColor(ren, (Uint8)r, (Uint8)g, (Uint8)b, 255); SDL_Rect q = { x, y, w, h }; SDL_RenderDrawRect(ren, &q); }
static int inside(int mx, int my, SDL_Rect r) { return mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h; }

typedef struct { SDL_Rect r; const char *label; int enabled; } Button;
static int hover_x = -1, hover_y = -1;
static void button(const Button *b) {
    int hot = b->enabled && inside(hover_x, hover_y, b->r);
    fill(b->r.x, b->r.y, b->r.w, b->r.h, hot ? 70 : 52, hot ? 92 : 70, hot ? 70 : 52);
    frame(b->r.x, b->r.y, b->r.w, b->r.h, 110, 130, 110);
    text(&body, b->r.x + (b->r.w - text_w(&body, b->label)) / 2, b->r.y + (b->r.h - body.h) / 2, b->label, b->enabled ? c_text : c_dim);
}

/* ------------------------------------------------------------------ the form */
enum { F_SRC, F_DST, F_CM2, F_CM96, F_EU0, NF = F_EU0 + INSTALL_NEDITIONS };   /* F_EU0 + k: install_edition(k) */
enum { T_BASE, T_CM2, T_9697, NT };
static const char *const tab_name[NT] = { "Base Game", "CM2", "CM 96/97" };
static int tab; static SDL_Rect tab_r[NT];
static int field_tab(int i) {
    if (i <= F_DST) return T_BASE;
    if (i >= F_EU0) return install_edition(i - F_EU0)->season == 1995 ? T_CM2 : T_9697;
    return i == F_CM2 ? T_CM2 : T_9697;
}
static int row_in_tab(int i) { int r = 0; for (int j = F_EU0; j < i; j++) if (field_tab(j) == field_tab(i)) r++; return r; }
static const char *season_name(int y) { return y == 1995 ? "1995/96" : "1996/97"; }
static struct { char label[80], value[1100], status[300]; int ok; SDL_Rect box; Button browse; } field[NF];
static int focus = -1;
static char base_dir[1100];

static void check_fields(void) {
    field[F_SRC].ok = install_check_source(field[F_SRC].value, field[F_SRC].status, sizeof field[F_SRC].status);
    if (!field[F_SRC].value[0] && install_has_base(field[F_DST].value)) {      /* importing into a game already set up */
        field[F_SRC].ok = 1;
        snprintf(field[F_SRC].status, sizeof field[F_SRC].status, "Not needed - the folder below is already set up. Just add the data to import.");
    }
    for (int i = F_CM2; i <= F_CM96; i++) {                          /* optional; each its own season */
        field[i].ok = install_check_cm2(field[i].value, field[i].status, sizeof field[i].status);
        if (!field[i].value[0]) { field[i].ok = 1; continue; }
        int y, d, m, want = i == F_CM2 ? 1995 : 1996;
        if (field[i].ok && cm2_detect(field[i].value, &y, &d, &m) && y != want) {
            field[i].ok = 0;
            snprintf(field[i].status, sizeof field[i].status, "That's the %s game - it goes on the %s tab.", season_name(y), y == 1995 ? "CM2" : "CM 96/97"); }
    }
    for (int k = 0; k < INSTALL_NEDITIONS; k++) {                    /* optional; each in its own box */
        int i = F_EU0 + k; char *st = field[i].status; size_t n = sizeof field[i].status;
        const struct install_edition *e = install_edition(k);
        static char seen[INSTALL_NEDITIONS][1100], seen_st[INSTALL_NEDITIONS][300];
        static int seen_ok[INSTALL_NEDITIONS], seen_r[INSTALL_NEDITIONS], seen_any[INSTALL_NEDITIONS];
        if (!seen_any[k] || strcmp(seen[k], field[i].value)) {        /* (reading an edition takes a moment: once per change) */
            seen_any[k] = 1; snprintf(seen[k], sizeof seen[k], "%s", field[i].value);
            seen_ok[k] = install_check_euro(field[i].value, seen_st[k], sizeof seen_st[k]);
            seen_r[k] = seen_ok[k] ? install_euro_release(field[i].value) : -1;
        }
        field[i].ok = seen_ok[k]; snprintf(st, n, "%s", seen_st[k]);
        int base = e->season == 1995 ? F_CM2 : F_CM96;
        if (!field[i].value[0]) { field[i].ok = 1; snprintf(st, n, "Adds the %s leagues in full to the %s database.", e->adjective, season_name(e->season)); }
        else if (field[i].ok && seen_r[k] >= 0 && seen_r[k] != k) {
            const struct install_edition *o = install_edition(seen_r[k]);
            field[i].ok = 0;
            snprintf(st, n, "That's %s - it goes in the %s box%s.", o->name, field[F_EU0 + seen_r[k]].label,
                     field_tab(F_EU0 + seen_r[k]) != field_tab(i) ? (o->season == 1995 ? " on the CM2 tab" : " on the CM 96/97 tab") : ""); }
        else if (field[i].ok && !field[base].value[0])
            snprintf(st, n, "Found - give the %s folder above too: it adds to that database.", base == F_CM2 ? "Championship Manager 2" : "CM2 96/97 Update");
    }
    const char *d = field[F_DST].value;
    field[F_DST].ok = d[0] != 0;
    if (!d[0]) snprintf(field[F_DST].status, sizeof field[F_DST].status, "Choose where to set up the game for the port.");
    else if (field[F_SRC].value[0] && install_same_folder(field[F_SRC].value, d)) {
        field[F_DST].ok = 0; snprintf(field[F_DST].status, sizeof field[F_DST].status, "Choose a different folder from your original game - that one is only read.");
    } else if (!field[F_SRC].value[0] && install_has_base(d))
        snprintf(field[F_DST].status, sizeof field[F_DST].status, "Already set up - only the CM2 / 96/97 data you choose will be imported.");
    else if (install_is_port_folder(d))
        snprintf(field[F_DST].status, sizeof field[F_DST].status, "Already set up - files will be updated; saved games there are kept.");
    else snprintf(field[F_DST].status, sizeof field[F_DST].status, "The game will be set up here.");
}

/* ------------------------------------------------------------------ install thread + log */
#define MAXLINES 400
static char *lines[MAXLINES]; static int nlines, scroll;
static SDL_mutex *log_lock;
static char partial[600];
static void add_line(const char *s) {
    if (nlines == MAXLINES) { free(lines[0]); memmove(lines, lines + 1, sizeof lines[0] * (MAXLINES - 1)); nlines--; }
    lines[nlines++] = strdup(s);
}
static void log_text(const char *t) {                 /* from the install thread */
    SDL_LockMutex(log_lock);
    for (; *t; t++) {
        size_t k = strlen(partial);
        if (*t == '\n') { add_line(partial); partial[0] = 0; }
        else if (k + 1 < sizeof partial) { partial[k] = *t; partial[k + 1] = 0; }
    }
    scroll = 0;
    SDL_UnlockMutex(log_lock);
}
static void log_stdout(const char *t) { fputs(t, stdout); fflush(stdout); }

static struct install_opts opts;
static SDL_atomic_t state;                            /* 0 form, 1 installing, 2 done, 3 failed */
static int install_thread(void *u) {
    (void)u;
    int r = install_run(&opts, log_text);
    SDL_LockMutex(log_lock); if (partial[0]) { add_line(partial); partial[0] = 0; } SDL_UnlockMutex(log_lock);
    SDL_AtomicSet(&state, r == 0 ? 2 : 3);
    return 0;
}

/* ------------------------------------------------------------------ folder dialogs */
/* the folder chosen last (Browse... or dropped): the next chooser opens there, so the editions
   (which tend to sit side by side) don't each mean finding the way back */
static char last_pick[1100];
static int usable_start(const char *d) {              /* a folder that exists, and safe to put in a command line */
    if (!d[0] || strpbrk(d, "\"'`$")) return 0;
#ifdef _WIN32
    DWORD a = GetFileAttributesA(d); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    DIR *t = opendir(d); if (t) closedir(t); return t != NULL;
#endif
}
static void parent_of(const char *d, char *out, size_t n) {
    snprintf(out, n, "%s", d);
    char *s1 = strrchr(out, '/'), *s2 = strrchr(out, '\\'), *s = s1 > s2 ? s1 : s2;
    if (s && s > out) *s = 0;
}
#ifdef _WIN32
static int CALLBACK browse_cb(HWND h, UINT msg, LPARAM lp, LPARAM data) {
    (void)lp; if (msg == BFFM_INITIALIZED && data) SendMessageA(h, BFFM_SETSELECTIONA, TRUE, data);
    return 0;
}
#endif
/* start: open the chooser with this folder selected (or "") */
static int pick_folder(const char *titletext, const char *start, char *out, size_t n) {
    if (!usable_start(start)) start = "";
#ifdef _WIN32
    BROWSEINFOA bi; memset(&bi, 0, sizeof bi);
    bi.lpszTitle = titletext; bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (start[0]) { bi.lpfn = browse_cb; bi.lParam = (LPARAM)start; }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    LPITEMIDLIST id = SHBrowseForFolderA(&bi);
    int ok = 0;
    if (id) { char p[MAX_PATH]; if (SHGetPathFromIDListA(id, p)) { snprintf(out, n, "%s", p); ok = 1; } CoTaskMemFree(id); }
    return ok;
#elif defined(__APPLE__)
    char cmd[1600], where[1300] = "";                 /* macOS: the Finder's own folder chooser */
    if (start[0]) { char par[1100]; parent_of(start, par, sizeof par); snprintf(where, sizeof where, " default location (POSIX file \"%s\")", par); }
    snprintf(cmd, sizeof cmd, "osascript -e 'POSIX path of (choose folder with prompt \"%s\"%s)' 2>/dev/null", titletext, where);
    FILE *p = popen(cmd, "r"); if (!p) return 0;
    char buf[1100] = ""; if (!fgets(buf, sizeof buf, p)) buf[0] = 0;
    pclose(p);
    size_t k = strlen(buf); while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
    while (k > 1 && buf[k - 1] == '/') buf[--k] = 0;
    if (!buf[0]) return 0;                            /* cancelled */
    snprintf(out, n, "%s", buf); return 1;
#else
    char par[1100]; parent_of(start, par, sizeof par);
    const char *cmds[] = { "zenity --file-selection --directory --title=\"%s\"%s%s%s 2>/dev/null",
                           "kdialog --getexistingdirectory \"%s%s\" --title \"%s\" 2>/dev/null", NULL };
    const char *progs[] = { "zenity", "kdialog" };
    for (int i = 0; cmds[i]; i++) {
        char which[64]; snprintf(which, sizeof which, "command -v %s >/dev/null 2>&1", progs[i]);
        if (system(which) != 0) continue;
        char cmd[2600];
        if (i == 0) snprintf(cmd, sizeof cmd, cmds[0], titletext, start[0] ? " --filename=\"" : "", start, start[0] ? "\"" : "");
        else snprintf(cmd, sizeof cmd, cmds[1], start[0] ? par : "$HOME", "", titletext);
        FILE *p = popen(cmd, "r"); if (!p) continue;
        char buf[1100] = ""; if (!fgets(buf, sizeof buf, p)) buf[0] = 0;
        pclose(p);
        size_t k = strlen(buf); while (k && (buf[k - 1] == '\n' || buf[k - 1] == '\r')) buf[--k] = 0;
        if (!buf[0]) return 0;                        /* cancelled */
        snprintf(out, n, "%s", buf); return 1;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Choose a folder",
        "No folder chooser (zenity or kdialog) was found.\n\nType or paste the folder's path into the box, or drag the folder onto this window.", win);
    return 0;
#endif
}

/* which box a folder is for, or -1 */
static int classify(const char *d) {
    char msg[300];
    if (install_check_source(d, msg, sizeof msg) || strstr(msg, "version")) return F_SRC;
    if (install_check_euro(d, msg, sizeof msg)) { int r = install_euro_release(d); return r >= 0 ? F_EU0 + r : -1; }
    if (install_check_cm2(d, msg, sizeof msg)) { int y = 1995, dd, mm; cm2_detect(d, &y, &dd, &mm); return y >= 1996 ? F_CM96 : F_CM2; }
    return -1;
}
/* the folders inside a folder that are for a box: filled in; how many */
static int fill_from_subfolders(const char *d) {
    int found = 0; char sub[1400];
#ifdef _WIN32
    char pat[1200]; snprintf(pat, sizeof pat, "%s\\*", d);
    WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s\\%s", d, fd.cFileName);
#else
    DIR *dir = opendir(d); struct dirent *e;
    if (!dir) return 0;
    while ((e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", d, e->d_name);
        DIR *t = opendir(sub); if (!t) continue; closedir(t);
#endif
        int f = classify(sub);
        if (f >= 0 && f != F_SRC) { snprintf(field[f].value, sizeof field[f].value, "%s", sub); tab = field_tab(f); found++; }
#ifdef _WIN32
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    }
    closedir(dir);
#endif
    return found;
}

/* a dropped folder: which box is it for? */
static void dropped(const char *path) {
    char d[1100]; snprintf(d, sizeof d, "%s", path);
    size_t k = strlen(d); while (k > 1 && (d[k - 1] == '/' || d[k - 1] == '\\')) d[--k] = 0;
    int f = classify(d);
    if (f >= 0 || usable_start(d)) snprintf(last_pick, sizeof last_pick, "%s", d);
    if (f >= 0) { snprintf(field[f].value, sizeof field[f].value, "%s", d); tab = field_tab(f); }
    else if (fill_from_subfolders(d)) ;                  /* (the CM2 CD's folders, say) */
    else {
        /* a file: its folder */
        char *s1 = strrchr(d, '/'), *s2 = strrchr(d, '\\'), *s = s1 > s2 ? s1 : s2;
        FILE *f = fopen(d, "rb");
        if (f && s) { fclose(f); *s = 0; dropped(d); return; }
        if (f) fclose(f);
        snprintf(field[F_DST].value, sizeof field[F_DST].value, "%s", d); tab = T_BASE;
    }
    check_fields();
}

static void launch_game(void) {
    char exe[1300];
#ifdef _WIN32
    snprintf(exe, sizeof exe, "%s\\cm9798.exe", opts.dst);
    if ((INT_PTR)ShellExecuteA(NULL, "open", exe, NULL, opts.dst, SW_SHOWNORMAL) <= 32)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Play", "Couldn't start cm9798.exe in the new folder.", win);
#else
    snprintf(exe, sizeof exe, "%s/cm9798", opts.dst);
    if (access(exe, X_OK) != 0) snprintf(exe, sizeof exe, "cm9798");   /* installed in the PATH */
    if (fork() == 0) { if (chdir(opts.dst) != 0) _exit(1); execlp(exe, exe, (char *)NULL); _exit(1); }
#endif
}

/* ------------------------------------------------------------------ main */
/* a window program has no console for stderr: say why it stopped in a message box as well */
static void start_failed(const char *what) {
    char m[600]; snprintf(m, sizeof m, "%s: %s", what, SDL_GetError());
    fprintf(stderr, "%s\n", m);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Championship Manager 97/98 - Setup", m, NULL);
}

int main(int argc, char **argv) {
    char *bp = SDL_GetBasePath();
    snprintf(base_dir, sizeof base_dir, "%s", bp ? bp : ".");
    if (bp) SDL_free(bp);
    { size_t k = strlen(base_dir); while (k > 1 && (base_dir[k - 1] == '/' || base_dir[k - 1] == '\\')) base_dir[--k] = 0; }

    if (argc >= 4 && !strcmp(argv[1], "--cli")) {      /* without a window (and for testing) */
        memset(&opts, 0, sizeof opts);
        snprintf(opts.src, sizeof opts.src, "%s", argv[2]); snprintf(opts.dst, sizeof opts.dst, "%s", argv[3]);
        if (argc > 4) snprintf(opts.cm2, sizeof opts.cm2, "%s", argv[4]);
        if (argc > 5) snprintf(opts.cm96, sizeof opts.cm96, "%s", argv[5]);
        for (int k = 0; k < INSTALL_NEDITIONS && 6 + k < argc; k++) snprintf(opts.euro[k], sizeof opts.euro[k], "%s", argv[6 + k]);
        snprintf(opts.port_dir, sizeof opts.port_dir, "%s", base_dir);
        return install_run(&opts, log_stdout) == 0 ? 0 : 1;
    }

    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { start_failed("SDL couldn't start"); return 1; }
    win = SDL_CreateWindow("Championship Manager 97/98 - Setup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W, H,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) { start_failed("The setup window couldn't be opened"); return 1; }
    {   /* window / taskbar icon (on Windows the .exe's own icon comes from icon/cm9798setup.rc) */
        SDL_Surface *ic = SDL_CreateRGBSurfaceWithFormatFrom((void *)setup_icon_rgba, SETUP_ICON_W, SETUP_ICON_H, 32,
                                                             SETUP_ICON_W * 4, SDL_PIXELFORMAT_RGBA32);
        if (ic) { SDL_SetWindowIcon(win, ic); SDL_FreeSurface(ic); }
    }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    SDL_RenderSetLogicalSize(ren, W, H);
    font_init(&body, font_body_px, font_body_w, font_body_off, font_body_h);
    font_init(&title, font_title_px, font_title_w, font_title_off, font_title_h);
    log_lock = SDL_CreateMutex();
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    snprintf(field[F_SRC].label, 80, "Your Championship Manager 97/98 folder");
    snprintf(field[F_DST].label, 80, "Set the game up in");
    snprintf(field[F_CM2].label, 80, "Championship Manager 2 folder (optional - adds the 1995/96 database)");
    snprintf(field[F_CM96].label, 80, "CM2 96/97 Update folder (optional - the 1996/97 English and Scottish leagues)");
    for (int k = 0; k < INSTALL_NEDITIONS; k++) {      /* "French"; "Italian Leagues", "Liga Portuguesa" */
        const struct install_edition *e = install_edition(k);
        snprintf(field[F_EU0 + k].label, 80, "%s", e->season == 1995 ? e->adjective : e->name); }
#ifdef _WIN32
    snprintf(field[F_DST].value, sizeof field[F_DST].value, "%s", base_dir);        /* next to the port */
#else
    { const char *home = getenv("HOME"); snprintf(field[F_DST].value, sizeof field[F_DST].value, "%s/Games/CM9798", home ? home : "."); }
#endif
    for (int t = 0; t < NT; t++) { SDL_Rect r = { 30 + t * 180, 62, 170, 32 }; tab_r[t] = r; }
    for (int i = 0; i < NF; i++) {
        if (i < F_EU0) {                                 /* label above, status below */
            int y = i == F_DST ? 190 : 110;
            SDL_Rect box = { 30, y + 22, 590, 30 }; field[i].box = box;
            Button b = { { 632, y + 22, 98, 30 }, "Browse...", 1 }; field[i].browse = b;
        } else {                                         /* the editions: label to the left */
            int y = 232 + row_in_tab(i) * 47;
            SDL_Rect box = { 160, y, 460, 26 }; field[i].box = box;
            Button b = { { 632, y, 98, 26 }, "Browse...", 1 }; field[i].browse = b;
        }
    }
    check_fields();
    int autostart = 0;
    if (getenv("CM_SETUP_TEST")) {                    /* testing: "<cm9798>;<target>" filled in and started */
        char t[2300]; snprintf(t, sizeof t, "%s", getenv("CM_SETUP_TEST")); char *semi = strchr(t, ';');
        if (semi) { *semi = 0; snprintf(field[F_SRC].value, sizeof field[F_SRC].value, "%s", t);
                    snprintf(field[F_DST].value, sizeof field[F_DST].value, "%s", semi + 1); check_fields(); autostart = 1; }
    }
    Button install = { { 30, 570, 700, 36 }, "Install", 1 };
    Button btn_play = { { 30, 650, 340, 34 }, "Play", 1 }, btn_close = { { 390, 650, 340, 34 }, "Close", 1 };

    SDL_StartTextInput();
    for (int run = 1; run;) {
        SDL_Event e;
        int st = SDL_AtomicGet(&state);
        install.enabled = st == 0 && field[F_SRC].ok && field[F_DST].ok && field[F_CM2].ok && field[F_CM96].ok;
        if (!field[F_SRC].value[0] && !field[F_CM2].value[0] && !field[F_CM96].value[0]) install.enabled = 0;   /* import-only: something to import */
        for (int k = 0; k < INSTALL_NEDITIONS; k++) install.enabled = install.enabled && field[F_EU0 + k].ok;
        if (autostart == 1 && install.enabled) {      /* (testing) press Install */
            autostart = 2; SDL_Event ev; memset(&ev, 0, sizeof ev); ev.type = SDL_MOUSEBUTTONDOWN;
            ev.button.button = SDL_BUTTON_LEFT; ev.button.x = -1; ev.button.y = -1; SDL_PushEvent(&ev);
        }
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT: if (st != 1) run = 0; break;
            case SDL_MOUSEMOTION: hover_x = e.motion.x; hover_y = e.motion.y; break;
            case SDL_MOUSEWHEEL: SDL_LockMutex(log_lock); scroll += e.wheel.y * 3; if (scroll < 0) scroll = 0;
                                 if (scroll > nlines) scroll = nlines; SDL_UnlockMutex(log_lock); break;
            case SDL_DROPFILE: if (st == 0) dropped(e.drop.file); SDL_free(e.drop.file); break;
            case SDL_MOUSEBUTTONDOWN: {
                if (e.button.button != SDL_BUTTON_LEFT) break;
                int x = e.button.x, y = e.button.y;
                if (st == 0) {
                    focus = -1;
                    for (int t = 0; t < NT; t++) if (inside(x, y, tab_r[t])) tab = t;
                    for (int i = 0; i < NF; i++) {
                        if (field_tab(i) != tab) continue;
                        if (inside(x, y, field[i].box)) focus = i;
                        if (inside(x, y, field[i].browse.r)) {
                            char p[1100], ttl[160];
                            if (i >= F_EU0) { const struct install_edition *e = install_edition(i - F_EU0);
                                              snprintf(ttl, sizeof ttl, "Championship Manager 2: %s", e->season == 1995 ? e->adjective : e->name);
                                              if (e->season == 1995) snprintf(ttl + strlen(ttl), sizeof ttl - strlen(ttl), " edition"); }
                            else snprintf(ttl, sizeof ttl, "%s", field[i].label);
                            if (pick_folder(ttl, usable_start(field[i].value) ? field[i].value : last_pick, p, sizeof p)) {
                                snprintf(field[i].value, sizeof field[i].value, "%s", p); snprintf(last_pick, sizeof last_pick, "%s", p); check_fields(); }
                        }
                    }
                    if (install.enabled && (inside(x, y, install.r) || autostart == 2)) {
                        memset(&opts, 0, sizeof opts);
                        snprintf(opts.src, sizeof opts.src, "%s", field[F_SRC].value);
                        snprintf(opts.dst, sizeof opts.dst, "%s", field[F_DST].value);
                        snprintf(opts.cm2, sizeof opts.cm2, "%s", field[F_CM2].value);
                        snprintf(opts.cm96, sizeof opts.cm96, "%s", field[F_CM96].value);
                        for (int k = 0; k < INSTALL_NEDITIONS; k++) snprintf(opts.euro[k], sizeof opts.euro[k], "%s", field[F_EU0 + k].value);
                        snprintf(opts.port_dir, sizeof opts.port_dir, "%s", base_dir);
                        focus = -1; SDL_AtomicSet(&state, 1);
                        SDL_Thread *t = SDL_CreateThread(install_thread, "install", NULL);
                        if (t) SDL_DetachThread(t); else { log_text("Couldn't start the installation.\n"); SDL_AtomicSet(&state, 3); }
                    }
                } else if (st >= 2) {
                    if (st == 2 && inside(x, y, btn_play.r)) { launch_game(); run = 0; }
                    if (inside(x, y, btn_close.r)) run = 0;
                }
                break; }
            case SDL_TEXTINPUT:
                if (st == 0 && focus >= 0) {
                    size_t k = strlen(field[focus].value);
                    snprintf(field[focus].value + k, sizeof field[focus].value - k, "%s", e.text.text); check_fields();
                }
                break;
            case SDL_KEYDOWN:
                if (st != 0 || focus < 0) break;
                if (e.key.keysym.sym == SDLK_BACKSPACE) {
                    size_t k = strlen(field[focus].value);
                    while (k && ((unsigned char)field[focus].value[k - 1] & 0xC0) == 0x80) k--;   /* whole UTF-8 character */
                    if (k) field[focus].value[k - 1] = 0;
                    check_fields();
                } else if (e.key.keysym.sym == SDLK_v && (SDL_GetModState() & KMOD_CTRL)) {
                    char *c = SDL_GetClipboardText();
                    if (c) {
                        char *q = c; while (*q == ' ' || *q == '"') q++;
                        size_t k = strlen(q); while (k && (q[k - 1] == '\n' || q[k - 1] == '\r' || q[k - 1] == ' ' || q[k - 1] == '"')) q[--k] = 0;
                        snprintf(field[focus].value, sizeof field[focus].value, "%s", q); SDL_free(c); check_fields();
                    }
                } else if (e.key.keysym.sym == SDLK_ESCAPE || e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_TAB) {
                    if (e.key.keysym.sym != SDLK_TAB) focus = -1;
                    else { int f = focus; do f = (f + 1) % NF; while (field_tab(f) != tab); focus = f; }
                }
                break;
            }
        }

        /* draw */
        fill(0, 0, W, H, 22, 34, 26);
        fill(0, 0, W, 50, 10, 70, 30);
        text(&title, (W - text_w(&title, "Championship Manager 97/98 - Setup")) / 2, 12, "Championship Manager 97/98 - Setup", c_head);
        if (st == 0) {
            for (int t = 0; t < NT; t++) {                   /* the tabs: red when something on one needs changing */
                SDL_Rect r = tab_r[t]; int bad = 0, any = 0;
                for (int i = 0; i < NF; i++) if (field_tab(i) == t) { bad |= !field[i].ok; any |= i >= F_CM2 && field[i].value[0]; }
                if (t == tab) fill(r.x, r.y, r.w, r.h, 40, 90, 50); else fill(r.x, r.y, r.w, r.h, 16, 26, 20);
                frame(r.x, r.y, r.w, r.h, t == tab ? 200 : 90, t == tab ? 220 : 110, t == tab ? 120 : 90);
                SDL_Color c = bad ? c_bad : t == tab ? c_head : any ? c_ok : c_text;
                text(&body, r.x + (r.w - text_w(&body, tab_name[t])) / 2, r.y + (r.h - body.h) / 2, tab_name[t], c);
            }
            fill(30, 94, 700, 2, 40, 90, 50);
            if (tab == T_CM2) text(&body, 30, 204, "European Leagues editions, 1995/96 (optional - each adds its country's leagues in full):", c_text);
            if (tab == T_9697) text(&body, 30, 204, "League releases, 1996/97 (optional - each adds its country's leagues in full):", c_text);
            for (int i = 0; i < NF; i++) {
                if (field_tab(i) != tab) continue;
                SDL_Rect b = field[i].box;
                if (i >= F_EU0) text(&body, 30, b.y + (b.h - body.h) / 2, field[i].label, c_text);
                else text(&body, b.x, b.y - 20, field[i].label, c_text);
                fill(b.x, b.y, b.w, b.h, 12, 18, 14);
                frame(b.x, b.y, b.w, b.h, focus == i ? 200 : 90, focus == i ? 220 : 110, focus == i ? 120 : 90);
                if (field[i].value[0]) text_fit(&body, b.x + 8, b.y + (b.h - body.h) / 2, b.w - 16, field[i].value, c_text);
                else text(&body, b.x + 8, b.y + (b.h - body.h) / 2, i >= F_CM2 ? "(leave empty to skip)" : "type, paste, Browse... or drop a folder here", c_dim);
                if (focus == i && (SDL_GetTicks() / 500) % 2) {
                    int cx = b.x + 8 + (text_w(&body, field[i].value) < b.w - 16 ? text_w(&body, field[i].value) : b.w - 16);
                    fill(cx, b.y + 6, 2, b.h - 12, 220, 220, 220);
                }
                button(&field[i].browse);
                int good = field[i].ok && !(i >= F_CM2 && !field[i].value[0]);
                text_fit(&body, b.x, b.y + b.h + (i >= F_EU0 ? 2 : 4), 700 - b.x + 30, field[i].status,
                     (i >= F_CM2 && !field[i].value[0]) ? c_dim : good ? c_ok : c_bad);
            }
            button(&install);
            text(&body, 30, 618, "Your original game folders are only read - nothing in them is changed.", c_dim);
            text(&body, 30, 640, "Saved games, autosaves and backups found there are brought across too.", c_dim);
        } else {
            SDL_LockMutex(log_lock);
            int top = 64, rows = (st == 1 ? 600 : 560) / (body.h + 2);
            fill(20, top - 6, 720, rows * (body.h + 2) + 12, 12, 18, 14);
            int first = nlines - rows - scroll; if (first < 0) first = 0;
            for (int i = 0; i < rows && first + i < nlines; i++) {
                const char *l = lines[first + i];
                SDL_Color c = strstr(l, "ERROR") || strstr(l, "couldn't") || strstr(l, "Can't") ? c_bad : c_text;
                text_fit(&body, 30, top + i * (body.h + 2), 700, l, c);
            }
            SDL_UnlockMutex(log_lock);
            if (st == 1) text(&body, 30, H - 32, "Installing...", c_head);
            else {
                text(&body, 30, 622, st == 2 ? "Finished." : "Something went wrong - see above.", st == 2 ? c_ok : c_bad);
                if (st == 2) button(&btn_play);
                button(&btn_close);
            }
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }
    SDL_Quit();
    return 0;
}
