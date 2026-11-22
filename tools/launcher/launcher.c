/* launcher.c - cm9798: the program players start. The game's code isn't distributed: the first time
 * it runs (and again after the port is updated), this makes it on the player's own machine from their
 * own CM2E16.EXE, then starts it:
 *   1. tools/cmgen turns CM2E16.EXE into C (gen/), exactly as make_gen.py does - and, when CM2ED.EXE (the
 *      data editor that came with the game) is next to it, that too (gen/ed_*.c: the Database Editor),
 *   2. the bundled C compiler (build/zig) compiles that,
 *   3. and links it with the port's own code (build/lib/libcm9798rt.a) into build/cm9798-game(.exe),
 *      which it then starts from the game folder (so players only see cm9798 and cm9798setup).
 * Everything it needs is in the build folder next to it:
 *   build/data/       functions.json, labels.csv, patches.txt, overrides.txt, relocations.txt
 *   build/data/editor/  the same for CM2ED.EXE
 *   build/include/    rt.h, rt_internal.h (what the generated C includes)
 *   build/lib/        libcm9798rt.a (+ on Windows the SDL import libraries and the icon)
 *   build/compiler.txt, cflags.txt, link.txt   one argument per line; {kit} = the build folder,
 *                     {find:a|b} = the first of those libraries found in the usual library folders
 *   build/zig.zip     the compiler (Zig, MIT licence) as downloaded (or zig.tar.xz): unpacked into
 *                     build/zig the first time it's needed (or build/zig/ itself, already unpacked)
 *   build/work/       made here (gen/, objects, the compiler's cache)
 * Once the game is made, all of that is deleted (CM_KEEP_BUILD=1 keeps it): build/ then holds only
 * cm9798-game, game.stamp and build.log. A newer release, set up with cm9798setup, brings a new kit.
 *   build/build.log   what the compiler said, if something goes wrong
 *   build/cm9798-game the game it makes (with no build folder - a developer's own build - the
 *                     cm9798-game next to this program is started instead)
 * Options: --rebuild (make the game again), anything else goes to the game (e.g. a game folder).
 * CM_CC="gcc" (say) uses another compiler instead of the bundled one. On Linux and macOS a release can
 * leave the compiler out: then the system's own (cc, gcc or clang) is used. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include "../cmgen/cmgen.h"
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define EXE ".exe"
#else
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#define MKDIR(p) mkdir(p, 0755)
#define EXE ""
extern char **environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#include <sys/sysctl.h>
#endif
#ifndef LAUNCHER_CONSOLE
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include "../setup/font_data.h"
#endif

#define LAUNCHER_VERSION "1"
#define GAME_NAME "cm9798-game" EXE

static char progdir[1100], kit[1200], logpath[1300];

/* ------------------------------------------------------------------ small helpers */
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static void mkdirs(const char *path) {
    char p[1300]; snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) if (*s == '/' || *s == '\\') { char c = *s; *s = 0; MKDIR(p); *s = c; }
    MKDIR(p);
}
static void program_dir(char *out, size_t max) {
    out[0] = 0;
#ifdef _WIN32
    DWORD n = GetModuleFileNameA(NULL, out, (DWORD)max); if (n == 0 || n >= max) { out[0] = 0; return; }
#elif defined(__APPLE__)
    uint32_t sz = (uint32_t)max; if (_NSGetExecutablePath(out, &sz) != 0) { out[0] = 0; return; }
    char real[PATH_MAX]; if (realpath(out, real)) snprintf(out, max, "%s", real);
#else
    ssize_t n = readlink("/proc/self/exe", out, max - 1); if (n <= 0) { out[0] = 0; return; } out[n] = 0;
#endif
    char *s1 = strrchr(out, '/'), *s2 = strrchr(out, '\\'), *s = s1 > s2 ? s1 : s2;
    if (s) *s = 0; else snprintf(out, max, ".");
}
static uint32_t crc_update(uint32_t c, const unsigned char *p, size_t n) {
    c = ~c;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
static uint32_t crc_file(uint32_t c, const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return crc_update(c, (const unsigned char *)"-", 1);
    unsigned char b[65536]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) c = crc_update(c, b, n);
    fclose(f); return c;
}
static void logf_(const char *fmt, ...) {
    FILE *f = fopen(logpath, "a"); if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a); fclose(f);
}
/* the last few lines of the log, for an error message */
static void log_tail(char *out, size_t n, int lines) {
    out[0] = 0; FILE *f = fopen(logpath, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long sz = ftell(f); long from = sz > 4000 ? sz - 4000 : 0; fseek(f, from, SEEK_SET);
    char buf[4001]; size_t k = fread(buf, 1, 4000, f); buf[k] = 0; fclose(f);
    char *p = buf + k; int nl = 0; while (p > buf) { if (*--p == '\n' && ++nl > lines) { p++; break; } }
    snprintf(out, n, "%s", p);
}

/* ------------------------------------------------------------------ the window */
#ifndef LAUNCHER_CONSOLE
static SDL_Window *win; static SDL_Renderer *ren;
typedef struct { SDL_Texture *t[224]; const unsigned short *w; int h; } Font;
static Font body, title;
static void font_init(Font *f, const unsigned char *px, const unsigned short *w, const unsigned int *off, int h) {
    f->w = w; f->h = h;
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
static void text(const Font *f, int x, int y, const char *s, int r, int g, int b) {
    for (; *s; s++) {
        int c = (unsigned char)*s; if (c < 32) continue;
        SDL_Texture *t = f->t[c - 32]; int w = f->w[c - 32];
        if (t) { SDL_SetTextureColorMod(t, (Uint8)r, (Uint8)g, (Uint8)b); SDL_Rect q = { x, y, w, f->h }; SDL_RenderCopy(ren, t, NULL, &q); }
        x += w;
    }
}
#endif
static int ui_cancelled;
static void ui_open(void) {
#ifndef LAUNCHER_CONSOLE
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return;
    win = SDL_CreateWindow("CM 97/98 - getting the game ready", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 560, 170, 0);
    if (!win) return;
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (!ren) return;
    font_init(&body, font_body_px, font_body_w, font_body_off, 18);
    font_init(&title, font_title_px, font_title_w, font_title_off, 27);
#endif
}
/* permille 0..1000 */
static void ui_progress(int permille, const char *what) {
#ifndef LAUNCHER_CONSOLE
    if (!ren) return;
    SDL_Event e;
    while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) ui_cancelled = 1;
    SDL_SetRenderDrawColor(ren, 24, 40, 24, 255); SDL_RenderClear(ren);
    text(&title, 24, 16, "Getting the game ready", 255, 230, 60);
    text(&body, 24, 54, "The first time it runs, the game is made from your own CM2E16.EXE.", 235, 235, 235);
    text(&body, 24, 74, "This takes a minute or two, and only happens once.", 150, 150, 150);
    SDL_Rect bar = { 24, 108, 512, 18 };
    SDL_SetRenderDrawColor(ren, 52, 70, 52, 255); SDL_RenderFillRect(ren, &bar);
    SDL_Rect fillr = { 24, 108, 512 * (permille < 0 ? 0 : permille > 1000 ? 1000 : permille) / 1000, 18 };
    SDL_SetRenderDrawColor(ren, 120, 220, 120, 255); SDL_RenderFillRect(ren, &fillr);
    SDL_SetRenderDrawColor(ren, 110, 130, 110, 255); SDL_RenderDrawRect(ren, &bar);
    text(&body, 24, 134, what, 150, 150, 150);
    SDL_RenderPresent(ren);
#else
    static int last = -1; if (permille / 50 != last) { last = permille / 50; fprintf(stderr, "%3d%% %s\n", permille / 10, what); }
#endif
}
static void ui_close(void) {
#ifndef LAUNCHER_CONSOLE
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    ren = NULL; win = NULL;
    SDL_Quit();
#endif
}
static void ui_error(const char *msg) {
    fprintf(stderr, "%s\n", msg);
#ifndef LAUNCHER_CONSOLE
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "CM 97/98", msg, win);
#endif
}

