/* cm2import.c - turns a Championship Manager 2 database (1995/96, or the 1996/97 update) into one
 * the CM 97/98 port can play: <game folder>/databases/9596/ or 9697/. Part of the installer:
 *
 *   int cm2_import(const char *cm2_folder, const char *game_folder, const char *pictures_from,
 *                  const char *addon, cm2_log_fn log)
 *
 * Which season: the newest season in its player histories (PLHIST.BIN) is the one before; the
 * day and month a new game starts are read from its program (1 August in CM2).
 * A record of everything done is written to databases/<season>/convert.log.
 *
 * Reads from CM2:  PLDATA1.DB1 (+ PLDATA2.DB1), TMDATA.DB1, MGDATA.DB1, PLHIST.BIN
 * Reads from 97/98 (databases/9798/ or the game folder): PLAYERS.DB1, TMDATA.DB1, MGDATA.DB1
 *   - the 97/98 file layouts are used as templates (the engine reads fields by position);
 *   - 97/98's clubs are the base list, so the special entries (Free Transfer, Retired, ...), the
 *     national teams and the leagues of the other countries stay as the engine expects them.
 * Writes: PLAYERS.DB1, TMDATA.DB1, MGDATA.DB1, PLHIST98.BIN, database.cfg
 *
 * Clubs: each CM2 club is matched to a 97/98 club (by name, allowing for different spellings);
 *   CM2's details replace 97/98's where CM2 has them, and for the leagues CM2 covers (England,
 *   Scotland, Italy) CM2's divisions are used and 97/98 clubs CM2 doesn't have leave the league.
 *   CM2 clubs with no 97/98 match are added. Matched clubs keep their 97/98 names.
 * Players: all CM2 players, at the clubs found above (CM2 often shortens club names). Fields 97/98
 *   has and CM2 hasn't: Unique ID (numbered), Reputation (from ability), the rest left empty as in
 *   most 97/98 records. Nationalities are put in 97/98's spelling.
 * Managers: CM2's, plus 97/98's that don't clash with them (other countries' clubs and national
 *   teams); 97/98 managers of English, Scottish or Italian clubs become unattached.
 * Player histories: CM2's, in 97/98's layout.
 * French European Leagues (96/97's French edition, optional): its 65 French clubs, with their
 *   divisions (FD1, FD2, FNL), players and managers, replace the English edition's; English-edition
 *   players it also has (same name and birth date) are left out, keeping their reputation for the
 *   French record. The French league can then be chosen. It has no player histories of its own.
 * Managers: the engine holds about 1000 (more stops a new game); past 990, unattached 97/98
 *   managers are left out, the least known first.
 * Pictures: CM2's club pictures (the .PCX files in STADPICS) go to databases/9596/pictures, where the port
 *   looks first. 97/98 finds a club's picture by the same naming as CM2 (short name, spaces and
 *   dots as "_", 8 letters), so they work as they are, except two renamed to 97/98's club names.
 *   CM2's own backgrounds, STADPIC1-5, go there too: the port's "Championship Manager 2" backgrounds
 *   (F1, Graphics) show them the way CM2 does.
 *
 * All database numbers are big-endian. A file is: field count (u16 LE); per field: name length,
 * name, type (0 number, 1 text), size, 2 flags; record count (2 bytes: n>>7, n&127); if the file
 * has a 4-byte "Unique ID" field, 4 more bytes (u32 LE, next free ID); then fixed-size records.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#include "cm2import.h"
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#define strcasecmp _stricmp
#else
#include <strings.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

/* ------------------------------------------------------------------ files */
static FILE *logf;
static cm2_log_fn logfn;
static jmp_buf failed;
static void say(const char *fmt, ...) {
    char buf[2048]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    if (logfn) logfn(buf);
    if (logf) fputs(buf, logf);
}
static void die(const char *fmt, ...);

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
/* path of `name` in `dir`, whatever its case */
static int find_in(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(dir); struct dirent *e; int ok = 0;
    if (!d) return 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}
static uint8_t *load(const char *path, long *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)*len + 1);
    if (fread(b, 1, (size_t)*len, f) != (size_t)*len) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}

/* ------------------------------------------------------------------ databases */
typedef struct { char name[40]; int type, size, off; } Field;
typedef struct {
    uint8_t *hdr; int hdrlen;            /* field descriptions, as in the file */
    Field f[80]; int nf, rs;
    int n, cap; uint8_t *rec;            /* records */
    int has_uid;
} DB;

static void db_read(DB *d, const char *path) {
    long len; uint8_t *b = load(path, &len);
    if (!b) die("Can't read %s\n", path);
    memset(d, 0, sizeof *d);
    int nf = b[0] | b[1] << 8, o = 2, off = 0;
    if (nf <= 0 || nf > 80) die("%s doesn't look like a CM database file\n", path);
    for (int i = 0; i < nf; i++) {
        int l = b[o]; Field *f = &d->f[i];
        snprintf(f->name, sizeof f->name, "%.*s", l, (char *)b + o + 1); o += 1 + l;
        f->type = b[o]; f->size = b[o + 1]; f->off = off; off += f->size; o += 4;
        if (!strcmp(f->name, "Unique ID") && f->type == 0 && f->size == 4) d->has_uid = 1;
    }
    d->nf = nf; d->rs = off; d->hdrlen = o;
    d->hdr = malloc((size_t)o); memcpy(d->hdr, b, (size_t)o);
    int pre = d->has_uid ? 6 : 2;
    d->n = (int)((len - o - pre) / d->rs);
    int counted = b[o] << 7 | b[o + 1];
    if ((long)o + pre + (long)d->n * d->rs != len || counted != d->n)
        say("  note: %s: %d records by size, %d by its count\n", path, d->n, counted);
    d->cap = d->n + 16;
    d->rec = calloc((size_t)d->cap, (size_t)d->rs);
    memcpy(d->rec, b + o + pre, (size_t)d->n * d->rs);
    free(b);
}
static void db_template(DB *d, const DB *t) {   /* empty database with t's layout */
    memset(d, 0, sizeof *d);
    d->hdr = malloc((size_t)t->hdrlen); memcpy(d->hdr, t->hdr, (size_t)t->hdrlen); d->hdrlen = t->hdrlen;
    memcpy(d->f, t->f, sizeof d->f); d->nf = t->nf; d->rs = t->rs; d->has_uid = t->has_uid;
    d->cap = 1024; d->rec = calloc((size_t)d->cap, (size_t)d->rs);
}
static uint8_t *db_add(DB *d) {
    if (d->n == d->cap) { d->cap *= 2; d->rec = realloc(d->rec, (size_t)d->cap * d->rs); }
    uint8_t *r = d->rec + (size_t)d->n++ * d->rs; memset(r, 0, (size_t)d->rs); return r;
}
static uint8_t *db_rec(const DB *d, int i) { return d->rec + (size_t)i * d->rs; }
static const Field *fld(const DB *d, const char *name) {
    for (int i = 0; i < d->nf; i++) if (!strcmp(d->f[i].name, name)) return &d->f[i];
    return NULL;
}
static const Field *need(const DB *d, const char *name, const char *file) {
    const Field *f = fld(d, name); if (!f) die("%s has no \"%s\" field - not a supported version\n", file, name);
    return f;
}
static void gets_(const DB *d, const uint8_t *r, const char *name, char *out, size_t n) {
    const Field *f = fld(d, name); out[0] = 0; if (!f) return;
    size_t k = 0; while (k < (size_t)f->size && r[f->off + k] && k + 1 < n) { out[k] = (char)r[f->off + k]; k++; }
    out[k] = 0;
    while (k && out[k - 1] == ' ') out[--k] = 0;
}
static uint32_t getn(const DB *d, const uint8_t *r, const char *name) {
    const Field *f = fld(d, name); uint32_t v = 0; if (!f) return 0;
    for (int i = 0; i < f->size; i++) v = v << 8 | r[f->off + i];
    return v;
}
static void sets(const DB *d, uint8_t *r, const char *name, const char *s) {
    const Field *f = fld(d, name); if (!f) return;
    memset(r + f->off, 0, (size_t)f->size);
    size_t k = strlen(s); if (k > (size_t)f->size - 1) k = (size_t)f->size - 1;
    memcpy(r + f->off, s, k);
}
static void setn(const DB *d, uint8_t *r, const char *name, uint32_t v) {
    const Field *f = fld(d, name); if (!f) return;
    uint32_t max = f->size >= 4 ? 0xFFFFFFFFu : (1u << (8 * f->size)) - 1;
    if (v > max) v = max;
    for (int i = f->size - 1; i >= 0; i--) { r[f->off + i] = (uint8_t)v; v >>= 8; }
}
static void db_write(const DB *d, const char *path, uint32_t next_uid) {
    FILE *f = fopen(path, "wb"); if (!f) die("Can't write %s\n", path);
    fwrite(d->hdr, 1, (size_t)d->hdrlen, f);
    uint8_t c[2] = { (uint8_t)(d->n >> 7), (uint8_t)(d->n & 127) }; fwrite(c, 1, 2, f);
    if (d->has_uid) { uint8_t u[4] = { (uint8_t)next_uid, (uint8_t)(next_uid >> 8), (uint8_t)(next_uid >> 16), (uint8_t)(next_uid >> 24) }; fwrite(u, 1, 4, f); }
    fwrite(d->rec, (size_t)d->rs, (size_t)d->n, f);
    if (fclose(f)) die("Can't write %s\n", path);
}

