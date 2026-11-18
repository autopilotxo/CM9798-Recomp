/* install.c - sets up a folder for the CM 97/98 port from the player's own copy of the game.
 *
 * The player's game folder is only read. Into the new folder go:
 *   - the game files the port needs (CM2E16.EXE v2.93, fonts, match engine tables, commentary,
 *     hiscores; the DOS editors, utilities and documents are left behind);
 *   - databases/9798/        PLAYERS.DB1 TMDATA.DB1 MGDATA.DB1 PLHIST98.BIN + database.cfg
 *   - databases/9798/pictures/  every .PCX picture
 *   - saves/9798/            GAMESS16.IDX (the slot list) and save1..save7, with any autosaves
 *                            and backups an earlier version of the port made (autosaves/slotN,
 *                            backups/slotN); a folder already set up for the port is copied as is
 *   - the port itself (cm9798.exe and its libraries), from the installer's own folder;
 *   - optionally Championship Manager 2 as databases/9596, or its 96/97 update as 9697 (cm2import.c).
 * Saved games and hiscores already in the new folder are never overwritten.
 * This is the same layout the game makes for itself on first run (rt/database.c).
 */
#include "install.h"
#include "cm2import.h"
#include "dlls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#else
#include <strings.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* the one executable the port was made from: CM 97/98 v2.93, 16 MB build (as rt/main.c) */
#define SUPPORTED_SIZE  1407137L
#define SUPPORTED_CRC32 0x359E908Eu

static install_log_fn logfn;
static void say(const char *fmt, ...) {
    char buf[2048]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    if (logfn) logfn(buf);
}

/* ------------------------------------------------------------------ files */
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
static int find_in(const char *dir, const char *name, char *out, size_t n) {   /* any case */
    DIR *d = opendir(dir); struct dirent *e; int ok = 0;
    if (!d) return 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}
static int mkdirs(const char *path) {                 /* make a folder and any missing parents */
    char p[1200]; snprintf(p, sizeof p, "%s", path);
    size_t n = strlen(p); while (n > 1 && (p[n - 1] == '/' || p[n - 1] == '\\')) p[--n] = 0;
    for (char *c = p + 1; *c; c++) {
        if (*c != '/' && *c != '\\') continue;
        if (c[-1] == ':') continue;                   /* "C:\" */
        char k = *c; *c = 0; if (!is_dir(p)) MKDIR(p); *c = k;
    }
    if (!is_dir(p)) MKDIR(p);
    return is_dir(p);
}
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"); if (!in) return 0;
    FILE *out = fopen(to, "wb"); if (!out) { fclose(in); return 0; }
    static char buf[1 << 18]; size_t n; int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in); if (fclose(out)) ok = 0;
    if (!ok) remove(to);
    return ok;
}
/* simple wildcard match, any case: '*' and '?' */
static int wild(const char *pat, const char *s) {
    if (!*pat) return !*s;
    if (*pat == '*') { for (;; s++) { if (wild(pat + 1, s)) return 1; if (!*s) return 0; } }
    if (!*s) return 0;
    if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s)) return 0;
    return wild(pat + 1, s + 1);
}

static int n_copied, n_kept, n_failed;
/* copy one file; keep = don't replace a file already there (saves, hiscores) */
static void put(const char *from, const char *to_dir, const char *name, int keep) {
    char to[1600], have[1600];
    if (!mkdirs(to_dir)) { say("  can't create %s\n", to_dir); n_failed++; return; }
    if (find_in(to_dir, name, have, sizeof have)) {
        if (keep) { n_kept++; return; }
        snprintf(to, sizeof to, "%s", have);          /* replace it under its own name */
    } else snprintf(to, sizeof to, "%s/%s", to_dir, name);
    if (copy_file(from, to)) n_copied++; else { say("  couldn't copy %s\n", from); n_failed++; }
}
/* copy the files of `from` matching `pat` into `to` (not folders) */
static int put_matching(const char *from, const char *pat, const char *to, int keep) {
    DIR *d = opendir(from); struct dirent *e; int k = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char p[1600]; snprintf(p, sizeof p, "%s/%s", from, e->d_name);
        if (e->d_name[0] == '.' || !wild(pat, e->d_name) || !is_file(p)) continue;
        put(p, to, e->d_name, keep); k++;
    }
    closedir(d);
    return k;
}
/* copy a whole folder tree */
static void put_tree(const char *from, const char *to, int keep) {
    DIR *d = opendir(from); struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1600], q[1700]; snprintf(p, sizeof p, "%s/%s", from, e->d_name);
        if (is_dir(p)) { snprintf(q, sizeof q, "%s/%s", to, e->d_name); put_tree(p, q, keep); }
        else if (is_file(p)) put(p, to, e->d_name, keep);
    }
    closedir(d);
}