/* ------------------------------------------------------------------ running programs */
typedef struct { char **v; int n, cap; } args_t;
static void arg(args_t *a, const char *s) {
    if (a->n + 2 > a->cap) { a->cap = a->cap * 2 + 16; a->v = realloc(a->v, a->cap * sizeof *a->v); }
    a->v[a->n++] = strdup(s); a->v[a->n] = NULL;
}
static void arg_free(args_t *a) { for (int i = 0; i < a->n; i++) free(a->v[i]); free(a->v); a->v = 0; a->n = a->cap = 0; }

#ifdef _WIN32
typedef HANDLE proc_t;
static void quote(char *out, size_t n, char **v) {   /* the command line CreateProcess wants */
    size_t k = 0;
    for (int i = 0; v[i]; i++) {
        if (i && k + 1 < n) out[k++] = ' ';
        if (k + 1 < n) out[k++] = '"';
        for (const char *s = v[i]; *s; s++) {
            int bs = 0; while (*s == '\\') { bs++; s++; }
            if (!*s) { for (int j = 0; j < bs * 2 && k + 1 < n; j++) out[k++] = '\\'; break; }
            if (*s == '"') { for (int j = 0; j < bs * 2 + 1 && k + 1 < n; j++) out[k++] = '\\'; }
            else for (int j = 0; j < bs && k + 1 < n; j++) out[k++] = '\\';
            if (k + 1 < n) out[k++] = *s;
        }
        if (k + 1 < n) out[k++] = '"';
    }
    out[k] = 0;
}
static int spawn(char **v, int to_log, proc_t *p) {
    static char cmd[32768]; quote(cmd, sizeof cmd, v);
    STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (to_log) {
        SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
        h = CreateFileA(logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = h; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); }
    }
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, to_log ? CREATE_NO_WINDOW : 0, NULL, NULL, &si, &pi);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) return -1;
    CloseHandle(pi.hThread); *p = pi.hProcess; return 0;
}
static int poll_proc(proc_t p, int *status) {   /* 1 = finished */
    if (WaitForSingleObject(p, 0) != WAIT_OBJECT_0) return 0;
    DWORD c = 1; GetExitCodeProcess(p, &c); CloseHandle(p); *status = (int)c; return 1;
}
static void kill_proc(proc_t p) { TerminateProcess(p, 1); CloseHandle(p); }
static void nap(void) { Sleep(20); }
static int ncpu(void) { SYSTEM_INFO si; GetSystemInfo(&si); return si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 2; }
static double mem_gb(void) { MEMORYSTATUSEX m; m.dwLength = sizeof m; return GlobalMemoryStatusEx(&m) ? m.ullTotalPhys / 1073741824.0 : 8; }
static void set_env(const char *k, const char *v) { SetEnvironmentVariableA(k, v); }
#else
typedef pid_t proc_t;
static int spawn(char **v, int to_log, proc_t *p) {
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    if (to_log) {
        posix_spawn_file_actions_addopen(&fa, 1, logpath, O_WRONLY | O_APPEND | O_CREAT, 0644);
        posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }
    int r = strchr(v[0], '/') ? posix_spawn(p, v[0], &fa, NULL, v, environ) : posix_spawnp(p, v[0], &fa, NULL, v, environ);
    posix_spawn_file_actions_destroy(&fa);
    return r ? -1 : 0;
}
static int poll_proc(proc_t p, int *status) {
    int st; pid_t r = waitpid(p, &st, WNOHANG);
    if (r == 0) return 0;
    *status = (r < 0) ? 1 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0); return 1;
}
static void kill_proc(proc_t p) { int st; kill(p, SIGTERM); waitpid(p, &st, 0); }
static void nap(void) { usleep(20000); }
static int ncpu(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 2; }
static double mem_gb(void) { long p = sysconf(_SC_PHYS_PAGES), z = sysconf(_SC_PAGESIZE); return p > 0 && z > 0 ? (double)p * z / 1073741824.0 : 8; }
static void set_env(const char *k, const char *v) { setenv(k, v, 1); }
#endif