/* ------------------------------------------------------------------ names */
/* lower case, accents dropped (Latin-1), punctuation as spaces, common club prefixes left out */
static void norm(const char *s, char *out, size_t n) {
    static const char *const stop[] = { "fc", "afc", "cf", "sc", "ac", "the", "cd", "sv", "sk", "fk", "nk", "ks", "club",
        "if", "il", "rfc", "kfc", "ksk", "rsc", "sm", "us", "as", "ss", "uc", "vfb", "vfl", "ca", "cs", "sl", "bk", "kv", "k", NULL };
    static const char *lat = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";   /* 0xC0..0xFF */
    char t[256]; size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k < sizeof t - 1; p++) {
        int c = *p;
        if (c >= 0xC0) c = lat[c - 0xC0];
        else if (c >= 0x80) c = ' ';
        c = tolower(c);
        t[k++] = isalnum(c) ? (char)c : ' ';
    }
    t[k] = 0;
    out[0] = 0; size_t o = 0;
    for (char *w = strtok(t, " "); w; w = strtok(NULL, " ")) {
        int skip = 0; for (int i = 0; stop[i]; i++) if (!strcmp(w, stop[i])) skip = 1;
        if (skip) continue;
        size_t L = strlen(w); if (o + L + 2 >= n) break;
        if (o) out[o++] = ' ';
        memcpy(out + o, w, L); o += L; out[o] = 0;
    }
}
/* similarity 0..1 (like Python's difflib: 2 * matching characters / total length) */
static int lcs_blocks(const char *a, int la, const char *b, int lb) {
    if (!la || !lb) return 0;
    int best = 0, ia = 0, ib = 0;
    for (int i = 0; i < la; i++) for (int j = 0; j < lb; j++) {
        int k = 0; while (i + k < la && j + k < lb && a[i + k] == b[j + k]) k++;
        if (k > best) { best = k; ia = i; ib = j; }
    }
    if (!best) return 0;
    return best + lcs_blocks(a, ia, b, ib) + lcs_blocks(a + ia + best, la - ia - best, b + ib + best, lb - ib - best);
}
static double similar(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    return la + lb ? 2.0 * lcs_blocks(a, la, b, lb) / (la + lb) : 0;
}

/* CM2 spellings -> 97/98 */
static const char *const nation_map[][2] = {
    { "Eire", "Republic Of Ireland" }, { "Czech", "Czech Republic" }, { "Czechoslovakia", "Czech Republic" },
    { "Holland", "Netherlands" }, { "Bosnia", "Bosnia-Herzegovina" }, { "Macedonia", "FYR Macedonia" },
    { "Congo", "Republic of Congo" }, { "Zaire", "The Congo" }, { "Trinidad", "Trinidad and Tobago" },
    { "Trinidad & Tobago", "Trinidad and Tobago" }, { "Portuga", "Portugal" }, { "Sotland", "Scotland" },
    { "N.Ireland", "Northern Ireland" },
    /* the Portuguese edition's own slips (unaccented) and a country 1996 didn't have yet */
    { "Romenia", "Romania" }, { "Trinidade e Tobago", "Trinidad and Tobago" }, { "Camaroes", "Cameroon" },
    { "Montenegro", "Yugoslavia" }, { NULL, NULL } };
static const char *nation(const char *s) {
    for (int i = 0; nation_map[i][0]; i++) if (!strcmp(s, nation_map[i][0])) return nation_map[i][1];
    return s;
}
static const char *const team_nation_map[][2] = { { "Czechoslovakia", "Czech Republic" }, { NULL, NULL } };
static const char *team_nation(const char *s) {
    for (int i = 0; team_nation_map[i][0]; i++) if (!strcmp(s, team_nation_map[i][0])) return team_nation_map[i][1];
    return s;
}
static int is_continent(const char *s) {
    static const char *const c[] = { "Europe", "Africa", "Asia", "South America", "CONCACAF", "Oceania", "North America", "EXTINCT", NULL };
    for (int i = 0; c[i]; i++) if (!strcmp(s, c[i])) return 1;
    return 0;
}
/* the leagues CM2 has: their divisions come from CM2 */
/* the European Leagues editions of 96/97 (one country each, full); which ones are merged in */
#define MAXADD 8
static const struct { const char *nation, *league, *div[3]; } homes[] = {
    { "France",   "french",     { "FD1", "FD2", "FNL" } }, { "Italy",    "italian",    { "ISA", "ISB", "ISC" } },
    { "Spain",    "spanish",    { "SP1", "SP2", "SPN" } }, { "Germany",  "german",     { "GD1", "GD2", "GNL" } },
    { "Holland",  "dutch",      { "HD1", "HD2", "HNL" } }, { "Portugal", "portuguese", { "PD1", "PD2", "PNL" } },
    { "Belgium",  "belgian",    { "BD1", "BD2", "BNL" } }, { NULL, NULL, { NULL } } };
static int nadd, add_home[MAXADD];
static int cm2_league(const char *div) {
    static const char *const d[] = { "EPR", "ED1", "ED2", "ED3", "ENL", "SPR", "SD1", "SD2", "SD3", "SNL", "ISA", "ISB", "ISC", "INL", NULL };
    for (int i = 0; d[i]; i++) if (!strcmp(div, d[i])) return 1;
    for (int a = 0; a < nadd; a++) for (int k = 0; k < 3; k++) if (!strcmp(div, homes[add_home[a]].div[k])) return 1;
    return 0;
}
/* an edition's division code as 97/98 has it (the Portuguese edition has its own) */
static const char *home_div(int h, const char *dv) {
    if (!strcmp(homes[h].nation, "Portugal")) {
        if (!strcmp(dv, "D1")) return "PD1";
        if (!strcmp(dv, "D2")) return "PD2";
        if (!strcmp(dv, "AMD")) return "PNL";
    }
    if (!strcmp(homes[h].nation, "Italy") && !strcmp(dv, "INL")) return "ISC";
    return dv;
}
static const char *division(const char *cm2div) { return !strcmp(cm2div, "INL") ? "" : cm2div; }

/* is `s` a nation the engine knows: a national team's name in any language, long or short?
   (the game stops with "Unrecognised Nation" otherwise) */
static int known_nation(const DB *t, const char *s) {
    static const char *const nf[] = { "UK Long Name", "UK Short Name", "Ita Long Name", "Ita Short Name", "Ger Long Name",
        "Ger Short Name", "Spa Long Name", "Spa Short Name", "Fre Long Name", "Fre Short Name", "Por Long Name",
        "Por Short Name", "Dut Long Name", "Dut Short Name", NULL };
    for (int i = 0; i < t->n; i++) {
        char nt[40], v[64]; gets_(t, db_rec(t, i), "Nation", nt, sizeof nt);
        if (!is_continent(nt)) continue;
        for (int k = 0; nf[k]; k++) { gets_(t, db_rec(t, i), nf[k], v, sizeof v); if (v[0] && !strcasecmp(v, s)) return 1; }
    }
    return 0;
}

static char out_dir[1200];
/* CM2 picture -> the name 97/98 looks for (the club is named differently, or CM2 has only a
   second version) */