/* the launcher's build kit (build/: the port's library, the data files and the compiler): copied as
   it is, file modes kept (the compiler is a program); what the launcher made there isn't copied */
static int put_kit(const char *from, const char *to) {
    DIR *d = opendir(from); struct dirent *e; int k = 0;
    if (!d) return 0;
    if (!mkdirs(to)) { closedir(d); say("  can't create %s\n", to); n_failed++; return 0; }
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (!strcmp(nm, ".") || !strcmp(nm, "..") || !strcmp(nm, "work") || !strcmp(nm, "game.stamp") || !strcmp(nm, "build.log")
            || !strncmp(nm, "cm9798-game", 11)) continue;
        char p[1600], q[1700]; snprintf(p, sizeof p, "%s/%s", from, nm); snprintf(q, sizeof q, "%s/%s", to, nm);
        /* the compiler unpacked from build/zig.zip (if cm9798 was run here): the archive is enough */
        if (!strcmp(nm, "zig")) {
            char a[1700]; int arc = 0; const char *z[] = { "zig.zip", "zig.tar.xz", "zig.tar.gz" };
            for (int i = 0; i < 3; i++) { snprintf(a, sizeof a, "%s/%s", from, z[i]); if (is_file(a)) arc = 1; }
            if (arc) continue;
        }
        if (is_dir(p)) { k += put_kit(p, q); continue; }
        if (!is_file(p)) continue;
        if (copy_file(p, q)) {
            k++;
#ifndef _WIN32
            struct stat st; if (!stat(p, &st)) chmod(q, st.st_mode & 0777);
#endif
        } else { say("  couldn't copy %s\n", p); n_failed++; }
    }
    closedir(d);
    return k;
}

#ifdef __APPLE__
#include <unistd.h>
#include <sys/wait.h>
/* macOS: the SDL frameworks (Frameworks/SDL2.framework, SDL2_ttf.framework) next to the programs: copied
   with their symbolic links (Versions/Current and the rest) kept as links */
static int put_links_tree(const char *from, const char *to) {
    DIR *d = opendir(from); struct dirent *e; int k = 0;
    if (!d) return 0;
    if (!mkdirs(to)) { closedir(d); say("  can't create %s\n", to); n_failed++; return 0; }
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1600], q[1700]; snprintf(p, sizeof p, "%s/%s", from, e->d_name); snprintf(q, sizeof q, "%s/%s", to, e->d_name);
        struct stat st; if (lstat(p, &st)) continue;
        if (S_ISLNK(st.st_mode)) {
            char t[1600]; ssize_t n = readlink(p, t, sizeof t - 1); if (n <= 0) continue; t[n] = 0;
            unlink(q); if (symlink(t, q) == 0) k++; else { say("  couldn't link %s\n", q); n_failed++; }
        } else if (S_ISDIR(st.st_mode)) k += put_links_tree(p, q);
        else if (S_ISREG(st.st_mode)) {
            if (copy_file(p, q)) { k++; chmod(q, st.st_mode & 0777); } else { say("  couldn't copy %s\n", p); n_failed++; }
        }
    }
    closedir(d);
    return k;
}
#endif