/* run one program to the end, keeping the window alive; its output goes to the log */
static int run_wait(char **v, int permille, const char *what) {
    proc_t p; int st = 1;
    for (int i = 0; v[i]; i++) logf_("%s%s", i ? " " : "", v[i]);
    logf_("\n");
    if (spawn(v, 1, &p)) { logf_("can't run %s\n", v[0]); return -1; }
    while (!poll_proc(p, &st)) {
        ui_progress(permille, what); if (ui_cancelled) { kill_proc(p); return -1; }
        nap();
    }
    return st;
}

/* ------------------------------------------------------------------ the build */
#ifndef _WIN32
#ifdef __APPLE__
/* macOS versions as major << 16 | minor << 8 (as Mach-O load commands keep them) */
static unsigned macos_version(void) {
    char v[32] = ""; size_t n = sizeof v; unsigned ma = 0, mi = 0;
    if (sysctlbyname("kern.osproductversion", v, &n, NULL, 0) != 0 || sscanf(v, "%u.%u", &ma, &mi) < 1) return 0;
    return ma << 16 | mi << 8;
}
/* the oldest macOS a program runs on, from its LC_BUILD_VERSION (or LC_VERSION_MIN_MACOSX); 0 if unknown */
static unsigned macho_minos(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    unsigned char h[32], lc[16]; unsigned r = 0;
    if (fread(h, 1, 32, f) == 32 && h[0] == 0xCF && h[1] == 0xFA && h[2] == 0xED && h[3] == 0xFE) {  /* 64-bit, little-endian */
        uint32_t ncmds = h[16] | h[17] << 8 | h[18] << 16 | (uint32_t)h[19] << 24; long at = 32;
        for (uint32_t i = 0; i < ncmds && i < 512; i++) {
            if (fseek(f, at, SEEK_SET) || fread(lc, 1, 16, f) != 16) break;
            uint32_t cmd = lc[0] | lc[1] << 8 | lc[2] << 16 | (uint32_t)lc[3] << 24, size = lc[4] | lc[5] << 8 | lc[6] << 16 | (uint32_t)lc[7] << 24;
            if (cmd == 0x32) { r = lc[12] | lc[13] << 8 | lc[14] << 16 | (uint32_t)lc[15] << 24; break; }   /* LC_BUILD_VERSION: minos */
            if (cmd == 0x24) { r = lc[8] | lc[9] << 8 | lc[10] << 16 | (uint32_t)lc[11] << 24; break; }    /* LC_VERSION_MIN_MACOSX */
            if (size < 8) break;
            at += size;
        }
    }
    fclose(f); return r;
}
#endif
/* the system's C compiler (cc, gcc or clang on the PATH); on a Mac only if Apple's command line tools
   are installed (/usr/bin/cc is there anyway, and would only offer to install them) */
static int system_cc(char *out, size_t n) {
#ifdef __APPLE__
    if (!is_dir("/Library/Developer/CommandLineTools/usr/bin") && !is_dir("/Applications/Xcode.app")) return 0;
#endif
    const char *names[] = { "cc", "gcc", "clang", NULL };
    const char *path = getenv("PATH"); if (!path || !*path) path = "/usr/local/bin:/usr/bin:/bin";
    for (int i = 0; names[i]; i++) {
        const char *p = path;
        while (*p) {
            const char *e = strchr(p, ':'); size_t L = e ? (size_t)(e - p) : strlen(p);
            if (L) { snprintf(out, n, "%.*s/%s", (int)L, p, names[i]); if (is_file(out) && access(out, X_OK) == 0) return 1; }
            if (!e) break; p = e + 1;
        }
    }
    return 0;
}
#endif
/* a text file of arguments, one per line ("#" comments), with {kit} and {find:...} filled in */
static const char *const lib_dirs[] = {
#ifdef __APPLE__
    "/opt/homebrew/lib", "/usr/local/lib", "/opt/local/lib",
#else
    "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu", "/usr/lib64", "/usr/lib", "/usr/local/lib",
    "/lib/x86_64-linux-gnu", "/lib64", "/lib",
#endif
    NULL };
