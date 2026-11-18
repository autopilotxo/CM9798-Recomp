/* dlls.c - copies the DLLs a Windows program needs into a folder, so it runs on a PC without the
 * build tools: SDL2.dll, SDL2_ttf.dll and whatever they need in turn. Each is looked for next to
 * the program, then on the PATH (where an MSYS2 build finds them); Windows' own DLLs are left out.
 * Used by the installer (install.c) and by the build (copydlls.c: copies them into build/).
 */
#include "dlls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#ifdef _WIN32
#include <windows.h>
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#endif

static int find_in(const char *dir, const char *name, char *out, size_t n) {   /* any case */
    DIR *d = opendir(dir); struct dirent *e; int ok = 0;
    if (!d) return 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}
static int same_dir(const char *a, const char *b) {
    char x[1200], y[1200];
#ifdef _WIN32
    if (!_fullpath(x, a, sizeof x)) snprintf(x, sizeof x, "%s", a);
    if (!_fullpath(y, b, sizeof y)) snprintf(y, sizeof y, "%s", b);
#else
    snprintf(x, sizeof x, "%s", a); snprintf(y, sizeof y, "%s", b);
#endif
    for (char *p = x; *p; p++) if (*p == '\\') *p = '/';
    for (char *p = y; *p; p++) if (*p == '\\') *p = '/';
    size_t i = strlen(x), j = strlen(y);
    while (i > 1 && x[i - 1] == '/') x[--i] = 0;
    while (j > 1 && y[j - 1] == '/') y[--j] = 0;
    return !strcasecmp(x, y);
}
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"); if (!in) return 0;
    FILE *out = fopen(to, "wb"); if (!out) { fclose(in); return 0; }
    static char buf[1 << 18]; size_t n; int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in); if (fclose(out)) ok = 0;
    return ok;
}

/* the DLLs a Windows program loads at start, from its import table */
static int pe_imports(const char *path, char names[][64], int max) {
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)n + 1); int k = 0;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
#define U16(o) ((o) + 2 <= n ? (uint32_t)(b[o] | b[(o) + 1] << 8) : 0u)
#define U32(o) ((o) + 4 <= n ? (uint32_t)(b[o] | b[(o) + 1] << 8 | b[(o) + 2] << 16 | (uint32_t)b[(o) + 3] << 24) : 0u)
    long pe = (long)U32(0x3C);
    if (n < 0x40 || pe <= 0 || pe + 24 > n || memcmp(b + pe, "PE\0\0", 4)) { free(b); return 0; }
    uint32_t nsec = U16(pe + 6), optsz = U16(pe + 20); long opt = pe + 24;
    int pe64 = U16(opt) == 0x20B;
    uint32_t imp = U32(opt + (pe64 ? 120 : 104));                   /* data directory 1: imports */
    long sec = opt + (long)optsz;
#define RVA2OFF(r, out) do { out = -1; for (uint32_t i_ = 0; i_ < nsec; i_++) { long s_ = sec + (long)i_ * 40; \
        uint32_t va_ = U32(s_ + 12), sz_ = U32(s_ + 8), raw_ = U32(s_ + 20); if (sz_ < U32(s_ + 16)) sz_ = U32(s_ + 16); \
        if ((r) >= va_ && (r) < va_ + sz_) { out = (long)(raw_ + ((r) - va_)); break; } } } while (0)
    long d; RVA2OFF(imp, d);
    for (; imp && d >= 0 && d + 20 <= n && k < max; d += 20) {
        uint32_t name = U32(d + 12); if (!name && !U32(d)) break;
        long o; RVA2OFF(name, o); if (o < 0 || o >= n) continue;
        snprintf(names[k++], 64, "%.63s", (const char *)b + o);
    }
    free(b);
    return k;
}

static char seen[96][64]; static int nseen;
static int walk(const char *exe, const char *exe_dir, const char *to, void (*log)(const char *)) {
    int copied = 0;
#ifdef _WIN32
    char names[128][64]; int k = pe_imports(exe, names, 128);
    char sys[MAX_PATH] = ""; GetSystemDirectoryA(sys, sizeof sys);
    for (int i = 0; i < k; i++) {
        const char *nm = names[i]; int already = 0;
        if (!strncasecmp(nm, "api-ms-", 7) || !strncasecmp(nm, "ext-ms-", 7)) continue;
        for (int j = 0; j < nseen; j++) if (!strcasecmp(seen[j], nm)) already = 1;
        if (already || nseen == 96) continue;
        snprintf(seen[nseen++], 64, "%s", nm);
        char p[1300];
        if (find_in(sys, nm, p, sizeof p)) continue;                /* part of Windows */
        int found = find_in(exe_dir, nm, p, sizeof p);
        const char *path = getenv("PATH");
        while (!found && path && *path) {
            char dir[1100]; size_t L = strcspn(path, ";");
            snprintf(dir, sizeof dir, "%.*s", (int)L, path);
            if (dir[0]) found = find_in(dir, nm, p, sizeof p);
            path += L; if (*path == ';') path++;
        }
        if (!found) {
            if (log) { char m[200]; snprintf(m, sizeof m, "  %s not found - the game may not start without it\n", nm); log(m); }
            continue;
        }
        char *sl = strrchr(p, '/'); char dir[1300]; snprintf(dir, sizeof dir, "%.*s", (int)(sl ? sl - p : 0), p);
        if (!same_dir(dir, to)) {                                   /* (already there: nothing to do) */
            char dst[1500], have[1500];
            if (find_in(to, sl ? sl + 1 : nm, have, sizeof have)) snprintf(dst, sizeof dst, "%s", have);
            else snprintf(dst, sizeof dst, "%s/%s", to, sl ? sl + 1 : nm);
            if (copy_file(p, dst)) copied++;
            else if (log) { char m[1600]; snprintf(m, sizeof m, "  couldn't copy %s\n", p); log(m); }
        }
        copied += walk(p, dir, to, log);                              /* what it needs in turn */
    }
#else
    (void)exe; (void)exe_dir; (void)to; (void)log; (void)pe_imports;
#endif
    return copied;
}
int copy_program_dlls(const char *exe, const char *to, void (*log)(const char *)) {
    char dir[1300]; snprintf(dir, sizeof dir, "%s", exe);
    char *a = strrchr(dir, '/'), *b = strrchr(dir, '\\'), *s = a > b ? a : b;
    if (s) *s = 0; else snprintf(dir, sizeof dir, ".");
    nseen = 0;
    return walk(exe, dir, to, log);
}