/* ------------------------------------------------------------------ checks */
static uint32_t crc32_file(const char *path, long *size) {
    FILE *f = fopen(path, "rb"); if (!f) { *size = -1; return 0; }
    static uint8_t buf[1 << 16]; size_t n; uint32_t c = 0xFFFFFFFFu; long total = 0;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        total += (long)n;
        for (size_t i = 0; i < n; i++) { c ^= buf[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1))); }
    }
    fclose(f); *size = total;
    return ~c;
}
static void exe_version(const char *path, char *out, size_t max) {   /* "2.xx" in the program */
    snprintf(out, max, "unknown");
    FILE *f = fopen(path, "rb"); if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n + 1);
    if (d && fread(d, 1, (size_t)n, f) == (size_t)n)
        for (long i = 1; i + 5 < n; i++)
            if (d[i - 1] == 0 && d[i] == '2' && d[i + 1] == '.' && isdigit(d[i + 2]) && isdigit(d[i + 3]) && d[i + 4] == 0) {
                snprintf(out, max, "%.4s", (const char *)d + i); break; }
    free(d); fclose(f);
}
/* the releases the importer knows: the data files it reads must match one of them exactly (CRC-32 and
   size), so a modified or unknown copy is turned away before anything is imported */
struct known_file { const char *name; long size; uint32_t crc; };
struct known_release { const char *what; struct known_file f[5]; };
static const struct known_release cm2_releases[] = {
    { "Championship Manager 2 (1995/96)", { { "TMDATA.DB1", 368474, 0xd996b5d7u }, { "PLDATA1.DB1", 683060, 0x36cffac5u },
        { "PLDATA2.DB1", 373831, 0xb2bb497bu }, { "MGDATA.DB1", 56094, 0xb8664c1cu }, { "PLHIST.BIN", 312638, 0x2401b8fcu } } },
    { "Championship Manager 96/97", { { "TMDATA.DB1", 396037, 0x8b905e4eu }, { "PLDATA1.DB1", 719171, 0x82474431u },
        { "PLDATA2.DB1", 375245, 0x7418c28eu }, { "MGDATA.DB1", 68626, 0xa91a26d3u }, { "PLHIST.BIN", 368608, 0x169687cau } } },
    { NULL, { { NULL, 0, 0 } } } };
static const struct known_release euro_releases[] = {
    /* (in the order of install_edition(): the six 1995/96 league editions, then the two 1996/97 releases) */
    { "French edition", { { "TMDATA.DB1", 323259, 0x1258d81fu }, { "PLDATA1.DB1", 71478, 0xed312b71u }, { "PLDATA2.DB1", 361416, 0x947920e9u },
        { "PLDATA3.DB1", 185696, 0x720e71e7u }, { "MGDATA.DB1", 10541, 0x09f2d5c3u } } },
    { "Italian edition", { { "TMDATA.DB1", 340703, 0xd3d7d111u }, { "PLDATA1.DB1", 56771, 0x614ce485u }, { "PLDATA2.DB1", 337732, 0xc7b9a347u },
        { "PLDATA3.DB1", 184550, 0xcc28ad58u }, { "MGDATA.DB1", 9581, 0x8885c455u } } },
    { "Spanish edition", { { "TMDATA.DB1", 339635, 0x25fb199du }, { "PLDATA1.DB1", 52760, 0xff7d044du }, { "PLDATA2.DB1", 354540, 0x8957b82du },
        { "PLDATA3.DB1", 182449, 0x13bbe620u }, { "MGDATA.DB1", 8877, 0xcda83a72u } } },
    { "German edition", { { "TMDATA.DB1", 325039, 0x9c0f04e4u }, { "PLDATA1.DB1", 71860, 0xebd1bb68u }, { "PLDATA2.DB1", 352821, 0x10e1ab63u },
        { "PLDATA3.DB1", 157619, 0xb7bd32e5u }, { "MGDATA.DB1", 9099, 0x87b4cd7eu } } },
    { "Dutch edition", { { "TMDATA.DB1", 350315, 0x7ac0fc86u }, { "PLDATA1.DB1", 56771, 0x5cf43b9cu }, { "PLDATA2.DB1", 379943, 0x21ef817fu },
        { "PLDATA3.DB1", 146541, 0x800503a3u }, { "MGDATA.DB1", 13919, 0x46036a31u } } },
    { "Belgian edition", { { "TMDATA.DB1", 381287, 0x240ea18au }, { "PLDATA1.DB1", 56198, 0x68289708u }, { "PLDATA2.DB1", 378606, 0xf4673d11u },
        { "PLDATA3.DB1", 162776, 0xb15d1252u }, { "MGDATA.DB1", 22595, 0x58cf3c53u } } },
    { "Italian Leagues", { { "TMDATA.DB1", 345040, 0xc8260b0bu }, { "PLDATA1.DB1", 56602, 0x15bc80bdu },
        { "PLDATA2.DB1", 329118, 0x8daa60e4u }, { "PLDATA3.DB1", 228951, 0x63b367ffu }, { "MGDATA.DB1", 15361, 0x88ce66b8u } } },
    { "Liga Portuguesa", { { "TMDATA.DB1", 372865, 0x2b3021bbu }, { "PLDATA1.DB1", 47531, 0xd2f75346u }, { "PLDATA2.DB1", 359226, 0xe76c5f20u },
        { "PLDATA3.DB1", 189386, 0x85e4b261u }, { "MGDATA.DB1", 10304, 0x145f3f2cu } } },
    { NULL, { { NULL, 0, 0 } } } };