static int read_args(const char *name, args_t *a, char *err, size_t errn) {
    char path[1400]; snprintf(path, sizeof path, "%s/%s", kit, name);
    FILE *f = fopen(path, "r"); if (!f) return 0;
    char ln[1400];
    while (fgets(ln, sizeof ln, f)) {
        ln[strcspn(ln, "\r\n")] = 0;
        char *s = ln; while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        char out[2000]; size_t k = 0;
        for (const char *p = s; *p && k + 1 < sizeof out; ) {
            if (!strncmp(p, "{kit}", 5)) { k += snprintf(out + k, sizeof out - k, "%s", kit); p += 5; continue; }
            if (!strncmp(p, "{find:", 6)) {
                const char *e = strchr(p, '}'); if (!e) break;
                char names[600]; snprintf(names, sizeof names, "%.*s", (int)(e - p - 6), p + 6);
                char found[1300] = "";
                for (char *nm = strtok(names, "|"); nm && !found[0]; nm = strtok(NULL, "|"))
                    for (int d = 0; lib_dirs[d] && !found[0]; d++) {
                        char c[1300]; snprintf(c, sizeof c, "%s/%s", lib_dirs[d], nm);
                        if (is_file(c)) snprintf(found, sizeof found, "%s", c);
                    }
                if (!found[0]) {
                    snprintf(err, errn, "A library the game needs isn't installed: %.*s\n\n"
#ifdef __APPLE__
                        "Install SDL2 and SDL2_ttf with Homebrew (brew install sdl2 sdl2_ttf), then start the game again.",
#else
                        "Install SDL2 and SDL2_ttf from your distribution (e.g. libsdl2-2.0-0 and libsdl2-ttf-2.0-0, or sdl2 and sdl2_ttf), then start the game again.",
#endif
                        (int)(e - p - 6), p + 6);
                    fclose(f); return -1;
                }
                k += snprintf(out + k, sizeof out - k, "%s", found); p = e + 1; continue;
            }
            out[k++] = *p++;
        }
        out[k] = 0; arg(a, out);
    }
    fclose(f); return 1;
}
static const char *const data_files[] = { "functions.json", "labels.csv", "patches.txt", "overrides.txt", "relocations.txt", NULL };
static void make_stamp(char *out, size_t n) {
    char p[1400]; uint32_t c = 0;
    c = crc_update(c, (const unsigned char *)LAUNCHER_VERSION, strlen(LAUNCHER_VERSION));
    snprintf(p, sizeof p, "%s/lib/libcm9798rt.a", kit); c = crc_file(c, p);
    for (int i = 0; data_files[i]; i++) { snprintf(p, sizeof p, "%s/data/%s", kit, data_files[i]); c = crc_file(c, p);
                                          snprintf(p, sizeof p, "%s/data/editor/%s", kit, data_files[i]); c = crc_file(c, p); }
    const char *t[] = { "compiler.txt", "cflags.txt", "link.txt", NULL };
    for (int i = 0; t[i]; i++) { snprintf(p, sizeof p, "%s/%s", kit, t[i]); c = crc_file(c, p); }
    snprintf(out, n, "cm9798 %s %08x\n", LAUNCHER_VERSION, c);
}
static int kit_complete(void) { char p[1400]; snprintf(p, sizeof p, "%s/lib/libcm9798rt.a", kit); return is_file(p); }
static int stamp_ok(void) {
    if (!kit_complete()) return 1;      /* tidied after the game was made (see tidy_kit): keep that game */
    char want[64], have[64] = "", p[1400]; make_stamp(want, sizeof want);
    snprintf(p, sizeof p, "%s/game.stamp", kit);
    FILE *f = fopen(p, "r"); if (!f) return 0;
    if (!fgets(have, sizeof have, f)) have[0] = 0;
    fclose(f); return !strcmp(want, have);
}

/* The compiler comes as the archive Zig is downloaded as (build/zig.zip, zig.tar.xz or zig.tar.gz):
   one file, rather than the 19,000 in an unpacked Zig, in the release and for setup to copy. Unpacked
   here into build/zig the first time it's needed, with tar (part of Windows 10 and 11, macOS, Linux). */
static void unpack_compiler(void) {
    static const char *const names[] = { "zig.zip", "zig.tar.xz", "zig.tar.gz", NULL };
    char arc[1400] = "";
    for (int i = 0; names[i] && !arc[0]; i++) { snprintf(arc, sizeof arc, "%s/%s", kit, names[i]); if (!is_file(arc)) arc[0] = 0; }
    if (!arc[0]) return;
    char tmp[1300], zig[1300]; snprintf(tmp, sizeof tmp, "%s/work/zig-unpack", kit); snprintf(zig, sizeof zig, "%s/zig", kit);
    mkdirs(tmp);
#ifdef _WIN32
    char tar[600]; const char *root = getenv("SystemRoot");
    snprintf(tar, sizeof tar, "%s\\System32\\tar.exe", root ? root : "C:\\Windows");
    if (!is_file(tar)) snprintf(tar, sizeof tar, "tar");
#else
    const char *tar = "tar";
#endif
    char *v[] = { (char *)tar, "-xf", arc, "-C", tmp, NULL };
    logf_("unpacking %s\n", arc);
    if (run_wait(v, 150, "Unpacking the compiler (once)") != 0) { logf_("couldn't unpack %s\n", arc); return; }
    /* the archive holds one folder (zig-<os>-<arch>-<version>): that becomes build/zig */
    DIR *d = opendir(tmp); struct dirent *e; char top[1400] = "";
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1400]; snprintf(p, sizeof p, "%s/%s", tmp, e->d_name);
        if (is_dir(p)) { snprintf(top, sizeof top, "%s", p); break; }
    }
    if (d) closedir(d);
    if (!top[0] || rename(top, zig)) { logf_("couldn't put the compiler in %s\n", zig); return; }
}

static void tidy_kit(void);
/* macOS marks files from a download (and files written by a program opened from one) as quarantined,
   and stops marked programs that aren't signed by a registered developer at once, with no message
   ("killed"): the compiler, the game made with it, the SDL frameworks. The player chose to run the game,
   so the mark is cleared from what it uses: the build kit, the Frameworks folder and the game itself. */
