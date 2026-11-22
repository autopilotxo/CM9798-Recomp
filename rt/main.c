/* main.c - host entry point for the recompiled Championship Manager 97/98 (CM2E16).
 *
 * usage: cm9798 [game folder]
 *   The folder must contain the player's own CM2E16.EXE and the game data files.
 *   Defaults to the current directory.
 */
#include "rt_internal.h"
#include "game_funcs.h"
#include "backup.h"
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <limits.h>
#ifdef _WIN32
#include <direct.h>
#define chdir _chdir
#define getcwd _getcwd
#else
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <io.h>
/* The Windows build has no console window. When started without one (double-click, shortcut),
   everything the program prints goes to cm9798.log in the game folder; started from a terminal,
   output stays in the terminal. */
static char g_log_path[MAX_PATH + 16];
static void redirect_output_to_log(void) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h != NULL && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN) return;
    extern const char *g_game_dir;                /* the game folder (the program may be in its build folder) */
    snprintf(g_log_path, sizeof g_log_path, "%s/cm9798.log", g_game_dir);
    if (!freopen(g_log_path, "w", stderr)) { g_log_path[0] = 0; return; }
    _dup2(_fileno(stderr), _fileno(stdout));
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(stderr, "Championship Manager 97/98 - log started %04d-%02d-%02d %02d:%02d:%02d\n",
            t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}
/* Windows: report native exceptions (incl. stack overflow, where a signal handler can't run) */
static LONG WINAPI on_win_exception(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    fprintf(stderr, "\n*** Windows exception 0x%08lX at %p%s\n", (unsigned long)code, ep->ExceptionRecord->ExceptionAddress,
            code == EXCEPTION_STACK_OVERFLOW ? " (STACK OVERFLOW)" : code == EXCEPTION_ACCESS_VIOLATION ? " (access violation)" : "");
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        uintptr_t fa = (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1];
        fprintf(stderr, "    faulting address %p = guest %08lx\n", (void *)fa, (unsigned long)(fa - (uintptr_t)M));
    }
    fprintf(stderr, "    guest EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n",
            eax, ebx, ecx, edx, esi, edi, ebp, esp);
    rt_dump_trace();
    fflush(stderr);
    if (g_log_path[0]) {                     /* no console window: tell the player where the details are */
        char msg[1400];
        snprintf(msg, sizeof msg, "The game has crashed.\n\nDetails were written to:\n%s\n\n"
                 "Progress since your last save is lost.", g_log_path);
        MessageBoxA(NULL, msg, "Championship Manager 97/98", MB_OK | MB_ICONERROR);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
static void install_win_handler(void) {
    ULONG guarantee = 64 * 1024; SetThreadStackGuarantee(&guarantee);   /* room to report a stack overflow */
    SetUnhandledExceptionFilter(on_win_exception);
}
#endif

extern void dos_setup(const char *dir);
extern int rt_load_exe(const char *path);
extern void host_init(void);
extern void host_message(const char *title, const char *text);   /* dialog box (SDL) or stderr */

/* the one executable this port was recompiled from: CM 97/98 v2.93, 16 MB build */
#define SUPPORTED_VERSION "2.93"
#define SUPPORTED_SIZE    1407137L
#define SUPPORTED_CRC32   0x359E908Eu

static uint32_t crc32_buf(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
/* find the game's version string ("2.xx") in an executable, for the error message */
static void find_version(const uint8_t *d, size_t n, char *out, size_t max) {
    snprintf(out, max, "unknown");
    for (size_t i = 1; i + 5 < n; i++)
        if (d[i - 1] == 0 && d[i] == '2' && d[i + 1] == '.' && d[i + 2] >= '0' && d[i + 2] <= '9' &&
            d[i + 3] >= '0' && d[i + 3] <= '9' && d[i + 4] == 0) { snprintf(out, max, "%.4s", (const char *)d + i); return; }
}
/* returns 0 if the file is the supported executable, otherwise shows a dialog and returns -1 */
static int check_version(const char *path) {
    if (getenv("CM_SKIP_VERSION_CHECK")) return 0;
    FILE *f = fopen(path, "rb"); if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n > 0 ? (size_t)n : 1);
    size_t got = d ? fread(d, 1, (size_t)n, f) : 0; fclose(f);
    int ok = d && got == (size_t)n && n == SUPPORTED_SIZE && crc32_buf(d, (size_t)n) == SUPPORTED_CRC32;
    if (!ok) {
        char ver[16], msg[1024]; find_version(d ? d : (const uint8_t *)"", d ? got : 0, ver, sizeof ver);
        snprintf(msg, sizeof msg,
            "Wrong version of Championship Manager 97/98.\n\n"
            "Found:     %s (version %s)\n"
            "Required: version " SUPPORTED_VERSION " (the final official release, 16 MB build CM2E16.EXE)\n\n"
            "Please put the v" SUPPORTED_VERSION " CM2E16.EXE in the game folder and try again.",
            path, ver);
        host_message("CM 97/98 - wrong game version", msg);
    }
    free(d);
    return ok ? 0 : -1;
}
extern uint32_t g_break_fn;

static void on_crash(int sig) {
    fprintf(stderr, "\n*** crash (signal %d). Guest regs: EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n",
            sig, eax, ebx, ecx, edx, esi, edi, ebp, esp);
    rt_dump_trace();
    _Exit(128 + sig);
}

/* say why we are exiting, so a silent close is never ambiguous */
static void report_exit(void) {
    if (getenv("CM_TRACE") || getenv("CM_INPUTLOG")) { fprintf(stderr, "[exit] program is exiting normally\n"); rt_dump_trace(); }
}

static FILE *open_exe(const char *dir, char *out, size_t max) {
    /* exact name first, then lower case (Windows is case-insensitive anyway) */
    const char *v[2] = { "CM2E16.EXE", "cm2e16.exe" };
    for (int i = 0; i < 2; i++) {
        snprintf(out, max, "%s/%s", dir, v[i]);
        FILE *f = fopen(out, "rb"); if (f) return f;
    }
    return NULL;
}

/* the folder this program is in ("" if unknown) */
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
    if (s) *s = 0; else out[0] = 0;
}