/* which known release a folder holds (its index), or -1 with the reason in msg */
static int match_release(const char *dir, const struct known_release *r, const char *kind, char *msg, size_t n) {
    struct { char name[16]; long size; uint32_t crc; } got[8]; int ng = 0;    /* (each file read once) */
    int best = -1, best_ok = -1; const char *best_bad = NULL;
    for (int i = 0; r[i].what; i++) {
        int ok = 0; const char *bad = NULL;
        for (int k = 0; k < 5 && r[i].f[k].name; k++) {
            int g = 0; while (g < ng && strcmp(got[g].name, r[i].f[k].name)) g++;
            if (g == ng && ng < 8) {
                char p[1300]; snprintf(got[ng].name, sizeof got[ng].name, "%s", r[i].f[k].name);
                if (find_in(dir, r[i].f[k].name, p, sizeof p)) got[ng].crc = crc32_file(p, &got[ng].size);
                else { got[ng].size = -1; got[ng].crc = 0; }
                ng++;
            }
            if (got[g].size == r[i].f[k].size && got[g].crc == r[i].f[k].crc) ok++; else if (!bad) bad = r[i].f[k].name;
        }
        if (!bad) return i;
        if (ok > best_ok) { best_ok = ok; best = i; best_bad = bad; }
    }
    if (best >= 0 && best_ok > 0)
        snprintf(msg, n, "ERROR: %s isn't the original %s %s - it may have been edited or patched.", best_bad, r[best].what, kind);
    else snprintf(msg, n, "ERROR: these aren't the original files of any %s release the importer knows (%s differs).", kind, best_bad ? best_bad : "?");
    return -1;
}