static void unquarantine(void) {
#ifdef __APPLE__
    if (!is_file("/usr/bin/xattr")) return;
    char fw[1300]; snprintf(fw, sizeof fw, "%s/Frameworks", progdir);
    char *v[] = { "/usr/bin/xattr", "-dr", "com.apple.quarantine", kit, is_dir(fw) ? fw : NULL, NULL };
    proc_t p; int st;
    if (spawn(v, 1, &p) == 0) while (!poll_proc(p, &st)) nap();
#endif
}

static void cmgen_cb(int pm, const char *what, void *ctx) { (void)ctx; ui_progress(pm * 15 / 100, what); }  /* 0..15% */

typedef struct { char path[1400]; long size; } src_t;
static int by_size(const void *a, const void *b) { long x = ((const src_t *)a)->size, y = ((const src_t *)b)->size; return x < y ? 1 : x > y ? -1 : 0; }

static int build(const char *exe, char *err, size_t errn) {
    char work[1300], gen[1350], obj[1350], cache[1350], tmp[1400], game[1300];
    snprintf(work, sizeof work, "%s/work", kit);
    snprintf(gen, sizeof gen, "%s/gen", work); snprintf(obj, sizeof obj, "%s/obj", work); snprintf(cache, sizeof cache, "%s/cache", work);
    mkdirs(gen); mkdirs(obj); mkdirs(cache);
    { FILE *f = fopen(logpath, "w"); if (f) fclose(f); }
    logf_("cm9798 launcher %s: making the game from %s\n", LAUNCHER_VERSION, exe);

    /* 1. CM2E16.EXE -> C */
    char data[1300]; snprintf(data, sizeof data, "%s/data", kit);
    if (cmgen_run(exe, data, gen, cmgen_cb, NULL, err, errn)) { logf_("%s\n", err); return -1; }
    /* ...and the Database Editor, from CM2ED.EXE next to it (rt/editor.c). Without it - or if it's
       another version - the game is made without the editor (the Options screen leaves it out). */
    {
        char edexe[1400], eddata[1350], ederr[600] = ""; int made = 0;
        snprintf(edexe, sizeof edexe, "%s", exe);
        char *s1 = strrchr(edexe, '/'), *s2 = strrchr(edexe, '\\'), *s = s1 > s2 ? s1 : s2;
        snprintf(eddata, sizeof eddata, "%s/data/editor", kit);
        for (int v = 0; v < 2 && !made; v++) {
            if (s) snprintf(s + 1, sizeof edexe - (size_t)(s + 1 - edexe), "%s", v ? "cm2ed.exe" : "CM2ED.EXE");
            else snprintf(edexe, sizeof edexe, "%s", v ? "cm2ed.exe" : "CM2ED.EXE");
            if (!is_file(edexe)) continue;
            if (cmgen_run(edexe, eddata, gen, cmgen_cb, NULL, ederr, sizeof ederr)) logf_("the Database Editor isn't included: %s\n", ederr);
            else { made = 1; logf_("the Database Editor is included (%s)\n", edexe); }
        }
        if (!made) {                                  /* none of its files from an earlier build */
            char p[1500]; const char *fixed[] = { "edfuncs.h", "edtable.c", "edretsites.c", NULL };
            for (int i = 0; fixed[i]; i++) { snprintf(p, sizeof p, "%s/%s", gen, fixed[i]); remove(p); }
            for (int i = 0; i < 100; i++) { snprintf(p, sizeof p, "%s/ed_%03d.c", gen, i); remove(p); }
        }
    }

    /* 2. compile */
    args_t cc = {0}, cfl = {0}, lnk = {0};
    const char *env_cc = getenv("CM_CC");
    if (env_cc && *env_cc) { char b[1000]; snprintf(b, sizeof b, "%s", env_cc); for (char *t = strtok(b, " "); t; t = strtok(NULL, " ")) arg(&cc, t); }
    else {
        int r = read_args("compiler.txt", &cc, err, errn); if (r < 0) return -1;
        if (!r) { char z[1300]; snprintf(z, sizeof z, "%s/zig/zig" EXE, kit); arg(&cc, z); arg(&cc, "cc"); }
    }
    if ((strchr(cc.v[0], '/') || strchr(cc.v[0], '\\')) && !is_file(cc.v[0])) unpack_compiler();
    unquarantine();
#ifdef __APPLE__
    /* the bundled Zig needs a newer macOS than the programs do (Zig 0.17: macOS 15): on an older Mac make
       the game with Apple's own compiler instead, from its command line tools */
    if (!(env_cc && *env_cc) && is_file(cc.v[0])) {
        unsigned need = macho_minos(cc.v[0]), have = macos_version();
        if (need && have && have < need) {
            char sys[1300];
            if (system_cc(sys, sizeof sys)) {
                logf_("macOS %u.%u is older than the bundled compiler needs (%u.%u): using %s\n", have >> 16, have >> 8 & 255, need >> 16, need >> 8 & 255, sys);
                arg_free(&cc); arg(&cc, sys);
            } else {
                snprintf(err, errn, "The game is made on your Mac the first time it starts. The compiler that comes with it needs "
                         "macOS %u or newer, and this Mac has macOS %u.%u.\n\nInstall Apple's command line tools instead "
                         "(in Terminal: xcode-select --install), then start the game again.", need >> 16, have >> 16, have >> 8 & 255);
                return -1;
            }
        }
    }
#endif
    if ((strchr(cc.v[0], '/') || strchr(cc.v[0], '\\')) && !is_file(cc.v[0])) {
#ifndef _WIN32
        /* no bundled compiler (a smaller download): use the system's own C compiler, if it has one */
        char sys[1300];
        if (system_cc(sys, sizeof sys)) { logf_("no bundled compiler (%s): using %s\n", cc.v[0], sys); arg_free(&cc); arg(&cc, sys); }
        else {
#ifdef __APPLE__
            snprintf(err, errn, "The game is made with a C compiler, and none was found.\n\nInstall Apple's command line tools "
                     "(in Terminal: xcode-select --install), then start the game again.");
#else
            snprintf(err, errn, "The game is made with a C compiler, and none was found.\n\nInstall gcc from your distribution "
                     "(e.g. sudo apt install gcc, sudo pacman -S gcc or sudo dnf install gcc), then start the game again.");
#endif
            return -1;
        }
#else
        snprintf(err, errn, "The compiler that makes the game is missing:\n%s\n\nPlease set the game up again with cm9798setup.", cc.v[0]); return -1;
#endif
    }
    set_env("ZIG_GLOBAL_CACHE_DIR", cache); set_env("ZIG_LOCAL_CACHE_DIR", cache);
    if (read_args("cflags.txt", &cfl, err, errn) <= 0) {
        const char *d[] = { "-O1", "-w", "-std=c11", "-fno-strict-aliasing", "-fwrapv", NULL };
        for (int i = 0; d[i]; i++) arg(&cfl, d[i]);
    }
    char inc1[1400], inc2[1400]; snprintf(inc1, sizeof inc1, "-I%s/include", kit); snprintf(inc2, sizeof inc2, "-I%s", gen);
    src_t src[64]; int ns = 0;
    { DIR *d = opendir(gen); struct dirent *e;
      while (d && (e = readdir(d)) && ns < 64) {
          size_t L = strlen(e->d_name); if (L < 3 || strcmp(e->d_name + L - 2, ".c")) continue;
          snprintf(src[ns].path, sizeof src[ns].path, "%s/%s", gen, e->d_name);
          struct stat st; src[ns].size = stat(src[ns].path, &st) ? 0 : (long)st.st_size; ns++;
      }
      if (d) closedir(d); }
    qsort(src, ns, sizeof *src, by_size);              /* biggest first: they take longest */
    long total = 0, done = 0; for (int i = 0; i < ns; i++) total += src[i].size;
    /* the biggest files take 1-2 GB each to compile: no more at once than the memory allows (a compiler
       the system stops for lack of memory says nothing); a file that fails is tried again on its own */
    int jobs = ncpu(), by_mem = (int)(mem_gb() / 2.5);
    if (jobs > by_mem) jobs = by_mem;
    if (jobs > 16) jobs = 16; if (jobs < 1) jobs = 1;
    logf_("compiling %d files, %d at a time (%d processors, %.0f GB)\n", ns, jobs, ncpu(), mem_gb());
    proc_t run[16]; int run_i[16], nrun = 0, next = 0, failed = 0, retry[64], nretry = 0;
    char objs[64][1400];
    while ((next < ns || nrun) && !failed && !ui_cancelled) {
        while (nrun < jobs && next < ns) {
            const char *base = strrchr(src[next].path, '/') + 1;
            snprintf(objs[next], sizeof objs[next], "%s/%.*s.o", obj, (int)(strlen(base) - 2), base);
            args_t a = {0};
            for (int i = 0; i < cc.n; i++) arg(&a, cc.v[i]);
            arg(&a, "-c"); for (int i = 0; i < cfl.n; i++) arg(&a, cfl.v[i]);
            arg(&a, inc1); arg(&a, inc2); arg(&a, src[next].path); arg(&a, "-o"); arg(&a, objs[next]);
            for (int i = 0; i < a.n; i++) logf_("%s%s", i ? " " : "", a.v[i]);
            logf_("\n");
            if (spawn(a.v, 1, &run[nrun])) { snprintf(err, errn, "Couldn't start the compiler (%s).", cc.v[0]); arg_free(&a); failed = 1; break; }
            run_i[nrun++] = next++; arg_free(&a);
        }
        for (int k = 0; k < nrun; k++) {
            int st;
            if (poll_proc(run[k], &st)) {
                if (st) {
                    logf_("%s: the compiler stopped (status %d)%s\n", strrchr(src[run_i[k]].path, '/') + 1, st, st > 128 ? " - stopped by the system, probably for memory" : "");
                    retry[nretry++] = run_i[k];
                } else done += src[run_i[k]].size;
                run[k] = run[nrun - 1]; run_i[k] = run_i[nrun - 1]; nrun--; k--;
            }
        }
        char what[96]; snprintf(what, sizeof what, "Compiling (%d of %d)", next - nrun, ns);
        ui_progress(150 + (int)(800.0 * done / (total ? total : 1)), what);
        nap();
    }
    /* what failed, once more, one at a time */
    for (int r = 0; r < nretry && !failed && !ui_cancelled; r++) {
        int i = retry[r]; args_t a = {0};
        for (int j = 0; j < cc.n; j++) arg(&a, cc.v[j]);
        arg(&a, "-c"); for (int j = 0; j < cfl.n; j++) arg(&a, cfl.v[j]);
        arg(&a, inc1); arg(&a, inc2); arg(&a, src[i].path); arg(&a, "-o"); arg(&a, objs[i]);
        char what[96]; snprintf(what, sizeof what, "Compiling %s again", strrchr(src[i].path, '/') + 1);
        int st = run_wait(a.v, 150 + (int)(800.0 * done / (total ? total : 1)), what);
        arg_free(&a);
        if (st) { failed = 1; snprintf(err, errn, "The game couldn't be compiled (%s).", strrchr(src[i].path, '/') + 1); }
        else done += src[i].size;
    }
    if (failed || ui_cancelled) {
        for (int k = 0; k < nrun; k++) kill_proc(run[k]);
        if (ui_cancelled) snprintf(err, errn, "Cancelled.");
        arg_free(&cc); arg_free(&cfl); return -1;
    }

    /* 3. link */
    snprintf(game, sizeof game, "%s/" GAME_NAME, kit);
    snprintf(tmp, sizeof tmp, "%s/%s.new" EXE, work, "cm9798-game");
    args_t a = {0};
    for (int i = 0; i < cc.n; i++) arg(&a, cc.v[i]);
    arg(&a, "-o"); arg(&a, tmp);
    for (int i = 0; i < ns; i++) arg(&a, objs[i]);
    { char lib[1400]; snprintf(lib, sizeof lib, "%s/lib/libcm9798rt.a", kit); arg(&a, lib); }
    if (read_args("link.txt", &lnk, err, errn) < 0) { arg_free(&a); return -1; }
    for (int i = 0; i < lnk.n; i++) arg(&a, lnk.v[i]);
    remove(tmp);
    int st = run_wait(a.v, 960, "Linking");
    arg_free(&a); arg_free(&cc); arg_free(&cfl); arg_free(&lnk);
    if (st || !is_file(tmp)) { if (!ui_cancelled) snprintf(err, errn, "The game couldn't be linked."); else snprintf(err, errn, "Cancelled."); return -1; }
#ifndef _WIN32
    chmod(tmp, 0755);
#endif
#ifdef __APPLE__
    if (is_file("/usr/bin/codesign")) {                 /* Apple Silicon runs only signed programs */
        char *v[] = { "/usr/bin/codesign", "-s", "-", "-f", tmp, NULL }; run_wait(v, 980, "Signing");
    }
#endif
    remove(game);
    { char old[1300]; snprintf(old, sizeof old, "%s/" GAME_NAME, progdir); remove(old); }   /* where earlier versions put it */
    if (rename(tmp, game)) { snprintf(err, errn, "Couldn't put the game in place:\n%s\n(%s)", game, strerror(errno)); return -1; }
    /* done: remember what it was made from */
    char stamp[64], p[1400]; make_stamp(stamp, sizeof stamp);
    snprintf(p, sizeof p, "%s/game.stamp", kit);
    FILE *f = fopen(p, "w"); if (f) { fputs(stamp, f); fclose(f); }
    logf_("made %s\n", game);
    tidy_kit();
    ui_progress(1000, "Done");
    return 0;
}