static const char *const picture_rename[][2] = {
    { "WEST_HA2.PCX", "WEST_HAM.PCX" },     /* West Ham United ("West Ham") */
    { "SPURS.PCX",    "TOTTENHA.PCX" },     /* Tottenham Hotspur (96/97 has it in its main folder) */
    { "BATH.PCX",     "BATH_CIT.PCX" },     /* Bath City */
    { NULL, NULL } };
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"); if (!in) return 0;
    FILE *out = fopen(to, "wb"); if (!out) { fclose(in); return 0; }
    static char buf[1 << 16]; size_t n; int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in); if (fclose(out)) ok = 0;
    return ok;
}
/* the club pictures of one folder; skip_stadpic: leave out STADPIC1-5 (CM2's own backgrounds) */
static void copy_picture_dir(const char *src, const char *dst, int skip_stadpic, int *n, int *renamed, int *bad, int keep) {
    DIR *d = opendir(src); struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcasecmp(e->d_name + L - 4, ".pcx")) continue;
        if (skip_stadpic && !strncasecmp(e->d_name, "STADPIC", 7)) continue;
        char name[260], from[1600], to[1600];
        snprintf(name, sizeof name, "%s", e->d_name);
        for (char *c = name; *c; c++) *c = (char)toupper((unsigned char)*c);
        int ren = -1;
        for (int k = 0; picture_rename[k][0]; k++)
            if (!strcmp(name, picture_rename[k][0])) { ren = k; snprintf(name, sizeof name, "%s", picture_rename[k][1]); break; }
        snprintf(from, sizeof from, "%s/%s", src, e->d_name);
        snprintf(to, sizeof to, "%s/%s", dst, name);
        { char have[1600]; if (keep && find_in(dst, name, have, sizeof have)) continue; }   /* already has one */
        if (logf && ren >= 0) fprintf(logf, "  picture %s -> %s\n", picture_rename[ren][0], name);
        else if (logf && keep) fprintf(logf, "  picture %s\n", name);
        if (ren >= 0) (*renamed)++;
        if (copy_file(from, to)) (*n)++; else { (*bad)++; if (logf) fprintf(logf, "  couldn't copy picture %s\n", from); }
    }
    closedir(d);
}
/* CM2 looks for a club's picture in its main folder; its installer put them in STADPICS. Both are
   taken (96/97 has a couple in the main folder). */
/* other: another CM2 install (the original CM2 for 96/97) - clubs this one has no picture of get
   that one's, if it has one */
static void copy_pictures(const char *cm2, const char *other) {
    char src[1300], dst[1300]; int n = 0, renamed = 0, bad = 0, borrowed = 0;
    snprintf(dst, sizeof dst, "%s/pictures", out_dir); MKDIR(dst);
    if (find_in(cm2, "STADPICS", src, sizeof src) && is_dir(src)) copy_picture_dir(src, dst, 1, &n, &renamed, &bad, 0);
    copy_picture_dir(cm2, dst, 0, &n, &renamed, &bad, 0);
    if (other && other[0]) {
        if (logf) fprintf(logf, "Pictures this one doesn't have, from %s:\n", other);
        if (find_in(other, "STADPICS", src, sizeof src) && is_dir(src)) copy_picture_dir(src, dst, 1, &borrowed, &renamed, &bad, 1);
        copy_picture_dir(other, dst, 0, &borrowed, &renamed, &bad, 1);
    }
    say("Pictures: %d copied (club pictures and the STADPIC backgrounds) (%d renamed to 97/98's names)", n, renamed);
    if (borrowed) say(", %d more from the other Championship Manager 2 folder", borrowed);
    say("%s\n", bad ? " - some couldn't be copied, see convert.log" : "");
}

/* CM2's match sounds (SOUNDS: CROWD, REFLONG, REFSHT, CRCHEER, CRBOO, CRGASP .PCM) -> <game folder>/SOUNDS,
   which every database uses (unless its database.cfg says "sounds = own"). Files already there are kept. */
static void copy_sounds(const char *cm2, const char *game) {
    char src[1300], dst[1300]; int n = 0, kept = 0;
    if (!find_in(cm2, "SOUNDS", src, sizeof src) || !is_dir(src)) { say("Sounds: CM2's SOUNDS folder isn't there\n"); return; }
    if (!find_in(game, "SOUNDS", dst, sizeof dst)) { snprintf(dst, sizeof dst, "%s/SOUNDS", game); MKDIR(dst); }
    DIR *d = opendir(src); struct dirent *e;
    if (!d) return;
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcasecmp(e->d_name + L - 4, ".pcm")) continue;
        char from[1600], to[1600], name[260], have[1600];
        snprintf(name, sizeof name, "%s", e->d_name);
        for (char *c = name; *c; c++) *c = (char)toupper((unsigned char)*c);
        if (find_in(dst, name, have, sizeof have)) { kept++; continue; }
        snprintf(from, sizeof from, "%s/%s", src, e->d_name); snprintf(to, sizeof to, "%s/%s", dst, name);
        if (copy_file(from, to)) n++; else if (logf) fprintf(logf, "  couldn't copy sound %s\n", from);
    }
    closedir(d);
    if (n) say("Sounds: %d copied into the game folder's SOUNDS (CM2's crowd and whistles)\n", n);
    else if (kept) say("Sounds: the game folder already has them\n");
}

/* ------------------------------------------------------------------ which season */
/* the start of a new game, from the program: CM2 sets day, month, weekday and year with
   mov ah,d / mov dl,m / mov ch,w / 4 x mov [..],reg / mov dh,y (1 August 1995 in CM2) */
static int exe_start(const char *dir, int *day, int *month, int *year) {
    DIR *d = opendir(dir); struct dirent *e; int ok = 0;
    if (!d) return 0;
    while (!ok && (e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcasecmp(e->d_name + L - 4, ".exe")) continue;
        char p[1300]; long n; snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        uint8_t *b = load(p, &n); if (!b) continue;
        for (long i = 0; i + 34 < n && !ok; i++) {
            const uint8_t *q = b + i;
            if (q[0] == 0xB4 && q[2] == 0xB2 && q[4] == 0xB5 && q[6] == 0x88 && q[7] == 0x25 && q[12] == 0x88 &&
                q[13] == 0x15 && q[18] == 0x88 && q[19] == 0x25 && q[24] == 0x88 && q[25] == 0x2D && q[30] == 0xB6 &&
                q[1] >= 1 && q[1] <= 31 && q[3] >= 1 && q[3] <= 12 && q[31] >= 80 && q[31] <= 130) {
                *day = q[1]; *month = q[3]; *year = 1900 + q[31]; ok = 1;
            }
        }
        free(b);
    }
    closedir(d);
    return ok;
}
/* the newest season in PLHIST.BIN (e.g. 94 for 1994/95) */
static int hist_last_season(const char *dir) {
    char p[1300]; long hl; if (!find_in(dir, "PLHIST.BIN", p, sizeof p)) return -1;
    uint8_t *h = load(p, &hl); if (!h) return -1;
    long o = 2; int last = -1;
    while (o < hl) {
        for (int k = 0; k < 3 && o < hl; k++) o += 1 + h[o];
        while (o + 1 < hl && h[o] != 0xFF) { if (h[o] > last && h[o] < 130) last = h[o]; o += h[o + 1] + 4; }
        o++;
    }
    free(h);
    return last;
}
int cm2_detect(const char *dir, int *year, int *day, int *month) {
    int d = 1, m = 8, y = 0;
    int ey; int have_exe = exe_start(dir, &d, &m, &ey);
    int last = hist_last_season(dir);
    if (last >= 80) y = 1900 + last + 1;                 /* histories run to the season before */
    else if (have_exe) y = ey;
    if (!y) return 0;
    if (have_exe && ey != y) { d = 1; m = 8; }           /* the program's date is for another season */
    *year = y; *day = d; *month = m;
    return 1;
}
static void season_name(int y, char *name, size_t n) {
    if (y == 1995) snprintf(name, n, "Championship Manager 2");
    else snprintf(name, n, "Championship Manager %02d/%02d", y % 100, (y + 1) % 100);
}
static void date_text(int y, int m, int q, char *out, size_t n) {   /* "Tuesday 1st August 1995" */
    static const char *const wd[] = { "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday" };
    static const char *const mo[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                      "September", "October", "November", "December" };
    int yy = y, mm = m; if (mm < 3) { mm += 12; yy--; }
    int h = (q + 13 * (mm + 1) / 5 + yy % 100 + (yy % 100) / 4 + (yy / 100) / 4 + 5 * (yy / 100)) % 7;
    const char *th = (q % 10 == 1 && q != 11) ? "st" : (q % 10 == 2 && q != 12) ? "nd" : (q % 10 == 3 && q != 13) ? "rd" : "th";
    snprintf(out, n, "%s %d%s %s %d", wd[h], q, th, mo[m - 1], y);
}
void cm2_describe(const char *dir, char *out, size_t n) {
    int y, d, m; char name[64];
    if (!cm2_detect(dir, &y, &d, &m)) { snprintf(out, n, "Championship Manager 2 found."); return; }
    season_name(y, name, sizeof name);
    snprintf(out, n, "%s (%d/%02d) found - goes in databases/%02d%02d.", name, y, (y + 1) % 100, y % 100, (y + 1) % 100);
}