int install_check_source(const char *dir, char *msg, size_t n) {
    char exe[1300];
    if (!dir[0]) { snprintf(msg, n, "Choose the folder your CM 97/98 is installed in."); return 0; }
    if (!is_dir(dir)) { snprintf(msg, n, "That folder doesn't exist."); return 0; }
    if (!find_in(dir, "CM2E16.EXE", exe, sizeof exe)) { snprintf(msg, n, "No CM2E16.EXE here - this isn't a CM 97/98 folder."); return 0; }
    long size; uint32_t crc = crc32_file(exe, &size);
    if (size != SUPPORTED_SIZE || crc != SUPPORTED_CRC32) {
        char v[16]; exe_version(exe, v, sizeof v);
        snprintf(msg, n, "Found version %s - this needs v2.93 (the final official release).", v); return 0;
    }
    char p[1300], db[1200]; snprintf(db, sizeof db, "%s/databases/9798", dir);
    if (!find_in(dir, "PLAYERS.DB1", p, sizeof p) && !find_in(db, "PLAYERS.DB1", p, sizeof p)) {
        snprintf(msg, n, "CM2E16.EXE v2.93 found, but the database (PLAYERS.DB1) is missing."); return 0; }
    snprintf(msg, n, "Championship Manager 97/98 v2.93 found.");
    return 1;
}
int install_check_cm2(const char *dir, char *msg, size_t n) {
    char p[1300];
    if (!dir[0]) { snprintf(msg, n, "Optional."); return 0; }
    if (!is_dir(dir)) { snprintf(msg, n, "That folder doesn't exist."); return 0; }
    static const char *const need[] = { "PLDATA1.DB1", "TMDATA.DB1", "MGDATA.DB1", "PLHIST.BIN", NULL };
    for (int i = 0; need[i]; i++) if (!find_in(dir, need[i], p, sizeof p)) {
        snprintf(msg, n, "No %s here - this isn't a Championship Manager 2 folder.", need[i]); return 0; }
    if (find_in(dir, "CM2E16.EXE", p, sizeof p)) { snprintf(msg, n, "That's a CM 97/98 folder, not Championship Manager 2."); return 0; }
    if (match_release(dir, cm2_releases, "data", msg, n) < 0) return 0;
    cm2_describe(dir, msg, n);
    return 1;
}
/* the league editions the importer takes, in install_opts.euro's order; season: the database they add to */
static const struct install_edition editions[INSTALL_NEDITIONS] = {
    { "the French edition",  "French",     "France",   1995 }, { "the Italian edition", "Italian", "Italy",   1995 },
    { "the Spanish edition", "Spanish",    "Spain",    1995 }, { "the German edition",  "German",  "Germany", 1995 },
    { "the Dutch edition",   "Dutch",      "Holland",  1995 }, { "the Belgian edition", "Belgian", "Belgium", 1995 },
    { "Italian Leagues",     "Italian",    "Italy",    1996 },   /* (releases of their own, with 1996/97's squads) */
    { "Liga Portuguesa",     "Portuguese", "Portugal", 1996 } };
const struct install_edition *install_edition(int k) { return k >= 0 && k < INSTALL_NEDITIONS ? &editions[k] : NULL; }
int install_euro_release(const char *dir) {
    char m[300]; return dir[0] && is_dir(dir) ? match_release(dir, euro_releases, "data", m, sizeof m) : -1;
}
int install_check_euro(const char *dir, char *msg, size_t n) {
    char p[1300];
    if (!dir[0]) { snprintf(msg, n, "Optional."); return 0; }
    if (!is_dir(dir)) { snprintf(msg, n, "That folder doesn't exist."); return 0; }
    static const char *const need[] = { "PLDATA1.DB1", "TMDATA.DB1", "MGDATA.DB1", NULL };
    for (int i = 0; need[i]; i++) if (!find_in(dir, need[i], p, sizeof p)) {
        snprintf(msg, n, "No %s here - this isn't a Championship Manager 2 folder.", need[i]); return 0; }
    if (find_in(dir, "PLHIST.BIN", p, sizeof p)) { snprintf(msg, n, "That's a main Championship Manager 2 game - it goes in the box above."); return 0; }
    if (!cm2_edition_country(dir)) { snprintf(msg, n, "No country in full here - this isn't one of CM2's league editions."); return 0; }
    int rel = match_release(dir, euro_releases, "data", msg, n);   /* (which release, by its files: Italy has two) */
    if (rel < 0) return 0;
    const struct install_edition *e = &editions[rel]; char name[64];
    snprintf(name, sizeof name, "%s", e->name[0] == 't' && e->name[1] == 'h' && e->name[2] == 'e' ? e->name + 4 : e->name);
    name[0] = (char)toupper((unsigned char)name[0]);
    snprintf(msg, n, "%s found - adds the %s leagues to the %s database.", name, e->adjective, e->season == 1995 ? "1995/96" : "1996/97");
    return 1;
}
/* is this folder already set up, with the 97/98 database (databases/9798/PLAYERS.DB1)? */
int install_has_base(const char *dir) {
    char p[1300], have[1300];
    if (!dir[0]) return 0;
    snprintf(p, sizeof p, "%s/databases/9798", dir);
    return find_in(p, "PLAYERS.DB1", have, sizeof have);
}
int install_is_port_folder(const char *dir) {
    char p[1300]; snprintf(p, sizeof p, "%s/databases", dir);
    return is_dir(p);
}
static void canon(const char *in, char *out, size_t n) {
#ifdef _WIN32
    if (!_fullpath(out, in, n)) snprintf(out, n, "%s", in);
#else
    char *r = realpath(in, NULL); snprintf(out, n, "%s", r ? r : in); free(r);
#endif
    size_t k = strlen(out);
    for (size_t i = 0; i < k; i++) if (out[i] == '\\') out[i] = '/';
    while (k > 1 && out[k - 1] == '/') out[--k] = 0;
}
int install_same_folder(const char *a, const char *b) {
    char x[1200], y[1200]; canon(a, x, sizeof x); canon(b, y, sizeof y);
#ifdef _WIN32
    return !strcasecmp(x, y);
#else
    return !strcmp(x, y);
#endif
}