/* Once the game is made, the kit isn't needed: only build/cm9798-game, game.stamp and build.log stay
   (installing a newer release brings a new kit, and the game is made again). CM_KEEP_BUILD=1 keeps it. */
static void rm_tree(const char *path) {
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1600]; snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        if (is_dir(p)) rm_tree(p);
        else {
#ifdef _WIN32
            SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);   /* read-only files can't be removed */
#endif
            remove(p);
        }
    }
    closedir(d);
#ifdef _WIN32
    _rmdir(path);
#else
    rmdir(path);
#endif
}
static void tidy_kit(void) {
    const char *e = getenv("CM_KEEP_BUILD"); if (e && *e && *e != '0') return;
    ui_progress(990, "Tidying up");
    DIR *d = opendir(kit); struct dirent *de; char names[64][256]; int n = 0;
    while (d && (de = readdir(d)) && n < 64) {
        const char *nm = de->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..") || !strcmp(nm, "game.stamp") || !strcmp(nm, "build.log") || !strncmp(nm, "cm9798-game", 11)) continue;
        snprintf(names[n++], sizeof names[0], "%s", nm);
    }
    if (d) closedir(d);
    for (int i = 0; i < n; i++) { char p[1600]; snprintf(p, sizeof p, "%s/%s", kit, names[i]); rm_tree(p); ui_progress(990, "Tidying up"); }
    logf_("tidied the build folder (CM_KEEP_BUILD=1 keeps it)\n");
}

