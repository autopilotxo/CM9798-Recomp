/* backup.c - copies a saved game somewhere safe before the game overwrites it.
 *
 * A CM 97/98 save in slot N is a set of files named <KIND>N.S16 (PLDATA1.S16, TMDATA1.S16, ...)
 * plus the slot list GAMESS16.IDX. Three of them, MCHFAX/MGHIST/PLHIST (match facts and
 * history), are fixed-size files the game also updates while you play, without saving.
 *
 * dos.c calls backup_before_write() just before the game creates or opens one of its files for
 * writing. The whole slot is copied to saves/<database>/saveN/backups/<date>_<time>/ when:
 *   - a save starts writing (first write to a file of the slot that isn't a history file; the
 *     rest of the same save, within a minute, doesn't trigger another copy), and
 *   - the first time anything of that slot is written in this session (the slot exactly as
 *     last saved, before the history files start changing).
 * The newest `g_backup_keep` backups of each slot are kept (0 = no backups).
 * To restore one: Restore Backup on the Restore Game screen (backup_list / backup_restore below),
 * or copy its files back into its saveN folder with the game closed.
 */
#include "backup.h"
#include "autosave.h"
#include "database.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

int g_backup_keep = 5;
static char game_dir[1024];
static int touched[10];            /* slot written to in this session */
static time_t last_save_backup[10];

void backup_init(const char *dir) { snprintf(game_dir, sizeof game_dir, "%s", dir && *dir ? dir : "."); }
const char *cm_game_dir(void) { return game_dir; }

/* "C:\CM2\PLDATA3.S16" -> slot 3, history = 0; anything else -> 0 */
int cm_save_slot(const char *path, int *history) {
    const char *b = path + strlen(path);
    while (b > path && b[-1] != '\\' && b[-1] != '/' && b[-1] != ':') b--;
    size_t n = strlen(b);
    if (n < 6 || strcasecmp(b + n - 4, ".S16") || !isdigit((unsigned char)b[n - 5])) return 0;
    int slot = b[n - 5] - '0';
    *history = !strncasecmp(b, "MCHFAX", 6) || !strncasecmp(b, "MGHIST", 6) || !strncasecmp(b, "PLHIST", 6);
    return slot;
}

int cm_copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"); if (!in) return 0;
    FILE *out = fopen(to, "wb"); if (!out) { fclose(in); return 0; }
    static char buf[1 << 20]; size_t n; int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    fclose(in); if (fclose(out)) ok = 0;
    return ok;
}

static void remove_dir(const char *dir) {
    DIR *d = opendir(dir); if (!d) return;
    struct dirent *e; char p[1400];
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name); remove(p);
    }
    closedir(d); rmdir(dir);
}

static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* keep the newest g_backup_keep backups of a slot (folder names sort by date) */
static void prune(const char *slotdir) {
    DIR *d = opendir(slotdir); if (!d) return;
    char *names[512]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 512)
        if (e->d_name[0] != '.' && strlen(e->d_name) == 17) names[n++] = strdup(e->d_name);   /* YYYY-MM-DD_HHMMSS */
    closedir(d);
    qsort(names, (size_t)n, sizeof *names, cmpstr);
    for (int i = 0; i < n; i++) {
        if (i < n - g_backup_keep) { char p[1400]; snprintf(p, sizeof p, "%s/%s", slotdir, names[i]); remove_dir(p); }
        free(names[i]);
    }
}

static void backup_copy(int slot, int write, const char *why, int do_prune);
void backup_slot_now(int slot, int write, const char *why) { backup_copy(slot, write, why, 1); }
static void backup_copy(int slot, int write, const char *why, int do_prune) {
    char saves[1100], src[1200], slotdir[1300], dest[1400];
    cm_saves_dir(write, saves, sizeof saves);
    cm_slot_dir(slot, write, src, sizeof src);
    snprintf(slotdir, sizeof slotdir, "%s/backups", src); MKDIR(slotdir);
    time_t now = time(NULL); struct stat st;
    for (int tries = 0; ; tries++, now++) {            /* a new folder: never mix two backups made in the same second */
        struct tm *t = localtime(&now);
        snprintf(dest, sizeof dest, "%s/%04d-%02d-%02d_%02d%02d%02d", slotdir,
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
        if (stat(dest, &st) != 0) break;
        if (tries > 60) { fprintf(stderr, "backup: no free name for a backup in %s\n", slotdir); return; }
    }
    if (MKDIR(dest) != 0) { fprintf(stderr, "backup: can't create %s\n", dest); return; }
    int files = 0; char from[1500], to[1700];
    for (int pass = 0; pass < 2; pass++) {                  /* the slot's files, then the slot list */
        const char *dir = pass ? saves : src;
        DIR *d = opendir(dir); if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            int hist, s = cm_save_slot(e->d_name, &hist);
            if (pass ? strcasecmp(e->d_name, "GAMESS16.IDX") != 0 : s != slot) continue;
            snprintf(from, sizeof from, "%s/%s", dir, e->d_name);
            snprintf(to, sizeof to, "%s/%s", dest, e->d_name);
            if (cm_copy_file(from, to)) files++;
            else fprintf(stderr, "backup: couldn't copy %s\n", e->d_name);
        }
        closedir(d);
    }
    fprintf(stderr, "backup: slot %d (%s): %d files -> %s\n", slot, why, files, dest);
    if (do_prune) prune(slotdir);
}