/* ------------------------------------------------------------------ install */
/* the files the port reads from the game folder (beyond the database, pictures and saves) */
static const char *const game_files[] = { "CM2E16.EXE", "MY?.FNT", "PRECALCS.BIN", "EXNMES.TXT", "ENG_MTCH.TXT",
                                          "MTCHDSC*.TXT", "PITCH.GFX", "CONFIG.DAT", NULL };
static const char *const db_files[] = { "PLAYERS.DB1", "TMDATA.DB1", "MGDATA.DB1", "PLHIST98.BIN", NULL };

static void write_cfg_9798(const char *dir) {
    char p[1300], have[1300];
    if (find_in(dir, "database.cfg", have, sizeof have)) return;
    snprintf(p, sizeof p, "%s/database.cfg", dir);
    FILE *f = fopen(p, "w"); if (!f) return;
    fprintf(f, "# shown as \"Using <name> Database\"\nname = Championship Manager 97/98\n"
               "# the day a new game starts\nstart = Wednesday 16th July 1997\n"
               "# match sounds: shared (the game folder's SOUNDS) | own (this folder's sounds folder)\nsounds = shared\n");
    fclose(f);
}

int install_run(const struct install_opts *o, install_log_fn log) {
    char msg[300], p[1300], q[1300];
    logfn = log; n_copied = n_kept = n_failed = 0;
    /* no 97/98 folder given and the destination is already set up: only import the CM2 databases into it */
    int import_only = !o->src[0] && install_has_base(o->dst);
    if (import_only && !o->cm2[0] && !o->cm96[0]) {
        say("Nothing to import - give a Championship Manager 2 or CM2 96/97 Update folder (or your 97/98 folder to set the game up again).\n");
        return -1; }
    if (!import_only && !install_check_source(o->src, msg, sizeof msg)) { say("%s\n", msg); return -1; }
    {   /* the databases to import: each must be a release the importer knows (checked before anything is copied) */
        int bad = 0;
        if (o->cm2[0] && !install_check_cm2(o->cm2, msg, sizeof msg)) { say("Championship Manager 2 folder %s:\n  %s\n", o->cm2, msg); bad = 1; }
        if (o->cm96[0] && !install_check_cm2(o->cm96, msg, sizeof msg)) { say("Championship Manager 96/97 folder %s:\n  %s\n", o->cm96, msg); bad = 1; }
        for (int k = 0; k < INSTALL_NEDITIONS; k++)
            if (o->euro[k][0] && !install_check_euro(o->euro[k], msg, sizeof msg)) { say("League edition folder %s:\n  %s\n", o->euro[k], msg); bad = 1; }
        if (bad) { say("\nERROR: nothing was installed - fix the folders above first.\n"); return -1; }
    }
    if (!o->dst[0]) { say("Choose a folder to install to.\n"); return -1; }
    if (import_only) { say("Importing into %s (already set up: the game, its 97/98 database and saved games are left as they are)\n", o->dst); goto imports; }
    if (install_same_folder(o->src, o->dst)) {
        say("Please install to a different folder from your original game - it is only read, never changed.\n"); return -1; }
    if (!mkdirs(o->dst)) { say("Can't create %s\n", o->dst); return -1; }
    say("Installing from %s\n           to %s\n\n", o->src, o->dst);
    int port_src = install_is_port_folder(o->src);

    /* game files */
    say("Game files...\n");
    for (int i = 0; game_files[i]; i++) put_matching(o->src, game_files[i], o->dst, 0);
    put_matching(o->src, "HISCORES.BIN", o->dst, 1);                   /* the player's hall of fame */
    if (port_src) put_matching(o->src, "cm9798.cfg", o->dst, 1);        /* the port's settings */
    say("  %d files\n", n_copied);

    /* the 97/98 database */
    say("Championship Manager 97/98 database...\n");
    char db[1200], dbsrc[1200]; snprintf(db, sizeof db, "%s/databases/9798", o->dst);
    snprintf(dbsrc, sizeof dbsrc, "%s/databases/9798", o->src);
    int before = n_copied;
    for (int i = 0; db_files[i]; i++) {
        if (find_in(dbsrc, db_files[i], p, sizeof p) || find_in(o->src, db_files[i], p, sizeof p)) {
            const char *b = strrchr(p, '/'); put(p, db, b ? b + 1 : p, 0);
        } else say("  %s is missing from your game folder\n", db_files[i]);
    }
    if (is_dir(dbsrc) && find_in(dbsrc, "database.cfg", p, sizeof p)) put(p, db, "database.cfg", 1);
    write_cfg_9798(db);
    say("  %d files\n", n_copied - before);

    /* pictures */
    say("Pictures...\n");
    snprintf(q, sizeof q, "%s/pictures", db); before = n_copied;
    put_matching(o->src, "*.PCX", q, 1);
    snprintf(p, sizeof p, "%s/pictures", dbsrc); if (is_dir(p)) put_matching(p, "*.PCX", q, 1);
    say("  %d picture%s\n", n_copied - before, n_copied - before == 1 ? "" : "s");

    /* other databases already set up for the port (e.g. 9596) */
    if (port_src) {
        snprintf(p, sizeof p, "%s/databases", o->src);
        DIR *d = opendir(p); struct dirent *e;
        if (d) {
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.' || !strcmp(e->d_name, "9798")) continue;
                char from[1600], to[1600]; snprintf(from, sizeof from, "%s/%s", p, e->d_name);
                if (!is_dir(from)) continue;
                snprintf(to, sizeof to, "%s/databases/%s", o->dst, e->d_name);
                say("Database %s...\n", e->d_name); put_tree(from, to, 1);
            }
            closedir(d);
        }
    }

    /* saved games */
    say("Saved games...\n");
    before = n_copied; int kept0 = n_kept;
    char saves[1200]; snprintf(saves, sizeof saves, "%s/saves", o->dst);
    snprintf(p, sizeof p, "%s/saves", o->src);
    if (is_dir(p)) put_tree(p, saves, 1);                              /* already in the port's layout */
    else {
        char s9[1300]; snprintf(s9, sizeof s9, "%s/9798", saves);
        if (find_in(o->src, "GAMESS16.IDX", p, sizeof p)) put(p, s9, "GAMESS16.IDX", 1);
        for (int slot = 1; slot <= 7; slot++) {
            char pat[16], sd[1400], from[1400], to[1500]; snprintf(pat, sizeof pat, "*%d.S16", slot);
            snprintf(sd, sizeof sd, "%s/save%d", s9, slot);
            put_matching(o->src, pat, sd, 1);
            snprintf(from, sizeof from, "%s/autosaves/slot%d", o->src, slot);      /* an earlier port's */
            if (is_dir(from)) { snprintf(to, sizeof to, "%s/autosave", sd); put_tree(from, to, 1); }
            snprintf(from, sizeof from, "%s/backups/slot%d", o->src, slot);
            if (is_dir(from)) { snprintf(to, sizeof to, "%s/backups", sd); put_tree(from, to, 1); }
        }
    }
    say("  %d files", n_copied - before);
    if (n_kept > kept0) say(" (%d already there, kept as they were)", n_kept - kept0);
    say("\n");

    /* the port itself */
    if (o->port_dir[0] && !install_same_folder(o->port_dir, o->dst)) {
        say("The port...\n"); before = n_copied;
#ifdef _WIN32
        put_matching(o->port_dir, "cm9798.exe", o->dst, 0);
        put_matching(o->port_dir, "*.dll", o->dst, 0);
        { char exe[1300];                                              /* and any it needs from elsewhere */
          if (find_in(o->port_dir, "cm9798.exe", exe, sizeof exe)) n_copied += copy_program_dlls(exe, o->dst, logfn); }
#else
        put_matching(o->port_dir, "cm9798", o->dst, 0);
        { char exe[1300]; snprintf(exe, sizeof exe, "%s/cm9798", o->dst); chmod(exe, 0755); }
#endif
        if (n_copied == before) say("  cm9798%s wasn't found next to the installer - copy it into the new folder yourself\n",
#ifdef _WIN32
                                    ".exe and SDL2.dll"
#else
                                    ""
#endif
                                    );
        else say("  %d files\n", n_copied - before);
        /* the build kit: the first time the game is started, cm9798 makes it from CM2E16.EXE with this */
        { char from[1300], to[1300]; snprintf(from, sizeof from, "%s/build", o->port_dir); snprintf(to, sizeof to, "%s/build", o->dst);
          if (is_dir(from)) {
              say("The build kit (the game is made from your CM2E16.EXE the first time it's started)...\n");
              int k = put_kit(from, to); n_copied += k; say("  %d files\n", k);   /* (an older game is made again: game.stamp) */
          } else say("  no build folder next to the installer - the game can't be made without it\n"); }
#ifdef __APPLE__
        { char from[1300], to[1300]; snprintf(from, sizeof from, "%s/Frameworks", o->port_dir); snprintf(to, sizeof to, "%s/Frameworks", o->dst);
          if (is_dir(from)) { say("SDL (Frameworks)...\n"); int k = put_links_tree(from, to); n_copied += k; say("  %d files\n", k); } }
        /* this program was opened from a download, so macOS marks what it writes as downloaded too, and
           would stop the launcher, the compiler and the game it makes ("killed"): clear the mark */
        { char *v[] = { "/usr/bin/xattr", "-dr", "com.apple.quarantine", (char *)o->dst, NULL };
          pid_t pid = fork();
          if (pid == 0) { execv(v[0], v); _exit(127); }
          if (pid > 0) { int st; waitpid(pid, &st, 0); } }
#endif
    }