/* ---- databases imported before 1.05 ----
 * The CM2 and 96/97 databases are made by cm9798setup from the player's own discs (databases/<folder>/,
 * with a database.cfg starting "# made by the installer"). Before 1.05 the importer gave a few league
 * clubs no players or manager (Rio Ave with the Portuguese edition, UD Salamanca and Hercules with the
 * Spanish one). From 1.05 database.cfg says "importer = 1.05"; one made by the installer without it is
 * older, and the player is asked to import it again - or told once and left alone ("Don't remind me"
 * adds "importer = old" to its database.cfg). */
static void check_databases(const char *game_dir) {
    char dir[1400]; snprintf(dir, sizeof dir, "%s/databases", game_dir);
    DIR *d = opendir(dir); if (!d) return;
    char names[600] = "", cfgs[8][1500]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 8) {
        if (e->d_name[0] == '.') continue;
        char cfg[1500]; snprintf(cfg, sizeof cfg, "%s/%s/database.cfg", dir, e->d_name);
        FILE *f = fopen(cfg, "r"); if (!f) continue;
        char line[300], name[128] = ""; int installer = 0, marked = 0, first = 1;
        while (fgets(line, sizeof line, f)) {
            if (first && !strncmp(line, "# made by the installer", 23)) installer = 1;
            first = 0;
            char v[128];
            if (sscanf(line, " importer = %127s", v) == 1) marked = 1;
            else if (sscanf(line, " name = %127[^\r\n]", v) == 1) snprintf(name, sizeof name, "%s", v);
        }
        fclose(f);
        if (!installer || marked) continue;
        snprintf(cfgs[n++], sizeof cfgs[0], "%s", cfg);
        size_t l = strlen(names); snprintf(names + l, sizeof names - l, "  - %s (databases/%s)\n", name[0] ? name : e->d_name, e->d_name);
    }
    closedir(d);
    if (!n) return;
    char msg[1600];
    snprintf(msg, sizeof msg,
             "%s imported by an older version of cm9798setup:\n\n%s\n"
             "Version 1.05 fixes clubs that were left with no players or manager (Rio Ave, UD Salamanca, "
             "Hercules). To get the fixes, run cm9798setup again and import %s again.\n\n"
             "Careers already started keep the data they began with; new careers get the fixes.",
             n > 1 ? "These databases were" : "This database was", names, n > 1 ? "them" : "it");
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "OK" },
        { 0, 1, "Don't remind me" } };
    const SDL_MessageBoxData box = { SDL_MESSAGEBOX_WARNING, NULL, "CM 97/98 - database to import again", msg, 2, buttons, NULL };
    int choice = 0;
    if (SDL_ShowMessageBox(&box, &choice) == 0 && choice == 1)
        for (int i = 0; i < n; i++) { FILE *f = fopen(cfgs[i], "a"); if (f) { fputs("\n# cm9798 was told not to ask for this one to be imported again\nimporter = old\n", f); fclose(f); } }
}