void backup_before_write(const char *dos_path) {
    int history, slot = cm_save_slot(dos_path, &history);
    if (g_backup_keep <= 0 || slot < 1 || slot > 9 || autosave_active()) return;   /* autosaves go elsewhere */
    time_t now = time(NULL);
    if (!touched[slot]) {
        touched[slot] = 1;
        backup_slot_now(slot, 1, history ? "as last saved" : "before saving");
        if (!history) last_save_backup[slot] = now;
        return;
    }
    if (!history && now - last_save_backup[slot] > 60) {
        last_save_backup[slot] = now;
        backup_slot_now(slot, 1, "before saving");
    }
}

/* ---- restoring (the Restore Backup panel on the Restore Game screen; the chosen database) ---- */

/* the slot list, GAMESS16.IDX: 80 bytes per slot, starting with the slot's name ("Unused Game #2"
   when empty) */
#define IDX_ENTRY 80
static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(dir); struct dirent *e; int ok = 0;
    if (!d) return 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}
static int idx_name(const char *idx_file, int slot, char *out, size_t n) {
    out[0] = 0;
    FILE *f = fopen(idx_file, "rb"); if (!f) return 0;
    unsigned char e[IDX_ENTRY]; int ok = fseek(f, (long)(slot - 1) * IDX_ENTRY, SEEK_SET) == 0 && fread(e, 1, sizeof e, f) == sizeof e;
    fclose(f);
    if (!ok) return 0;
    size_t k = 0; while (k < sizeof e && e[k] && k + 1 < n) { out[k] = (char)e[k]; k++; } out[k] = 0;
    return out[0] && strncmp(out, "Unused", 6) != 0;
}
int backup_slot_name(int slot, char *out, size_t n) {
    char saves[1100], idx[1300]; cm_saves_dir(0, saves, sizeof saves);
    if (!find_ci(saves, "GAMESS16.IDX", idx, sizeof idx)) { out[0] = 0; return 0; }
    return idx_name(idx, slot, out, n);
}