/* ------------------------------------------------------------------ main */
static void die(const char *fmt, ...) {
    char buf[2048]; va_list a; va_start(a, fmt); vsnprintf(buf, sizeof buf, fmt, a); va_end(a);
    say("ERROR: %s", buf);
    if (logf) { fclose(logf); logf = NULL; }
    longjmp(failed, 1);
}

static DB t7, t2, p7, p2a, p2b, m7, m2, T, P, M;   /* 7 = 97/98, 2 = CM2, capitals = output */
/* the French European Leagues edition (optional): its clubs, players (3 files) and managers. t2 is
   then the merged club list; t2e the English edition's own, e2m/f2m their clubs' places in t2
   (-1: not in it - the English edition's French clubs, the French edition's foreign ones) */
static DB t2e, ta[MAXADD], pa[MAXADD][3], ma[MAXADD];
static int *e2m, *a2m[MAXADD];
/* a country's name in English, as 97/98 has it ("Alemanha" -> "Germany"): the national teams' names
   in all seven languages (the Portuguese edition has them in Portuguese) */
static char (*nat_alias)[40], (*nat_uk_name)[40]; static int n_alias = -1;
static const char *nat_uk(const char *s) {
    if (!s[0]) return s;
    if (n_alias < 0) {
        static const char *const nf[] = { "UK Long Name", "UK Short Name", "Ita Long Name", "Ita Short Name", "Ger Long Name",
            "Ger Short Name", "Spa Long Name", "Spa Short Name", "Fre Long Name", "Fre Short Name", "Por Long Name",
            "Por Short Name", "Dut Long Name", "Dut Short Name", NULL };
        n_alias = 0; nat_alias = malloc(40 * 14 * (size_t)t7.n); nat_uk_name = malloc(40 * 14 * (size_t)t7.n);
        for (int i = 0; i < t7.n; i++) {
            char nt[40], uk[40]; gets_(&t7, db_rec(&t7, i), "Nation", nt, sizeof nt);
            if (!is_continent(nt)) continue;
            gets_(&t7, db_rec(&t7, i), "UK Long Name", uk, sizeof uk);
            for (int k = 0; nf[k]; k++) { gets_(&t7, db_rec(&t7, i), nf[k], nat_alias[n_alias], 40);
                if (nat_alias[n_alias][0]) { snprintf(nat_uk_name[n_alias], 40, "%s", uk); n_alias++; } }
        }
    }
    static char (*clubnat)[40]; static int ncn = -1;      /* the countries 97/98's clubs are in, as it writes them */
    if (ncn < 0) {
        ncn = 0; clubnat = malloc(40 * (size_t)t7.n);
        for (int i = 0; i < t7.n; i++) {
            char nt[40]; gets_(&t7, db_rec(&t7, i), "Nation", nt, sizeof nt);
            if (!nt[0] || is_continent(nt)) continue;
            int seen = 0; for (int k = 0; k < ncn && !seen; k++) if (!strcmp(clubnat[k], nt)) seen = 1;
            if (!seen) snprintf(clubnat[ncn++], 40, "%s", nt);
        }
    }
    for (int k = 0; k < ncn; k++) if (!strcmp(s, clubnat[k])) return s;   /* already 97/98's ("Holland") */
    for (int i = 0; i < n_alias; i++) if (!strcasecmp(s, nat_alias[i])) return nat_uk_name[i];
    return s;
}

/* CM2 club index -> output club name */
static char (*club_out)[40];
/* resolve a club as CM2 writes it in player/manager records (long, short or shortened name) */
static int resolve_in(const DB *t, const char *c, const char *nat);
static int resolve_club_in(const char *c, const char *nat) { return resolve_in(&t2, c, nat); }
static int resolve_club(const char *c) { return resolve_club_in(c, ""); }
/* `nat`: the player's nationality, to choose between clubs a short name could be ("Omonia": Omonia
   Nicosia or Omonia Aradippou) - a club of his own country, the bigger one if there are several */
/* English names CM2's players sometimes have for a club CM2 lists under its own name */
static const char *const club_alias[][2] = { { "Seville", "Sevilla" }, { NULL, NULL } };
static int resolve_in(const DB *tt, const char *c, const char *nat) {
    for (int i = 0; club_alias[i][0]; i++) if (!strcasecmp(c, club_alias[i][0])) { c = club_alias[i][1]; break; }
    if (!c[0]) return -1;
    char ln[64], sn[64];
    for (int j = 0; j < tt->n; j++) {
        gets_(tt, db_rec(tt, j), "Long Name", ln, sizeof ln); gets_(tt, db_rec(tt, j), "Short Name", sn, sizeof sn);
        if (!strcasecmp(c, ln) || (sn[0] && !strcasecmp(c, sn))) return j;
    }
    /* "Huddersfield T", "Queen of", "Dinamo Tb": each word starts the club's words in order */
    char tok[8][40]; int nt = 0; char tmp[64]; snprintf(tmp, sizeof tmp, "%s", c);
    for (char *w = strtok(tmp, " .-"); w && nt < 8; w = strtok(NULL, " .-")) snprintf(tok[nt++], 40, "%s", w);
    int found = -1, nfound = 0, league = -1, nleague = 0, home = -1, nhome = 0; uint32_t home_rep = 0;
    for (int j = 0; j < tt->n; j++) {
        for (int pass = 0; pass < 2; pass++) {
            gets_(tt, db_rec(tt, j), pass ? "Short Name" : "Long Name", tmp, sizeof tmp);
            if (!tmp[0]) continue;
            char wd[12][40]; int nw = 0;
            for (char *w = strtok(tmp, " .-"); w && nw < 12; w = strtok(NULL, " .-")) snprintf(wd[nw++], 40, "%s", w);
            if (nw < nt) continue;
            int ok = 1; for (int i = 0; i < nt && ok; i++) if (strncasecmp(wd[i], tok[i], strlen(tok[i]))) ok = 0;
            if (!ok) continue;
            found = j; nfound++;
            char dv[16]; gets_(tt, db_rec(tt, j), "Division", dv, sizeof dv);
            if (dv[0]) { league = j; nleague++; }
            char cn[40]; gets_(tt, db_rec(tt, j), "Nation", cn, sizeof cn);
            if (nat[0] && (!strcmp(cn, nat) || !strcmp(team_nation(cn), nat))) {
                uint32_t rep = getn(tt, db_rec(tt, j), "Standard");
                if (!nhome || rep > home_rep) { home = j; home_rep = rep; }
                nhome++;
            }
            break;
        }
    }
    if (nfound == 1) return found;
    if (nleague == 1) return league;
    if (nhome) return home;
    return -2;
}

/* the club a player or manager of database `t` is at, as a place in the merged list t2: -1 none (or
   one whose people come from the other edition), -2 couldn't tell which */
static int club_of(const DB *t, const int *map, const char *c, const char *nat) {
    int k = resolve_in(t, c, nat);
    return k >= 0 ? map[k] : k;
}
static void key_of(const DB *d, const uint8_t *r, char *out, size_t n) {   /* name and birth date */
    char a[40], b[40], c[24]; gets_(d, r, "First Name", a, sizeof a); gets_(d, r, "Second Name", b, sizeof b);
    { size_t k = strlen(b); while (k && b[k - 1] == '*') b[--k] = 0; }
    gets_(d, r, "Birth Date", c, sizeof c); snprintf(out, n, "%s|%s|%s", a, b, c);
    for (char *q = out; *q; q++) *q = (char)tolower((unsigned char)*q);
}

/* which country a European Leagues edition has in full ("France"), or NULL */
const char *cm2_edition_country(const char *dir) {
    static DB t, pl; char q[1300]; const char *res = NULL;
    cm2_log_fn keep = logfn; logfn = NULL;                 /* quietly */
    if (setjmp(failed)) { logfn = keep; return NULL; }
    if (!find_in(dir, "TMDATA.DB1", q, sizeof q)) { logfn = keep; return NULL; }
    db_read(&t, q);
    int cnt[16] = { 0 };
    for (int part = 1; part <= 3; part++) {
        char name[16]; snprintf(name, sizeof name, "PLDATA%d.DB1", part);
        if (!find_in(dir, name, q, sizeof q)) continue;
        db_read(&pl, q);
        for (int i = 0; i < pl.n; i++) {
            char c[64]; gets_(&pl, db_rec(&pl, i), "Club", c, sizeof c);
            int k = resolve_in(&t, c, ""); if (k < 0) continue;
            char nt[40]; gets_(&t, db_rec(&t, k), "Nation", nt, sizeof nt);
            for (int h = 0; homes[h].nation; h++) if (!strcmp(nt, homes[h].nation)) cnt[h]++;
        }
        free(pl.rec); free(pl.hdr);
    }
    int best = 0; for (int h = 1; homes[h].nation; h++) if (cnt[h] > cnt[best]) best = h;
    if (cnt[best] >= 300) res = homes[best].nation;
    free(t.rec); free(t.hdr); logfn = keep;
    return res;
}