const char *g_game_dir = ".";   /* folder with the game files (sharp text looks for fonts there) */
int main(int argc, char **argv) {
    editor_startup();                                             /* before anything changes (rt/editor.c) */
    if (argc > 1 && !strncmp(argv[1], "-psn", 4)) argc = 1;      /* macOS Finder's process id, not a folder */
    const char *game = argc > 1 ? argv[1] : ".";
    /* Started without a folder from somewhere else (double-clicked in the Finder or a file manager,
       which starts programs in the home folder): if the game isn't here but is next to the program,
       work from the program's folder. Everything (saves, databases, settings) is relative to it. */
    if (argc <= 1) {
        char probe[1100]; FILE *pf = open_exe(".", probe, sizeof probe);
        if (pf) fclose(pf);
        else {
            static char pdir[1100]; program_dir(pdir, sizeof pdir);
            if (pdir[0] && (pf = open_exe(pdir, probe, sizeof probe)) == NULL) {
                /* the launcher keeps the game in the game folder's build folder: try the folder above */
                char *s1 = strrchr(pdir, '/'), *s2 = strrchr(pdir, '\\'), *s = s1 > s2 ? s1 : s2;
                if (s && s > pdir) { *s = 0; pf = open_exe(pdir, probe, sizeof probe); }
            }
            if (pdir[0] && pf) {
                fclose(pf);
                if (chdir(pdir) != 0) game = pdir;
            }
        }
    }
    char exe[1100]; FILE *f = open_exe(game, exe, sizeof exe);
    if (!f) {
        char msg[1400], shown[1100];
        if (!strcmp(game, ".") && getcwd(shown, sizeof shown)) {} else snprintf(shown, sizeof shown, "%s", game);
        snprintf(msg, sizeof msg,
            "CM2E16.EXE was not found in:\n%s\n\n"
            "This program needs the original Championship Manager 97/98 v" SUPPORTED_VERSION " files.\n"
#ifdef _WIN32
            "Copy cm9798.exe (and SDL2.dll) into your CM 97/98 folder and run it from there,\n"
            "or start it with the game folder as an argument, e.g.\n"
            "    cm9798.exe \"C:\\Games\\CM9798\"", shown);
#else
            "Set the game up with cm9798setup, then start cm9798 from the folder it made,\n"
            "or give the game folder as an argument, e.g.\n"
            "    ./cm9798 ~/Games/CM9798-game", shown);
#endif
        host_message("CM 97/98 - game files not found", msg);
        return 1;
    }
    fclose(f);
    g_game_dir = game;
    backup_init(game);
    db_count();                      /* databases/: found now, made on first run (database.c) */
    rt_init();
#ifdef _WIN32
    redirect_output_to_log();
    install_win_handler();
    setvbuf(stderr, NULL, _IONBF, 0); setvbuf(stdout, NULL, _IONBF, 0);
#else
    signal(SIGSEGV, on_crash); signal(SIGFPE, on_crash); signal(SIGILL, on_crash);
#endif

    if (check_version(exe)) return 1;
    int r = rt_load_exe(exe);
    if (r) { fprintf(stderr, "could not load %s (error %d) - is it the original CM 97/98 16MB executable?\n", exe, r); return 1; }

    /* optional development check against images produced by tools/img.py */
    const char *vdir = getenv("CM_VERIFY_IMG");
    if (vdir) {
        const char *names[2] = { "obj1_01010000.bin", "obj2_010e0000.bin" }; uint32_t at[2] = { 0x01010000u, 0x010E0000u };
        for (int i = 0; i < 2; i++) {
            char p[1200]; snprintf(p, sizeof p, "%s/%s", vdir, names[i]);
            FILE *v = fopen(p, "rb"); if (!v) { fprintf(stderr, "verify: missing %s\n", p); continue; }
            long diff = 0, len = 0; int c;
            while ((c = fgetc(v)) != EOF) { if (M[at[i] + len] != (uint8_t)c) diff++; len++; }
            fclose(v);
            fprintf(stderr, "verify %s: %ld bytes, %ld differ\n", names[i], len, diff);
        }
        return 0;
    }

    { extern void apply_text_patches(void); apply_text_patches(); }   /* overrides.c */
    dos_setup(game);
    { extern int g_os_pointer; if (getenv("CM_SOFT_POINTER")) g_os_pointer = 0; }   /* original software pointer */
    host_init();
    { extern uint32_t g_watch_addr; if (getenv("CM_WATCH")) g_watch_addr = (uint32_t)strtoul(getenv("CM_WATCH"), 0, 16); }
    if (getenv("CM_BREAK_FN")) g_break_fn = (uint32_t)strtoul(getenv("CM_BREAK_FN"), 0, 16) + 0x01000000u;
    if (getenv("CM_TRACE")) fprintf(stderr, "CM2E16 recompiled: %u functions, entering _cstart_\n", g_nfuncs);
    atexit(report_exit);
    PUSH32(0xFFFFFFF0u);          /* sentinel return address */
    f_10bdc64();                  /* _cstart_ (original 0x0BDC64) */
    return (int)eax;
}