imports:
    /* Championship Manager 2 and/or 96/97: the older first; 96/97 borrows CM2's pictures it lacks */
    ;
    const char *cm[2] = { o->cm2, o->cm96 }; int yr[2] = { 0, 0 }, import_failed = 0;
    for (int i = 0; i < 2; i++) if (cm[i][0]) { int d, m; if (!cm2_detect(cm[i], &yr[i], &d, &m)) yr[i] = 1995; }
    if (cm[0][0] && cm[1][0] && yr[0] > yr[1]) { const char *t = cm[0]; cm[0] = cm[1]; cm[1] = t; int y = yr[0]; yr[0] = yr[1]; yr[1] = y; }
    for (int i = 0; i < 2; i++) {
        if (!cm[i][0]) continue;
        const char *other = i == 1 && cm[0][0] && yr[0] < yr[1] ? cm[0] : NULL;
        say("\n");
        const char *euro[INSTALL_NEDITIONS]; int ne = 0;       /* the league editions of this one's season */
        for (int k = 0; k < INSTALL_NEDITIONS; k++) {
            int r = o->euro[k][0] ? install_euro_release(o->euro[k]) : -1;
            if (r >= 0 && editions[r].season == yr[i]) euro[ne++] = o->euro[k];
        }
        if (cm2_import(cm[i], o->dst, other, euro, ne, log) != 0) { say("\nThat one wasn't imported.\n"); import_failed = 1; }
    }
    for (int k = 0; k < INSTALL_NEDITIONS; k++) {            /* any left without the game they add to */
        int r = o->euro[k][0] ? install_euro_release(o->euro[k]) : -1; if (r < 0) continue;
        int have = (cm[0][0] && yr[0] == editions[r].season) || (cm[1][0] && yr[1] == editions[r].season);
        if (!have) say("\n%s was left out: it adds to the %s database, so give that folder too.\n", editions[r].name,
                       editions[r].season == 1995 ? "Championship Manager 2 (1995/96)" : "CM2 96/97 Update");
    }
    if (import_failed) { say("\nThe rest is installed.\n"); return -1; }

    if (n_failed) { say("\n%d files couldn't be copied (see above).\n", n_failed); return -1; }
    say("\nAll done.\n");
    return 0;
}