int cm2_import(const char *cm2, const char *game, const char *pictures_from, const char *const *addons, int naddons, cm2_log_fn log) {
    logfn = log; logf = NULL;
    memset(&t7, 0, sizeof t7); memset(&t2, 0, sizeof t2); memset(&p7, 0, sizeof p7); memset(&p2a, 0, sizeof p2a);
    memset(&p2b, 0, sizeof p2b); memset(&m7, 0, sizeof m7); memset(&m2, 0, sizeof m2);
    memset(&T, 0, sizeof T); memset(&P, 0, sizeof P); memset(&M, 0, sizeof M);
    memset(&t2e, 0, sizeof t2e); memset(ta, 0, sizeof ta); memset(pa, 0, sizeof pa); memset(ma, 0, sizeof ma);
    nadd = 0; n_alias = -1;   /* (nat_uk's lists are made again from this import's 97/98 files) */
    if (setjmp(failed)) return -1;
    if (!is_dir(cm2)) die("%s is not a folder\n", cm2);
    char p[1300], src7[1200];
    /* 97/98 files: databases/9798, else the game folder */
    snprintf(src7, sizeof src7, "%s/databases/9798", game);
    if (!find_in(src7, "PLAYERS.DB1", p, sizeof p)) snprintf(src7, sizeof src7, "%s", game);
    if (!find_in(src7, "PLAYERS.DB1", p, sizeof p))
        die("Couldn't find the CM 97/98 files (PLAYERS.DB1) in %s - install CM 97/98 first.\n", game);
    char ddir[1200]; snprintf(ddir, sizeof ddir, "%s/databases", game); MKDIR(ddir);
    int sy, sd, sm; char sname[64], sdate[64];
    if (!cm2_detect(cm2, &sy, &sd, &sm)) sy = 1995;
    sd = 16; sm = 7;   /* not CM2's own 1 August: the 97/98 engine sets the season up in late July, so a
                          career starting on 1 August stops on its first day ("Football Writers") */
    season_name(sy, sname, sizeof sname); date_text(sy, sm, sd, sdate, sizeof sdate);
    snprintf(out_dir, sizeof out_dir, "%s/%02d%02d", ddir, sy % 100, (sy + 1) % 100); MKDIR(out_dir);
    if (!is_dir(out_dir)) die("Can't create %s\n", out_dir);
    snprintf(p, sizeof p, "%s/convert.log", out_dir); logf = fopen(p, "w");
    say("%s, new games from %s\nFrom:         %s\nCM 97/98:     %s\nWriting to:   %s\n\n", sname, sdate, cm2, src7, out_dir);

#define OPEN(db, dir, name) do { char q_[1300]; if (!find_in(dir, name, q_, sizeof q_)) die("%s is missing from %s\n", name, dir); db_read(&db, q_); } while (0)
    OPEN(t7, src7, "TMDATA.DB1"); OPEN(p7, src7, "PLAYERS.DB1"); OPEN(m7, src7, "MGDATA.DB1");
    OPEN(t2, cm2, "TMDATA.DB1"); OPEN(p2a, cm2, "PLDATA1.DB1"); OPEN(m2, cm2, "MGDATA.DB1");
    { char q[1300]; if (find_in(cm2, "PLDATA2.DB1", q, sizeof q)) db_read(&p2b, q); }
    need(&t2, "Long Name", "CM2 TMDATA.DB1"); need(&t2, "Division", "CM2 TMDATA.DB1");
    need(&p2a, "Club", "CM2 PLDATA1.DB1"); need(&p2a, "Ability", "CM2 PLDATA1.DB1");
    need(&m2, "M/Club", "CM2 MGDATA.DB1");
    need(&t7, "UK Long Name", "97/98 TMDATA.DB1"); need(&p7, "Current Club", "97/98 PLAYERS.DB1"); need(&m7, "Motivating Abil", "97/98 MGDATA.DB1");
    say("CM2: %d clubs, %d players, %d managers\n", t2.n, p2a.n + p2b.n, m2.n);

    /* ---- the European Leagues editions: each has one country in full, whose clubs (and players
       and managers, below) take the place of the English edition's ---- */
    t2e = t2;
    e2m = malloc(sizeof(int) * (size_t)t2e.n);
    for (int x = 0; x < naddons && x < MAXADD; x++) {
        const char *dir = addons[x]; if (!dir || !dir[0]) continue;
        if (!is_dir(dir)) die("%s is not a folder\n", dir);
        int a = nadd;
        OPEN(ta[a], dir, "TMDATA.DB1"); OPEN(pa[a][0], dir, "PLDATA1.DB1"); OPEN(ma[a], dir, "MGDATA.DB1");
        { char q[1300]; if (find_in(dir, "PLDATA2.DB1", q, sizeof q)) db_read(&pa[a][1], q);
                        if (find_in(dir, "PLDATA3.DB1", q, sizeof q)) db_read(&pa[a][2], q); }
        /* its country: the one most of its players play in */
        int cnt[16] = { 0 };
        for (int part = 0; part < 3; part++) for (int i = 0; i < pa[a][part].n; i++) {
            char c[64]; gets_(&pa[a][part], db_rec(&pa[a][part], i), "Club", c, sizeof c);
            int k = resolve_in(&ta[a], c, ""); if (k < 0) continue;
            char nt[40]; gets_(&ta[a], db_rec(&ta[a], k), "Nation", nt, sizeof nt);
            for (int h = 0; homes[h].nation; h++) if (!strcmp(nat_uk(nt), homes[h].nation)) cnt[h]++;
        }
        int best = 0; for (int h = 1; homes[h].nation; h++) if (cnt[h] > cnt[best]) best = h;
        if (cnt[best] < 300) die("%s doesn't look like one of CM2's 96/97 league editions (no country in full)\n", dir);
        int dup = 0; for (int b = 0; b < nadd; b++) if (add_home[b] == best) dup = 1;
        if (dup) { say("Left out %s: a second %s edition\n", dir, homes[best].nation); continue; }
        add_home[a] = best; nadd++;
        say("League edition, %s: %d players at its clubs\n", homes[best].nation, cnt[best]);
    }
    if (nadd) {
        db_template(&t2, &t2e);
        for (int j = 0; j < t2e.n; j++) {
            char nt[40]; gets_(&t2e, db_rec(&t2e, j), "Nation", nt, sizeof nt); int mine = 0;
            for (int a = 0; a < nadd; a++) if (!strcmp(nt, homes[add_home[a]].nation)) mine = 1;
            if (mine) { e2m[j] = -1; continue; }
            memcpy(db_add(&t2), db_rec(&t2e, j), (size_t)t2.rs); e2m[j] = t2.n - 1;
        }
        for (int a = 0; a < nadd; a++) {
            int h = add_home[a], nc = 0;
            a2m[a] = malloc(sizeof(int) * (size_t)ta[a].n);
            for (int j = 0; j < ta[a].n; j++) {
                char nt[40]; gets_(&ta[a], db_rec(&ta[a], j), "Nation", nt, sizeof nt);
                a2m[a][j] = -1; if (strcmp(nat_uk(nt), homes[h].nation)) continue;
                uint8_t *o = db_add(&t2), *r = db_rec(&ta[a], j); a2m[a][j] = t2.n - 1; nc++;
                for (int k = 0; k < t2.nf; k++) {              /* the fields both have */
                    const Field *from = fld(&ta[a], t2.f[k].name); if (!from) continue;
                    char v[64];
                    if (t2.f[k].type) { gets_(&ta[a], r, from->name, v, sizeof v); sets(&t2, o, t2.f[k].name, v); }
                    else setn(&t2, o, t2.f[k].name, getn(&ta[a], r, from->name));
                }
                char v[64]; sets(&t2, o, "Nation", homes[h].nation);
                gets_(&t2, o, "Division", v, sizeof v); sets(&t2, o, "Division", home_div(h, v));
                gets_(&t2, o, "Last Div", v, sizeof v); sets(&t2, o, "Last Div", home_div(h, v));
            }
            say("  %s: %d clubs, replacing the English edition's\n", homes[h].nation, nc);
        }
    } else for (int j = 0; j < t2e.n; j++) e2m[j] = j;

    /* ---- clubs ---- */
    db_template(&T, &t7);
    for (int i = 0; i < t7.n; i++) memcpy(db_add(&T), db_rec(&t7, i), (size_t)T.rs);
    club_out = calloc((size_t)t2.n, sizeof *club_out);
    int *match = malloc(sizeof(int) * (size_t)t2.n); char *used = calloc((size_t)t7.n, 1);
    char (*n7)[64] = malloc(64 * (size_t)t7.n), (*n7s)[64] = malloc(64 * (size_t)t7.n), (*nat7)[40] = malloc(40 * (size_t)t7.n);
    for (int i = 0; i < t7.n; i++) {
        char s[64]; gets_(&t7, db_rec(&t7, i), "UK Long Name", s, sizeof s); norm(s, n7[i], 64);
        gets_(&t7, db_rec(&t7, i), "UK Short Name", s, sizeof s); norm(s, n7s[i], 64);
        gets_(&t7, db_rec(&t7, i), "Nation", nat7[i], 40);
    }
    int matched = 0, renamed = 0, added = 0, skipped = 0;
    /* two rounds, so a close spelling can't take a club that has an exact match ("Olympique Alès"
       is close to "Olympique Lyonnais", which the real Lyon needs) */
    for (int j = 0; j < t2.n; j++) match[j] = -1;
    for (int round = 0; round < 2; round++)
    for (int j = 0; j < t2.n; j++) {
        if (match[j] >= 0) continue;
        uint8_t *r = db_rec(&t2, j); char ln[64], sn[64], nt[40], a[64];
        gets_(&t2, r, "Long Name", ln, sizeof ln); gets_(&t2, r, "Short Name", sn, sizeof sn); gets_(&t2, r, "Nation", nt, sizeof nt);
        const char *nt7 = team_nation(nt);
        int m = -1;
        if (round == 0) {
            for (int i = 0; i < t7.n && m < 0; i++) { char s[64]; gets_(&t7, db_rec(&t7, i), "UK Long Name", s, sizeof s); if (!strcmp(s, ln) && !used[i]) m = i; }
            for (int pass = 0; pass < 2 && m < 0; pass++) {          /* same name once spelled alike: same country first */
                norm(pass ? sn : ln, a, sizeof a); if (!a[0]) continue;
                int any = -1;
                for (int i = 0; i < t7.n; i++) {
                    if (used[i] || (strcmp(n7[i], a) && strcmp(n7s[i], a))) continue;
                    if (!strcmp(nat7[i], nt) || !strcmp(nat7[i], nt7)) { m = i; break; }
                    if (any < 0) any = i;
                }
                if (m < 0) m = any;
            }
        } else {                                                  /* close spelling, same country */
            norm(ln, a, sizeof a); double best = 0.75;
            for (int i = 0; i < t7.n; i++) {
                if (used[i] || (strcmp(nat7[i], nt) && strcmp(nat7[i], nt7))) continue;
                double s = similar(a, n7[i]); if (s >= best) { best = s; m = i; }
            }
        }
        match[j] = m;
        if (m >= 0) {
            used[m] = 1; matched++;
            gets_(&t7, db_rec(&t7, m), "UK Long Name", club_out[j], 40);
            if (strcmp(club_out[j], ln)) { renamed++; if (logf) fprintf(logf, "  club %-28s = %s\n", ln, club_out[j]); }
        } else snprintf(club_out[j], 40, "%s", ln);
    }
    for (int j = 0; j < t2.n; j++) {
        uint8_t *r = db_rec(&t2, j); char ln[64], nt[40], dv[16], s[64];
        gets_(&t2, r, "Long Name", ln, sizeof ln); gets_(&t2, r, "Nation", nt, sizeof nt); gets_(&t2, r, "Division", dv, sizeof dv);
        if (!strcmp(ln, "Free Transfer")) continue;
        uint8_t *o;
        if (match[j] >= 0) {
            o = db_rec(&T, match[j]);
            if (is_continent(nt)) continue;                       /* national teams: as in 97/98 */
            char dv7[16]; gets_(&T, o, "Division", dv7, sizeof dv7);
            if (dv[0] || cm2_league(dv7)) {                       /* CM2's leagues: CM2's divisions */
                sets(&T, o, "Division", division(dv));
                gets_(&t2, r, "Last Div", s, sizeof s); sets(&T, o, "Last Division", division(s));
                setn(&T, o, "Last Position", getn(&t2, r, "Last Pos"));
            }
        } else {
            if (is_continent(nt)) { skipped++; if (logf) fprintf(logf, "  skipped %s (%s)\n", ln, nt); continue; }
            o = db_add(&T); added++;
            static const char *const lang[] = { "UK", "Ita", "Ger", "Spa", "Fre", "Por", "Dut" };
            char sn[64]; gets_(&t2, r, "Short Name", sn, sizeof sn);
            for (int k = 0; k < 7; k++) {
                char f[40]; snprintf(f, sizeof f, "%s Long Name", lang[k]); sets(&T, o, f, ln);
                snprintf(f, sizeof f, "%s Short Name", lang[k]); sets(&T, o, f, sn);
            }
            sets(&T, o, "Nation", team_nation(nt));
            gets_(&t2, r, "Region", s, sizeof s); sets(&T, o, "Region", s);
            setn(&T, o, "X Co-ord", getn(&t2, r, "X Co-ord")); setn(&T, o, "Y Co-ord", getn(&t2, r, "Y Co-ord"));
            setn(&T, o, "EEC Member", getn(&t2, r, "EEC Member")); setn(&T, o, "TCoef 89-93", getn(&t2, r, "TCoef 89-93"));
            setn(&T, o, "Developed", getn(&t2, r, "Developed"));   /* (96/97 only) */
            sets(&T, o, "Division", division(dv));
            gets_(&t2, r, "Last Div", s, sizeof s); sets(&T, o, "Last Division", division(s));
            setn(&T, o, "Last Position", getn(&t2, r, "Last Pos"));
            setn(&T, o, "Essential", dv[0] ? 1 : 0);
            if (logf) fprintf(logf, "  added club %s (%s) %s\n", ln, nt, dv);
        }
        /* CM2's details where it has them */
#define COPYS(from, to) do { gets_(&t2, r, from, s, sizeof s); if (s[0]) sets(&T, o, to, s); } while (0)
#define COPYN(from, to) do { uint32_t v_ = getn(&t2, r, from); if (v_) setn(&T, o, to, v_); } while (0)
        COPYS("City", "City or Area"); COPYS("Stadium", "Stadium Name");
        COPYN("Capacity", "Capacity"); COPYN("Seating", "Seating"); COPYN("Following", "Following");
        COPYN("Standard", "Reputation"); COPYN("Blend", "Blend");
        COPYS("Formation", "Formation"); COPYS("Style", "Style");
        COPYS("Home Col 1", "Home Text Col"); COPYS("Home Col 2", "Home Back Col");
        COPYS("Away Col 1", "Away Text Col"); COPYS("Away Col 2", "Away Back Col");
        COPYN("Cash", "Cash"); COPYN("League Std", "League Std");
    }
    /* 97/98 clubs in CM2's leagues that CM2 doesn't have: out of the league */
    int left = 0;
    for (int i = 0; i < t7.n; i++) {
        if (used[i]) continue;
        uint8_t *o = db_rec(&T, i); char dv[16]; gets_(&T, o, "Division", dv, sizeof dv);
        if (!cm2_league(dv)) continue;
        sets(&T, o, "Division", ""); sets(&T, o, "Last Division", ""); setn(&T, o, "Last Position", 0); left++;
    }
    uint32_t uid = 0;
    for (int i = 0; i < T.n; i++) { uint32_t u = getn(&T, db_rec(&T, i), "Unique ID"); if (u > uid) uid = u; }
    for (int i = t7.n; i < T.n; i++) setn(&T, db_rec(&T, i), "Unique ID", ++uid);
    say("Clubs: %d matched to 97/98 (%d spelled differently), %d added, %d national/extinct teams skipped,\n"
        "       %d 97/98 clubs taken out of the English/Scottish/Italian leagues; %d clubs in all\n",
        matched, renamed, added, skipped, left, T.n);

    /* ---- players ---- */
    db_template(&P, &p7);
    int unres = 0, np = 0, badnat = 0, replaced = 0;
    /* the editions' players at their own country's clubs (the first edition to have a player wins);
       English-edition players they replace */
    int nfp = 0; char (*fkey)[96] = NULL; uint32_t *frep = NULL; const DB **fsrc = NULL; int *fidx = NULL, *fclub = NULL;
    if (nadd) {
        int tot = 0; for (int a = 0; a < nadd; a++) tot += pa[a][0].n + pa[a][1].n + pa[a][2].n;
        fkey = malloc(96 * (size_t)tot); frep = calloc((size_t)tot, sizeof *frep);
        fsrc = malloc(sizeof *fsrc * (size_t)tot); fidx = malloc(sizeof(int) * (size_t)tot); fclub = malloc(sizeof(int) * (size_t)tot);
        for (int a = 0; a < nadd; a++) for (int part = 0; part < 3; part++) for (int i = 0; i < pa[a][part].n; i++) {
            const DB *d = &pa[a][part]; uint8_t *r = db_rec(d, i); char c[64], pn[64];
            gets_(d, r, "Club", c, sizeof c); gets_(d, r, "Nation", pn, sizeof pn);
            int k = club_of(&ta[a], a2m[a], c, pn);
            if (k < 0) continue;                                   /* not at one of its own country's clubs */
            char key[96]; key_of(d, r, key, sizeof key); int dup = 0;
            for (int q = 0; q < nfp && !dup; q++) if (!strcmp(key, fkey[q])) dup = 1;
            if (dup) continue;
            fsrc[nfp] = d; fidx[nfp] = i; fclub[nfp] = k; snprintf(fkey[nfp], 96, "%s", key); nfp++;
        }
    }
    for (int part = 0; part < 2 + 3; part++) {
        const DB *s = part == 0 ? &p2a : part == 1 ? &p2b : NULL;
        int nrec = s ? s->n : part == 2 ? nfp : 0;                  /* (part 2: all the editions' players) */
        for (int i = 0; i < nrec; i++) {
            const DB *sd = s ? s : fsrc[i];
            uint8_t *r = s ? db_rec(s, i) : db_rec(sd, fidx[i]); char v[64];
            char pn[64]; gets_(sd, r, "Nation", pn, sizeof pn);
            if (!s) { char t[64]; snprintf(t, sizeof t, "%s", nat_uk(pn)); snprintf(pn, sizeof pn, "%s", t); }   /* (nat_uk may hand pn back) */         /* (the Portuguese edition's are in Portuguese) */
            int c; uint32_t rep = 0;
            if (s) {
                gets_(sd, r, "Club", v, sizeof v);
                c = club_of(&t2e, e2m, v, nation(pn));
                if (nadd) {
                    if (c == -1 && v[0] && resolve_in(&t2e, v, nation(pn)) >= 0) { replaced++; continue; }   /* an edition has his club */
                    char key[96]; key_of(sd, r, key, sizeof key); int dup = -1;
                    for (int k = 0; k < nfp && dup < 0; k++) if (!strcmp(key, fkey[k])) dup = k;
                    if (dup >= 0) { frep[dup] = getn(sd, r, "Reputation"); replaced++; continue; }   /* an edition has him */
                }
            } else { c = fclub[i]; rep = frep[i]; }
            uint8_t *o = db_add(&P);
            setn(&P, o, "Unique ID", (uint32_t)++np);
            gets_(sd, r, "First Name", v, sizeof v); sets(&P, o, "First Name", v);
            gets_(sd, r, "Second Name", v, sizeof v);
            { size_t k = strlen(v); while (k && v[k - 1] == '*') v[--k] = 0; }    /* CM2 marks some names */
            sets(&P, o, "Second Name", v);
            { char nat[64]; snprintf(nat, sizeof nat, "%s", nation(pn));
              if (!known_nation(&T, nat)) {                       /* a slip in CM2's data: use the club's country */
                  char cn[40] = "England";
                  if (c >= 0) { char tn[40]; gets_(&t2, db_rec(&t2, c), "Nation", tn, sizeof tn); snprintf(cn, sizeof cn, "%s", team_nation(tn)); }
                  if (!known_nation(&T, cn)) snprintf(cn, sizeof cn, "England");
                  if (logf) { char fn[40], sn[40]; gets_(sd, r, "First Name", fn, sizeof fn); gets_(sd, r, "Second Name", sn, sizeof sn);
                              fprintf(logf, "  player %s %s: unknown nation \"%s\" - made %s\n", fn, sn, pn, cn); }
                  snprintf(nat, sizeof nat, "%s", cn); badnat++;
              }
              sets(&P, o, "Nation", nat); }
            setn(&P, o, "Caps", getn(sd, r, "Caps")); setn(&P, o, "Goals", getn(sd, r, "Goals"));
            gets_(sd, r, "Club", v, sizeof v);
            if (c >= 0) sets(&P, o, "Current Club", club_out[c]);
            else if (c == -2) { sets(&P, o, "Current Club", "Free Transfer"); unres++; if (logf) fprintf(logf, "  player's club not found: \"%s\" - free transfer (CM2 has no such club, or several and none in his country)\n", v); }
            gets_(sd, r, "Birth Date", v, sizeof v); sets(&P, o, "Date of Birth", v);
            setn(&P, o, "Age", getn(sd, r, "Age"));
            static const char *const same[] = { "Goalkeeper", "Sweeper", "Defence", "Anchor", "Midfield", "Support", "Attack",
                "Right Sided", "Left Sided", "Central", "Ability", "Potential", "Aggression", "Big-Occasion", "Character",
                "Consistency", "Creativity", "Determination", "Dirtyness", "Dribbling", "Flair", "Heading", "Influence",
                "Inj Prone", "Marking", "Off the ball", "Pace", "Passing", "Positioning", "Set Pieces", "Shooting",
                "Stamina", "Strength", "Tackling", "Technique", NULL };
            for (int k = 0; same[k]; k++) setn(&P, o, same[k], getn(sd, r, same[k]));
            if (rep) setn(&P, o, "Reputation", rep);                   /* the English edition's, for the same player */
            else if (fld(sd, "Reputation")) setn(&P, o, "Reputation", getn(sd, r, "Reputation"));   /* 96/97 has it */
            else setn(&P, o, "Reputation", getn(sd, r, "Ability") * 3 / 4);   /* CM2: about 3/4 of ability, as in 97/98 */
        }
    }
    if (nadd) say("Players: %d from the league editions, %d of the English edition's replaced by them\n", nfp, replaced);
    say("Players: %d (%d with a club that couldn't be found - free transfers; %d with an unknown nation; see convert.log)\n", P.n, unres, badnat);

    /* ---- managers ---- */
    db_template(&M, &m7);
    char natjob[32][40]; int nnat = 0;
    for (int src = 0; src <= nadd; src++) {
        const DB *md = src ? &ma[src - 1] : &m2;
        for (int i = 0; i < md->n; i++) {
            uint8_t *r = db_rec(md, i); char v[64], club[64];
            gets_(md, r, "M/Club", club, sizeof club);
            int c = src ? club_of(&ta[src - 1], a2m[src - 1], club, "") : club_of(&t2e, e2m, club, "");
            if (src && c < 0) continue;                        /* an edition: its own country's clubs only */
            if (!src && nadd && c == -1 && club[0] && resolve_in(&t2e, club, "") >= 0) continue;   /* replaced */
            if (nadd) {                                        /* the same man in an edition (a later one, or already in) */
                char a[40], b[40], fa[40], fb[40]; int dup = 0;
                gets_(md, r, "First Name", a, sizeof a); gets_(md, r, "Second Name", b, sizeof b);
                for (int k = 0; k < M.n && !dup; k++) {
                    gets_(&M, db_rec(&M, k), "First Name", fa, sizeof fa); gets_(&M, db_rec(&M, k), "Second Name", fb, sizeof fb);
                    if (!strcasecmp(a, fa) && !strcasecmp(b, fb)) dup = 1;
                }
                for (int e = 0; e < nadd && !dup && !src; e++) for (int k = 0; k < ma[e].n && !dup; k++) {
                    char fc[64]; gets_(&ma[e], db_rec(&ma[e], k), "M/Club", fc, sizeof fc);
                    if (club_of(&ta[e], a2m[e], fc, "") < 0) continue;
                    gets_(&ma[e], db_rec(&ma[e], k), "First Name", fa, sizeof fa); gets_(&ma[e], db_rec(&ma[e], k), "Second Name", fb, sizeof fb);
                    if (!strcasecmp(a, fa) && !strcasecmp(b, fb)) dup = 1;
                }
                if (dup) continue;
            }
            uint8_t *o = db_add(&M);
            gets_(md, r, "First Name", v, sizeof v); sets(&M, o, "First Name", v);
            gets_(md, r, "Second Name", v, sizeof v);
            { size_t k = strlen(v); while (k && v[k - 1] == '*') v[--k] = 0; }
            sets(&M, o, "Second Name", v);
            gets_(md, r, "Nation", v, sizeof v); sets(&M, o, "Nation", nation(nat_uk(v)));
            uint32_t y = fld(md, "Yrs In Game") ? getn(md, r, "Yrs In Game") : getn(md, r, "Seasons");
            setn(&M, o, "Yrs In Game", y < 100 ? y : 0);
            gets_(md, r, "Favoured", v, sizeof v); { int k = resolve_club(v); if (k >= 0) sets(&M, o, "Favoured Club", club_out[k]); }
            uint32_t ab = getn(md, r, "Ability");
            setn(&M, o, "Motivating Abil", ab); setn(&M, o, "Judgement", ab);
            setn(&M, o, "Reputation", getn(md, r, "Reputation"));
            gets_(md, r, "Formation", v, sizeof v); sets(&M, o, "Formation", v);
            gets_(md, r, "Style", v, sizeof v); sets(&M, o, "Style", v);
            if (c >= 0) sets(&M, o, "Current Club", club_out[c]);
            else if (c == -2 && logf) fprintf(logf, "  manager's club not found: \"%s\"\n", club);
            gets_(md, r, "Ap/Club", v, sizeof v); sets(&M, o, "Date Joined", v);
            gets_(md, r, "M/Int", v, sizeof v);
            if (v[0]) { sets(&M, o, "National Job", nation(v)); if (nnat < 32) snprintf(natjob[nnat++], 40, "%s", nation(v)); }
            gets_(md, r, "Ap/Int", v, sizeof v); sets(&M, o, "Date Started", v);
            setn(&M, o, "Playing ?", getn(md, r, "Playing"));
        }
    }
    int ncm = M.n;                                       /* managers from CM2 (and the French edition) */
    int kept = 0, freed = 0;
    for (int i = 0; i < m7.n; i++) {
        uint8_t *r = db_rec(&m7, i); char fn[40], sn[40], club[40], job[40], a[40], b[40];
        gets_(&m7, r, "First Name", fn, sizeof fn); gets_(&m7, r, "Second Name", sn, sizeof sn);
        int dup = 0;
        for (int k = 0; k < ncm && !dup; k++) {
            gets_(&M, db_rec(&M, k), "First Name", a, sizeof a); gets_(&M, db_rec(&M, k), "Second Name", b, sizeof b);
            if (!strcasecmp(a, fn) && !strcasecmp(b, sn)) dup = 1;
        }
        if (dup) continue;
        uint8_t *o = db_add(&M); memcpy(o, r, (size_t)M.rs); kept++;
        gets_(&M, o, "Current Club", club, sizeof club);
        if (club[0]) {                                   /* a club whose 95/96 manager comes from CM2? */
            int clash = 0;
            for (int k = 0; k < T.n && !clash; k++) {
                char tn[40], dv[16]; gets_(&T, db_rec(&T, k), "UK Long Name", tn, sizeof tn);
                if (strcmp(tn, club)) continue;
                gets_(&T, db_rec(&T, k), "Nation", a, sizeof a); gets_(&T, db_rec(&T, k), "Division", dv, sizeof dv);
                if (cm2_league(dv) || !strcmp(a, "England") || !strcmp(a, "Scotland") || !strcmp(a, "Italy") || !strcmp(a, "Wales") ||
                    0) clash = 1;
                for (int e = 0; e < nadd; e++) if (!strcmp(a, homes[add_home[e]].nation)) clash = 1;
            }
            for (int k = 0; k < ncm && !clash; k++) { gets_(&M, db_rec(&M, k), "Current Club", a, sizeof a); if (!strcmp(a, club)) clash = 1; }
            if (clash) { sets(&M, o, "Current Club", ""); sets(&M, o, "Date Joined", ""); freed++; }
        }
        gets_(&M, o, "National Job", job, sizeof job);
        for (int k = 0; k < nnat; k++) if (!strcmp(job, natjob[k])) { sets(&M, o, "National Job", ""); sets(&M, o, "Date Started", ""); }
    }
    /* the engine holds about 1000 managers (with the human ones a new game adds; 1005 crashes it on
       starting a new game): past MAX_MANAGERS, 97/98's unattached ones go, the least known first */
#define MAX_MANAGERS 990
    int dropped = 0;
    while (M.n > MAX_MANAGERS) {
        int worst = -1; uint32_t wrep = 0xFFFFFFFFu;
        for (int i = ncm; i < M.n; i++) {
            char club[40], job[40]; gets_(&M, db_rec(&M, i), "Current Club", club, sizeof club); gets_(&M, db_rec(&M, i), "National Job", job, sizeof job);
            if (club[0] || job[0]) continue;
            uint32_t rp = getn(&M, db_rec(&M, i), "Reputation"); if (rp < wrep) { wrep = rp; worst = i; }
        }
        if (worst < 0) break;
        memmove(db_rec(&M, worst), db_rec(&M, worst + 1), (size_t)(M.n - worst - 1) * M.rs); M.n--; kept--; dropped++;
    }
    if (dropped) say("Managers: %d unattached 97/98 managers left out - the game holds about 1000\n", dropped);
    if (M.n > MAX_MANAGERS) say("Note: %d managers - more than the game is known to cope with (%d)\n", M.n, MAX_MANAGERS);
    for (int i = 0; i < M.n; i++) setn(&M, db_rec(&M, i), "Unique ID", (uint32_t)(i + 1));
    say("Managers: %d from CM2%s, %d more from 97/98 (%d of those made unattached)\n", ncm, nadd ? " and the editions" : "", kept, freed);

    /* ---- player histories ---- */
    long hl; char hp[1300];
    if (!find_in(cm2, "PLHIST.BIN", hp, sizeof hp)) die("PLHIST.BIN is missing from %s\n", cm2);
    uint8_t *h = load(hp, &hl); if (!h) die("Can't read %s\n", hp);
    uint8_t *ho = malloc((size_t)hl * 2 + 64); long o = 2, w = 2; int nh = 0;
    while (o < hl) {
        for (int k = 0; k < 3; k++) {                   /* name, nationality, date of birth */
            int l = h[o]; if (o + 1 + l > hl) die("PLHIST.BIN looks damaged (record %d)\n", nh);
            if (k == 1) {
                char v[64]; snprintf(v, sizeof v, "%.*s", l, (char *)h + o + 1);
                const char *m = nation(v); size_t L = strlen(m);
                ho[w++] = (uint8_t)L; memcpy(ho + w, m, L); w += (long)L;
            } else { memcpy(ho + w, h + o, (size_t)l + 1); w += l + 1; }
            o += 1 + l;
        }
        ho[w++] = 0; ho[w++] = 0;                        /* 97/98 has two more bytes here */
        while (o < hl && h[o] != 0xFF) {                  /* seasons: year, club, apps, goals */
            int l = h[o + 1]; memcpy(ho + w, h + o, (size_t)l + 4); w += l + 4; o += l + 4;
        }
        if (o >= hl) die("PLHIST.BIN looks damaged (record %d)\n", nh);
        ho[w++] = 0xFF; o++; nh++;
    }
    ho[0] = (uint8_t)nh; ho[1] = (uint8_t)(nh >> 8);
    say("Player histories: %d\n", nh);
    copy_pictures(cm2, pictures_from);
    copy_sounds(cm2, game);

    /* ---- write ---- */
    char q[1400];
    snprintf(q, sizeof q, "%s/TMDATA.DB1", out_dir); db_write(&T, q, uid + 1);
    snprintf(q, sizeof q, "%s/PLAYERS.DB1", out_dir); db_write(&P, q, (uint32_t)np + 1);
    snprintf(q, sizeof q, "%s/MGDATA.DB1", out_dir); db_write(&M, q, (uint32_t)M.n + 1);
    snprintf(q, sizeof q, "%s/PLHIST98.BIN", out_dir);
    { FILE *f = fopen(q, "wb"); if (!f || fwrite(ho, 1, (size_t)w, f) != (size_t)w || fclose(f)) die("Can't write %s\n", q); }
    snprintf(q, sizeof q, "%s/database.cfg", out_dir);
    { FILE *f = fopen(q, "w"); if (!f) die("Can't write %s\n", q);
      fprintf(f, "# made by the installer from %s\nname = %s\n"
                 "# the day a new game starts\nstart = %s\n", sname, sname, sdate);
      fprintf(f, "# leagues that can be chosen for a new game (the others are greyed out)\nleagues = english scottish");
      for (int a = 0; a < nadd; a++) fprintf(f, " %s", homes[add_home[a]].league);
      fprintf(f, "\n# match sounds: shared (the game folder's SOUNDS) | own (this folder's sounds folder, for a\n"
                 "# database with sounds of its own; the shared ones are used for any it lacks)\nsounds = shared\n");
      fclose(f); }
    say("%s imported. In the game, use Change Database on the Options screen.\n", sname);
    if (logf) { fclose(logf); logf = NULL; }
    return 0;
}