static int cmpstr_desc(const void *a, const void *b) { return -strcmp((const char *)a, (const char *)b); }
int backup_list(int slot, struct backup_info *out, int max) {
    char sd[1200], dir[1300]; cm_slot_dir(slot, 0, sd, sizeof sd);
    snprintf(dir, sizeof dir, "%s/backups", sd);
    DIR *d = opendir(dir); if (!d) return 0;
    int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        const char *s = e->d_name;                      /* YYYY-MM-DD_HHMMSS */
        if (strlen(s) != 17 || s[4] != '-' || s[7] != '-' || s[10] != '_') continue;
        snprintf(out[n].name, sizeof out[n].name, "%s", s); n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, cmpstr_desc);    /* newest first (the name is first in the struct) */
    static const char *const wd[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const mo[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    for (int i = 0; i < n; i++) {
        int Y, M, D, h, m, sec; struct backup_info *b = &out[i];
        if (sscanf(b->name, "%4d-%2d-%2d_%2d%2d%2d", &Y, &M, &D, &h, &m, &sec) != 6 || M < 1 || M > 12) { snprintf(b->when, sizeof b->when, "%s", b->name); }
        else {
            struct tm t; memset(&t, 0, sizeof t); t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D; t.tm_hour = 12;
            mktime(&t);                                 /* works out the weekday */
            snprintf(b->when, sizeof b->when, "%s %d %s %d, %02d:%02d", wd[t.tm_wday % 7], D, mo[M - 1], Y, h, m); (void)sec;
        }
        char bd[1400], idx[1500]; snprintf(bd, sizeof bd, "%s/%s", dir, b->name);
        b->manager[0] = 0; b->last[0] = 0;
        if (find_ci(bd, "GAMESS16.IDX", idx, sizeof idx)) idx_name(idx, slot, b->manager, sizeof b->manager);
    }
    return n;
}

int backup_restore(int slot, const char *name) {
    char saves[1100], sd[1200], src[1500];
    cm_saves_dir(0, saves, sizeof saves); cm_slot_dir(slot, 0, sd, sizeof sd);
    snprintf(src, sizeof src, "%s/backups/%s", sd, name);
    /* the backup must hold this slot's saved game */
    int have = 0; DIR *d = opendir(src); struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d))) { int hist; if (cm_save_slot(e->d_name, &hist) == slot) have++; }
    closedir(d);
    if (!have) return -1;
    backup_copy(slot, 0, "before restoring a backup", 0);   /* so the restore can be undone */
    int files = 0, failed = 0; char from[1700], to[1800];
    d = opendir(src); if (!d) return -1;
    while ((e = readdir(d))) {
        int hist; if (cm_save_slot(e->d_name, &hist) != slot) continue;
        snprintf(from, sizeof from, "%s/%s", src, e->d_name);
        char existing[1800];                            /* keep the name the slot folder already uses */
        if (find_ci(sd, e->d_name, existing, sizeof existing)) snprintf(to, sizeof to, "%s", existing);
        else snprintf(to, sizeof to, "%s/%s", sd, e->d_name);
        if (cm_copy_file(from, to)) files++; else { failed++; fprintf(stderr, "backup: couldn't restore %s\n", e->d_name); }
    }
    closedir(d);
    /* the slot's line in the slot list (its name; an empty slot becomes a saved game again) */
    char bidx[1700], idx[1300];
    if (find_ci(src, "GAMESS16.IDX", bidx, sizeof bidx)) {
        FILE *f = fopen(bidx, "rb"); unsigned char ent[IDX_ENTRY]; int ok = 0;
        if (f) { ok = fseek(f, (long)(slot - 1) * IDX_ENTRY, SEEK_SET) == 0 && fread(ent, 1, sizeof ent, f) == sizeof ent; fclose(f); }
        if (ok) {
            if (!find_ci(saves, "GAMESS16.IDX", idx, sizeof idx)) {   /* no slot list at all: take the backup's */
                snprintf(idx, sizeof idx, "%s/GAMESS16.IDX", saves); cm_copy_file(bidx, idx);
            } else if ((f = fopen(idx, "r+b")) != NULL) {
                if (fseek(f, (long)(slot - 1) * IDX_ENTRY, SEEK_SET) == 0) fwrite(ent, 1, sizeof ent, f);
                fclose(f);
            }
        }
    }
    fprintf(stderr, "backup: slot %d restored from %s (%d files%s)\n", slot, name, files, failed ? ", some failed" : "");
    if (g_backup_keep > 0) { char bd[1300]; snprintf(bd, sizeof bd, "%s/backups", sd); prune(bd); }
    return failed ? -1 : files;
}

/* ---- what a backup holds: the player's last match (the Restore Backup panel) ----
 * Read from the save files themselves (layouts: port-analysis/formats):
 *   MGDATAn: count, then per manager 2 strings + 0x27 bytes (club: u16 at +12); then a byte, the
 *            number of human managers, who are the last ones in the list
 *   TMDATAn: count, then per club 7 strings (the first is the short name) + 0x5BA bytes
 *   CPDATAn: count, then per competition 5 strings (the third: "UEFA Cup", "Premier Div") + 0x7D4
 *   FXDATAn: from 0x344, 300 match days of 0x695 bytes: 120 fixtures of 14 bytes (competition u16,
 *            low byte = competition; home u16; away u16; ...; at +11 a u16 whose low 14 bits are
 *            1 + the match's record in MCHFAX once it has been played), then the day's date (d, m, yy)
 *   MCHFAXn: records of 0x887 bytes; the score at +8 (home) and +9 (away) */
static unsigned char *slurp(const char *dir, const char *kind, int slot, long *len) {
    char name[32], p[1500]; snprintf(name, sizeof name, "%s%d.S16", kind, slot);
    if (!find_ci(dir, name, p, sizeof p)) return NULL;
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = n > 0 ? malloc((size_t)n) : NULL;
    if (d && fread(d, 1, (size_t)n, f) != (size_t)n) { free(d); d = NULL; }
    fclose(f); *len = n; return d;
}
static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
/* the strings of record i of a list file (nstr strings, then size bytes); NULL if malformed */
static const unsigned char *list_rec(const unsigned char *d, long len, int nstr, int size, unsigned i, long *after) {
    if (len < 2) return NULL;
    unsigned n = rd16(d); long o = 2;
    for (unsigned r = 0; r < n; r++) {
        const unsigned char *rec = d + o;
        for (int k = 0; k < nstr; k++) { if (o + 2 > len) return NULL; o += 2 + rd16(d + o); }
        o += size; if (o > len) return NULL;
        if (r == i) { if (after) *after = o; return rec; }
    }
    if (after) *after = o;
    return i == 0xFFFFFFFFu ? d + o : NULL;          /* (all of them: where the list ends) */
}
static void rec_str(const unsigned char *rec, int k, char *out, size_t n) {   /* string k of a record */
    for (int j = 0; j < k; j++) rec += 2 + rd16(rec);
    size_t l = rd16(rec); if (l >= n) l = n - 1;
    memcpy(out, rec + 2, l); out[l] = 0;
    while (l && out[l - 1] == ' ') out[--l] = 0;
}