/* CM2E16.EXE: in the folder given on the command line, else here, else next to this program (as rt/main.c) */
static int find_exe(const char *dir, char *out, size_t n) {
    const char *v[2] = { "CM2E16.EXE", "cm2e16.exe" };
    for (int i = 0; i < 2; i++) { snprintf(out, n, "%s/%s", dir, v[i]); if (is_file(out)) return 1; }
    return 0;
}

int main(int argc, char **argv) {
    program_dir(progdir, sizeof progdir);
    snprintf(kit, sizeof kit, "%s/build", progdir);
    snprintf(logpath, sizeof logpath, "%s/build.log", kit);
    int rebuild = 0; const char *gamedir = NULL;
    args_t pass = {0};
    /* the game: in the build folder; a developer's own build (no build folder) has it next to this program */
    char game[1300]; snprintf(game, sizeof game, "%s/" GAME_NAME, is_dir(kit) ? kit : progdir);
    arg(&pass, game);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rebuild")) { rebuild = 1; continue; }
        if (!strncmp(argv[i], "-psn", 4)) continue;      /* macOS Finder's process id */
        if (!gamedir && argv[i][0] != '-') gamedir = argv[i];
        arg(&pass, argv[i]);
    }
    int have_game = is_file(game);
    if (have_game && !rebuild && (!is_dir(kit) || stamp_ok())) {   /* (no build folder: a developer's own build) */
        if (is_dir(kit) && kit_complete()) tidy_kit();          /* the same release set up again: nothing to make */
        goto run;
    }
    {
        char exe[1300], err[2048] = "";
        if (is_dir(kit) && !kit_complete()) {
            ui_error("The files the game is made from were tidied away after it was made.\n"
                     "To make it again, set the game up again with cm9798setup.");
            return 1;
        }
        if (!is_dir(kit)) {
            ui_error("The build folder is missing from the program's folder, so the game can't be made.\n"
                     "Please set the game up again with cm9798setup.");
            return 1;
        }
        if (!(gamedir && find_exe(gamedir, exe, sizeof exe)) && !find_exe(".", exe, sizeof exe) && !find_exe(progdir, exe, sizeof exe)) {
            ui_error("CM2E16.EXE wasn't found.\n\nThe game is made from your own Championship Manager 97/98 v2.93:\n"
                     "set it up with cm9798setup, then start cm9798 from the folder it made.");
            return 1;
        }
        ui_open();
        ui_progress(0, "Starting");
        int r = build(exe, err, sizeof err);
        if (r) {
            char tail[1200] = "", msg[3400];
            if (strstr(err, "compiled") || strstr(err, "linked")) log_tail(tail, sizeof tail, 8);   /* what the compiler said */
            if (ui_cancelled) { ui_close(); return 1; }
            snprintf(msg, sizeof msg, "The game couldn't be made from CM2E16.EXE.\n\n%s\n\n%s%s\n(The full details are in %s)", err,
                     tail[0] ? "The compiler said:\n" : "", tail, logpath);
            ui_error(msg); ui_close(); return 1;
        }
        ui_close();
    }
run:
    if (is_dir(kit)) unquarantine();
    check_databases(gamedir ? gamedir : progdir);
    /* started without a game folder: start the game in this program's folder (the game folder), as the
       game itself looks for CM2E16.EXE where it's started and next to itself */
    {
        char probe[1300]; int here = find_exe(".", probe, sizeof probe);
        const char *wd = (!gamedir && !here) ? progdir : NULL;
#ifdef _WIN32
        /* the SDL DLLs are in the game folder, not next to the game in build\ */
        { char path[32768]; const char *old = getenv("PATH");
          snprintf(path, sizeof path, "%s;%s", progdir, old ? old : ""); SetEnvironmentVariableA("PATH", path); }
        static char cmd[32768]; quote(cmd, sizeof cmd, pass.v);
        STARTUPINFOA si; PROCESS_INFORMATION pi; memset(&si, 0, sizeof si); si.cb = sizeof si;
        if (!CreateProcessA(game, cmd, NULL, NULL, FALSE, 0, NULL, wd, &si, &pi)) {
            char m[1500]; snprintf(m, sizeof m, "Couldn't start %s (error %lu).", game, (unsigned long)GetLastError()); ui_error(m); return 1; }
        CloseHandle(pi.hThread); WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess);
        return (int)code;
#else
        if (wd && chdir(wd) != 0) {}
        execv(game, pass.v);
        { char m[1500]; snprintf(m, sizeof m, "Couldn't start %s (%s).", game, strerror(errno)); ui_error(m); }
        return 1;
#endif
    }
}