static struct { char path[1400]; char last[96]; } desc_cache[64];
static int desc_n;
void backup_describe(int slot, struct backup_info *b) {
    char sd[1200], dir[1400]; cm_slot_dir(slot, 0, sd, sizeof sd);
    snprintf(dir, sizeof dir, "%s/backups/%s", sd, b->name);
    for (int i = 0; i < desc_n; i++) if (!strcmp(desc_cache[i].path, dir)) { snprintf(b->last, sizeof b->last, "%s", desc_cache[i].last); return; }
    b->last[0] = 0;
    long lm = 0, lt = 0, lc = 0, lf = 0, end;
    unsigned char *mg = slurp(dir, "MGDATA", slot, &lm), *tm = NULL, *cp = NULL, *fx = NULL;
    int club = -1;
    if (mg && list_rec(mg, lm, 2, 0x27, 0xFFFFFFFFu, &end) && end < lm) {
        unsigned n = rd16(mg), humans = mg[end];
        const unsigned char *r = humans && humans <= n ? list_rec(mg, lm, 2, 0x27, n - humans, &end) : NULL;
        if (r) { const unsigned char *body = r; for (int k = 0; k < 2; k++) body += 2 + rd16(body);
                 unsigned c = rd16(body + 12); if (c != 0xFFFF) club = (int)c; }
    }
    fx = club >= 0 ? slurp(dir, "FXDATA", slot, &lf) : NULL;
    int best = -1, bday = 0, bmon = 0, byr = 0, bcomp = 0;
    if (fx && lf >= 0x344 + 300L * 0x695) {
        for (int day = 0; day < 300; day++) {
            const unsigned char *db = fx + 0x344 + (long)day * 0x695, *dt = db + 120 * 14;
            int d = dt[0], m = dt[1], y = dt[2];
            if (d < 1 || d > 31 || m < 1 || m > 12) continue;
            for (int k = 0; k < 120; k++) {
                const unsigned char *e = db + k * 14;
                if ((int)rd16(e + 2) != club && (int)rd16(e + 4) != club) continue;
                int mf = (int)(int16_t)(rd16(e + 11) << 2) >> 2;
                if (mf <= 0) continue;                          /* not played */
                if (best < 0 || y * 10000 + m * 100 + d > byr * 10000 + bmon * 100 + bday) {
                    best = mf - 1; bday = d; bmon = m; byr = y; bcomp = e[0];
                }
            }
        }
    }
    if (best >= 0) {
        char p[1500], name[32]; unsigned char r[12]; int ok = 0;
        snprintf(name, sizeof name, "MCHFAX%d.S16", slot);
        if (find_ci(dir, name, p, sizeof p)) { FILE *f = fopen(p, "rb");
            if (f) { ok = fseek(f, (long)best * 0x887, SEEK_SET) == 0 && fread(r, 1, sizeof r, f) == sizeof r; fclose(f); } }
        tm = ok ? slurp(dir, "TMDATA", slot, &lt) : NULL;
        cp = ok ? slurp(dir, "CPDATA", slot, &lc) : NULL;
        const unsigned char *h = tm ? list_rec(tm, lt, 7, 0x5BA, rd16(r + 2), NULL) : NULL;
        const unsigned char *a = tm ? list_rec(tm, lt, 7, 0x5BA, rd16(r + 4), NULL) : NULL;
        const unsigned char *c = cp ? list_rec(cp, lc, 5, 0x7D4, (unsigned)bcomp, NULL) : NULL;
        if (h && a) {
            char hn[40], an[40], cn[40] = ""; rec_str(h, 0, hn, sizeof hn); rec_str(a, 0, an, sizeof an);
            if (c) rec_str(c, 2, cn, sizeof cn);
            snprintf(b->last, sizeof b->last, "%d.%d.%02d  %s %d-%d %s%s%s%s", bday, bmon, byr % 100, hn, r[8], r[9], an,
                     cn[0] ? " (" : "", cn, cn[0] ? ")" : "");
        }
    }
    free(mg); free(tm); free(cp); free(fx);
    if (desc_n < 64) { snprintf(desc_cache[desc_n].path, sizeof desc_cache[desc_n].path, "%s", dir);
                       snprintf(desc_cache[desc_n].last, sizeof desc_cache[desc_n].last, "%s", b->last); desc_n++; }
}
