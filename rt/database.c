/* database.c - several game databases side by side, one folder each.
 *
 *   <game>/databases/9798/   PLAYERS.DB1 TMDATA.DB1 MGDATA.DB1 PLHIST98.BIN  [database.cfg]
 *   <game>/databases/9596/   ... the same files for another season
 *
 * database.cfg (optional):   name = 97/98      shown as "Using 97/98 Database"
 *                            start = Wednesday 16th July 1997   the day a new game starts
 *                                    (also "16.7.1997", or just "1997" for 16 July)
 * Without it, a folder called "9596" means name 95/96, start 1995.
 *
 * When the game opens one of the four database files, dos.c asks db_redirect() and gets the
 * copy from the right folder:
 *   - in the front end (Options screen, New Game; the routine at 0x44C94): the database chosen
 *     with Change Database, saved in cm9798.cfg as "database = <folder>";
 *   - during a career: the database the career was started with, found by the year the career
 *     began (saved with the game) - so a 95/96 career keeps reading 95/96 files.
 * Pictures: the game's .PCX pictures (backgrounds etc.) are read from databases/<db>/pictures/,
 * then databases/9798/pictures/ (so another database only needs the pictures it changes), then
 * the game folder. At start, game-folder .PCX files not yet in databases/9798/pictures are copied there.
 *
 * Hiscores and everything else stay in the game folder, shared by all databases.
 *
 * Saved games: each database has its own seven slots, one folder each:
 *   <game>/saves/9798/GAMESS16.IDX           the slot list
 *   <game>/saves/9798/save1/PLDATA1.S16 ...  slot 1 (+ autosave/ and backups/, see autosave.c, backup.c)
 * dos.c asks save_redirect(). In the front end (the slot lists, Restore, Delete) that is the chosen
 * database's folder; once a career is started or restored ("career" below), its saves go to that
 * career's database folder, even if Change Database is used meanwhile.
 * First run: saves, autosaves and backups found in the game folder are copied into saves/9798
 * (the originals are left where they are).
 *
 * A database folder without database.cfg gets one with those defaults, ready to edit.
 *
 * First run: if databases/9798 doesn't exist, it is made and the four files are copied there
 * from the game folder (the originals stay, so older builds keep working). If there is no
 * databases folder at all and it can't be made, the files are read from the game folder as before.
 */
#include "rt_internal.h"
#include "database.h"
#include "backup.h"
#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

#define A(x) ((uint32_t)(x) + 0x01000000u)
#define CAREER_START_YEAR A(0xA44976)   /* byte, years since 1900; written at new game, saved */

#define MAXDB 16
static const char *const db_files[] = { "players.db1", "tmdata.db1", "mgdata.db1", "plhist98.bin", NULL };
static struct db { char dir[64], name[48], leagues[160]; int start, day, month, own_sounds; } dbs[MAXDB];
static int ndb, sel = -1, scanned, in_frontend, career = -1;   /* career: database of the game being played */
static char choice[64] = "9798";

static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    return b;
}
static int is_db_file(const char *b) {
    for (int i = 0; db_files[i]; i++) if (!strcasecmp(b, db_files[i])) return 1;
    return 0;
}
/* the file in dir whose name matches b ignoring case (Linux is case-sensitive) */
static int find_in(const char *dir, const char *b, char *out, size_t n) {
    DIR *d = opendir(dir); struct dirent *e; int ok = 0;
    if (!d) return 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, b)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d);
    return ok;
}
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }

/* "Tuesday 1st August 1995" (the weekday is optional and worked out anyway), "1.8.1995" or
   "1995" (16 July): day, month, year; 0 if not a date */
static int parse_date(const char *s, int *dd, int *mm, int *yy) {
    static const char *const mon[] = { "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec" };
    int a, b, c;
    if (sscanf(s, " %d.%d.%d", &a, &b, &c) == 3) { *dd = a; *mm = b; *yy = c; }
    else {
        char w[4][32]; int n = sscanf(s, " %31s %31s %31s %31s", w[0], w[1], w[2], w[3]);
        int k = 0;
        if (n >= 1 && !isdigit((unsigned char)w[0][0])) k = 1;          /* a weekday first */
        if (n - k == 3 && isdigit((unsigned char)w[k][0])) {
            *dd = atoi(w[k]); *mm = 0; *yy = atoi(w[k + 2]);              /* "1st" -> 1 */
            for (int i = 0; i < 12; i++) if (!strncasecmp(w[k + 1], mon[i], 3)) *mm = i + 1;
        } else if (n == 1 && sscanf(s, " %d", &a) == 1) { *dd = 16; *mm = 7; *yy = a; }
        else return 0;
    }
    return *yy >= 1980 && *yy <= 2030 && *mm >= 1 && *mm <= 12 && *dd >= 1 && *dd <= 31;
}

static void read_info(struct db *d, const char *path) {
    /* defaults from the folder name: "9596" -> "95/96", 1995 */
    const char *s = d->dir;
    if (strlen(s) == 4 && s[0] >= '0' && s[0] <= '9' && s[1] >= '0' && s[1] <= '9' && s[2] >= '0' && s[3] >= '0') {
        snprintf(d->name, sizeof d->name, "%.2s/%.2s", s, s + 2);
        int y = (s[0] - '0') * 10 + (s[1] - '0'); d->start = y >= 50 ? 1900 + y : 2000 + y;
    } else { snprintf(d->name, sizeof d->name, "%s", s); d->start = 1997; }
    d->day = 16; d->month = 7;                             /* 97/98's first day: 16 July */
    if (!strcmp(s, "9798")) snprintf(d->name, sizeof d->name, "Championship Manager 97/98");
    char cfg[1200]; FILE *f;
    int old = 0;                                           /* written by an earlier version of the port? */
    if (find_in(path, "database.cfg", cfg, sizeof cfg) && (f = fopen(cfg, "r"))) {
        char first[200] = ""; if (!fgets(first, sizeof first, f)) first[0] = 0; fclose(f);
        old = !strncmp(first, "# Championship Manager 97/98 - original database", 48) ||
              !strncmp(first, "# shown as \"Using <name> Database\"; new games start on 16 July", 60);
        if (old) fprintf(stderr, "database: updating %s (made by an earlier version)\n", cfg);
    }
    if (old || !find_in(path, "database.cfg", cfg, sizeof cfg)) {  /* none yet: write the defaults, to edit */
        snprintf(cfg, sizeof cfg, "%s/database.cfg", path);
        if ((f = fopen(cfg, "w"))) {
            static const char *const wd[] = { "Saturday", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday" };
            static const char *const mo[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                              "September", "October", "November", "December" };
            int y = d->start, m = d->month, q = d->day; if (m < 3) { m += 12; y--; }
            int h = (q + 13 * (m + 1) / 5 + y % 100 + (y % 100) / 4 + (y / 100) / 4 + 5 * (y / 100)) % 7;   /* Zeller */
            const char *th = (q % 10 == 1 && q != 11) ? "st" : (q % 10 == 2 && q != 12) ? "nd" : (q % 10 == 3 && q != 13) ? "rd" : "th";
            fprintf(f, "# shown as \"Using <name> Database\"\nname = %s\n# the day a new game starts\nstart = %s %d%s %s %d\n",
                    d->name, wd[h], d->day, th, mo[d->month - 1], d->start);
            fprintf(f, "# match sounds: shared (the game folder's SOUNDS) | own (this folder's sounds folder, for a\n"
                       "# database with sounds of its own; the shared ones are used for any it lacks)\nsounds = shared\n");
            fclose(f); fprintf(stderr, "database: wrote %s\n", cfg);
        }
        return;
    }
    if (!(f = fopen(cfg, "r"))) return;
    char line[256], v[128]; int y, dd, mm;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " name = %127[^\r\n]", v) == 1) { size_t k = strlen(v); while (k && v[k - 1] == ' ') v[--k] = 0; snprintf(d->name, sizeof d->name, "%s", v); }
        else if (sscanf(line, " start = %127[^\r\n]", v) == 1 && parse_date(v, &dd, &mm, &y)) {
            d->start = y; d->day = dd; d->month = mm; }
        else if (sscanf(line, " leagues = %127[^\r\n]", v) == 1) snprintf(d->leagues, sizeof d->leagues, " %s ", v);
        else if (sscanf(line, " sounds = %127s", v) == 1) d->own_sounds = !strcasecmp(v, "own");
    }
    fclose(f);
}

static void first_run(const char *root) {
    char dst[1100], from[1200], to[1300];
    snprintf(dst, sizeof dst, "%s/9798", root);
    if (is_dir(dst)) return;
    for (int i = 0; db_files[i]; i++) if (!find_in(cm_game_dir(), db_files[i], from, sizeof from)) return;   /* nothing to copy */
    if (!is_dir(root)) MKDIR(root);
    if (MKDIR(dst) && !is_dir(dst)) return;
    for (int i = 0; db_files[i]; i++) {
        find_in(cm_game_dir(), db_files[i], from, sizeof from);
        snprintf(to, sizeof to, "%s/%s", dst, base_name(from));
        if (!cm_copy_file(from, to)) fprintf(stderr, "database: could not copy %s\n", from);
    }
    fprintf(stderr, "database: made databases/9798 from the game folder's database files\n");
}

static int cmp_db(const void *a, const void *b) { return strcmp(((const struct db *)b)->dir, ((const struct db *)a)->dir); }  /* newest first */

static void migrate_saves(void);
/* the game folder's pictures -> databases/9798/pictures: any not there yet (originals kept,
   pictures already in the folder never overwritten) */
static void copy_pictures(const char *root) {
    char base[1100], dst[1200], have[1400];
    snprintf(base, sizeof base, "%s/9798", root); snprintf(dst, sizeof dst, "%s/pictures", base);
    if (!is_dir(base)) return;
    DIR *d = opendir(cm_game_dir()); struct dirent *e; int k = 0;
    if (!d) return;
    if (!is_dir(dst)) MKDIR(dst);
    while ((e = readdir(d))) {
        size_t L = strlen(e->d_name);
        if (L < 5 || strcasecmp(e->d_name + L - 4, ".pcx")) continue;
        if (find_in(dst, e->d_name, have, sizeof have)) continue;           /* already there */
        char a[1400], c[1500];
        snprintf(a, sizeof a, "%s/%s", cm_game_dir(), e->d_name);
        snprintf(c, sizeof c, "%s/%s", dst, e->d_name);
        if (cm_copy_file(a, c)) k++; else fprintf(stderr, "database: could not copy %s\n", a);
    }
    closedir(d);
    if (k) fprintf(stderr, "database: copied %d pictures into databases/9798/pictures (the originals are kept)\n", k);
}
static void scan(void) {
    if (scanned) return;
    scanned = 1; ndb = 0;
    char root[1100]; snprintf(root, sizeof root, "%s/databases", cm_game_dir());
    first_run(root);
    copy_pictures(root);
    migrate_saves();
    DIR *d = opendir(root); struct dirent *e;
    if (d) {
        while ((e = readdir(d)) && ndb < MAXDB) {
            if (e->d_name[0] == '.') continue;
            char p[1200]; snprintf(p, sizeof p, "%s/%s", root, e->d_name);
            if (!is_dir(p)) continue;
            char f[1300]; if (!find_in(p, "players.db1", f, sizeof f)) continue;   /* not a database */
            struct db *x = &dbs[ndb++]; memset(x, 0, sizeof *x);   /* (a rescan must not keep old values) */
            snprintf(x->dir, sizeof x->dir, "%s", e->d_name);
            read_info(x, p);
        }
        closedir(d);
    }
    qsort(dbs, (size_t)ndb, sizeof dbs[0], cmp_db);
    sel = -1;
    if (getenv("CM_DATABASE")) snprintf(choice, sizeof choice, "%s", getenv("CM_DATABASE"));   /* testing */
    for (int i = 0; i < ndb; i++) if (!strcasecmp(dbs[i].dir, choice)) sel = i;
    if (sel < 0) for (int i = 0; i < ndb; i++) if (!strcmp(dbs[i].dir, "9798")) sel = i;
    if (sel < 0 && ndb) sel = 0;
    for (int i = 0; i < ndb; i++)
        fprintf(stderr, "database: %s \"%s\" from %d%s\n", dbs[i].dir, dbs[i].name, dbs[i].start, i == sel ? " (chosen)" : "");
    if (!ndb) fprintf(stderr, "database: no databases folder - using the game folder's files\n");
}

/* ---- saved games ---- */
static int mkdirs(const char *p) { if (!is_dir(p)) MKDIR(p); return is_dir(p); }
static const char *save_db(int write) {
    scan();
    int use = (write || !in_frontend) && career >= 0 ? career : sel;
    return use >= 0 ? dbs[use].dir : "9798";
}
/* folders are made when written to (write = 1), not when only looked at */
int cm_saves_dir(int write, char *out, size_t n) {
    char root[1100]; snprintf(root, sizeof root, "%s/saves", cm_game_dir());
    snprintf(out, n, "%s/%s", root, save_db(write));
    if (write) { mkdirs(root); mkdirs(out); }
    return is_dir(out);
}
int cm_slot_dir(int slot, int write, char *out, size_t n) {
    char d[1200]; cm_saves_dir(write, d, sizeof d);
    snprintf(out, n, "%s/save%d", d, slot);
    if (write) mkdirs(out);
    return is_dir(out);
}
static void path_in(const char *dir, const char *b, char *host, size_t n) {
    if (!find_in(dir, b, host, n)) snprintf(host, n, "%s/%s", dir, b);   /* existing name, whatever its case */
}
int save_redirect(const char *dos_path, char *host, size_t n, int write) {
    const char *b = base_name(dos_path);
    int hist, slot = cm_save_slot(b, &hist), idx = !strcasecmp(b, "GAMESS16.IDX");
    if ((slot < 1 || slot > 9) && !idx) return 0;
    if (!write && in_frontend && slot && strncasecmp(b, "GNDATA", 6)) career = sel;   /* restoring a game (the
                                                                     slot list only reads GNDATA) */
    char dir[1300];
    if (idx) cm_saves_dir(write, dir, sizeof dir); else cm_slot_dir(slot, write, dir, sizeof dir);
    path_in(dir, b, host, n);
    return 1;
}

/* copy every file of `from` (not folders) into `to` */
static int copy_files(const char *from, const char *to, int slot_only) {
    DIR *d = opendir(from); struct dirent *e; int k = 0;
    if (!d) return 0;
    while ((e = readdir(d))) {
        char a[1400], c[1600]; int hist;
        if (e->d_name[0] == '.') continue;
        if (slot_only && cm_save_slot(e->d_name, &hist) != slot_only) continue;
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        if (is_dir(a)) continue;
        snprintf(c, sizeof c, "%s/%s", to, e->d_name);
        if (!k) mkdirs(to);
        if (cm_copy_file(a, c)) k++; else fprintf(stderr, "saves: could not copy %s\n", a);
    }
    closedir(d);
    return k;
}
/* first run: the game folder's saves (and the port's autosaves/backups) -> saves/9798 */
static void migrate_saves(void) {
    char root[1100], dst[1200], a[1400], c[1500];
    snprintf(root, sizeof root, "%s/saves", cm_game_dir());
    if (is_dir(root)) return;
    if (!mkdirs(root)) return;
    snprintf(dst, sizeof dst, "%s/9798", root); mkdirs(dst);
    int k = 0;
    if (find_in(cm_game_dir(), "GAMESS16.IDX", a, sizeof a)) { snprintf(c, sizeof c, "%s/%s", dst, base_name(a)); k += cm_copy_file(a, c); }
    for (int slot = 1; slot <= 7; slot++) {
        snprintf(c, sizeof c, "%s/save%d", dst, slot);
        int n = copy_files(cm_game_dir(), c, slot);
        snprintf(a, sizeof a, "%s/autosaves/slot%d", cm_game_dir(), slot);
        if (is_dir(a)) { char t[1600]; snprintf(t, sizeof t, "%s/autosave", c); mkdirs(c); n += copy_files(a, t, 0); }
        snprintf(a, sizeof a, "%s/backups/slot%d", cm_game_dir(), slot);
        DIR *d = opendir(a); struct dirent *e;
        if (d) {
            char bk[1600]; snprintf(bk, sizeof bk, "%s/backups", c); mkdirs(c); mkdirs(bk);
            while ((e = readdir(d))) {
                if (e->d_name[0] == '.') continue;
                char from[1600], to[1800]; snprintf(from, sizeof from, "%s/%s", a, e->d_name);
                if (!is_dir(from)) continue;
                snprintf(to, sizeof to, "%s/%s", bk, e->d_name); n += copy_files(from, to, 0);
            }
            closedir(d);
        }
        k += n;
    }
    fprintf(stderr, "saves: first run - copied %d files of saved games into saves/9798 (the originals are kept)\n", k);
}

void db_set_choice(const char *dir) { snprintf(choice, sizeof choice, "%s", dir); scanned = 0; }
const char *db_choice(void) { scan(); return sel >= 0 ? dbs[sel].dir : choice; }
const char *db_name(void) { scan(); return sel >= 0 ? dbs[sel].name : "97/98"; }
int db_start_year(void) { scan(); return sel >= 0 ? dbs[sel].start : 1997; }
int db_start_day(void) { scan(); return sel >= 0 ? dbs[sel].day : 16; }
int db_start_month(void) { scan(); return sel >= 0 ? dbs[sel].month : 7; }
int db_count(void) { scan(); return ndb; }
void db_next(void) {
    scan(); if (ndb < 2) return;
    sel = (sel + 1) % ndb; snprintf(choice, sizeof choice, "%s", dbs[sel].dir);
    fprintf(stderr, "database: now %s (%s)\n", dbs[sel].dir, dbs[sel].name);
}
/* the Choose Database panel (sdl.c): the list, and choosing from it */
int db_selected(void) { scan(); return sel; }
int db_info(int i, const char **name, int *day, int *month, int *year) {
    scan(); if (i < 0 || i >= ndb) return 0;
    *name = dbs[i].name; *day = dbs[i].day; *month = dbs[i].month; *year = dbs[i].start;
    return 1;
}
const char *db_folder(int i) { scan(); return i >= 0 && i < ndb ? dbs[i].dir : NULL; }
const char *db_leagues_of(int i) { scan(); return i >= 0 && i < ndb ? dbs[i].leagues : ""; }
void db_select(int i) {
    scan(); if (i < 0 || i >= ndb) return;
    sel = i; snprintf(choice, sizeof choice, "%s", dbs[sel].dir);
    fprintf(stderr, "database: now %s (%s)\n", dbs[sel].dir, dbs[sel].name);
}
void db_frontend(int in) { in_frontend = in; }
int  db_in_frontend(void) { return in_frontend; }

int db_redirect(const char *dos_path, char *host, size_t n) {
    const char *b = base_name(dos_path);
    if (!is_db_file(b)) return 0;
    scan(); if (!ndb) return 0;
    int use = sel;
    if (in_frontend) career = sel;                        /* starting a new game */
    else if (career >= 0) use = career;                   /* in a career: the database it began with */
    else {                                                /* (not known: by the year it began) */
        int y = 1900 + RD8(CAREER_START_YEAR);
        for (int i = 0; i < ndb; i++) if (dbs[i].start == y) { use = i; break; }
    }
    if (use < 0) return 0;
    char dir[1200]; snprintf(dir, sizeof dir, "%s/databases/%s", cm_game_dir(), dbs[use].dir);
    if (!find_in(dir, b, host, n)) return 0;             /* not in that folder: the game folder's copy */
    if (getenv("CM_FILELOG")) fprintf(stderr, "[file]   -> %s\n", host);
    return 1;
}

/* ---- "Championship Manager 2" backgrounds (F1, Graphics; cm9798.cfg "backgrounds = cm2") ----
   CM2 (CM2.EXE) has one background at a time: STADPIC1-5.PCX in turn, the next one each day the game
   moves on, starting with STADPIC2 when a game begins; and on a club's screens the club's own picture
   (its name, spaces and dots as "_", 8 letters, .PCX) if there is one, else the stadium picture.
   Here every picture 97/98 asks for (generic, country, competition, trophy, club, title screens) becomes
   that: a CM2 picture of the same name if a CM2 database's pictures folder has one (the clubs; the setup
   copies CM2's club pictures and STADPIC1-5 there), else today's STADPIC. Only the publishers' logo
   screens stay. With no STADPIC files anywhere, 97/98's pictures are used as before. */
int g_cm2_backgrounds;
__attribute__((constructor)) static void cm2_bg_env(void) {
    const char *e = getenv("CM_BACKGROUNDS"); if (e) g_cm2_backgrounds = !strcasecmp(e, "cm2");
}
static long bg_days(int y, int m, int d) {          /* days since 1970-01-01 */
    y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static int cm2_pic_dir(int k, char *dir, size_t n, int game_folder) {   /* k-th folder of CM2 pictures (0: none left) */
    int use = !in_frontend && career >= 0 ? career : sel, i = 0;
    if (use >= 0 && strcmp(dbs[use].dir, "9798")) {    /* the database being played first, if it's a CM2 one */
        if (k == 0) { snprintf(dir, n, "%s/databases/%s/pictures", cm_game_dir(), dbs[use].dir); return 1; }
        k--;
    }
    for (i = 0; i < ndb; i++) {
        if (i == use || !strcmp(dbs[i].dir, "9798")) continue;
        if (k-- == 0) { snprintf(dir, n, "%s/databases/%s/pictures", cm_game_dir(), dbs[i].dir); return 1; }
    }
    if (game_folder && k == 0) { snprintf(dir, n, "%s", cm_game_dir()); return 1; }   /* STADPICn put there by hand */
    return 0;
}
static int cm2_find(const char *b, char *host, size_t n, int game_folder) {   /* (the game folder holds 97/98's) */
    char dir[1300];
    for (int k = 0; cm2_pic_dir(k, dir, sizeof dir, game_folder); k++) if (find_in(dir, b, host, n)) return 1;
    return 0;
}
/* sound.c: the sounds folder of the database in use, if its database.cfg says "sounds = own" (0: the shared
   one, <game>/SOUNDS) */
int db_own_sounds_dir(char *out, size_t n) {
    scan(); if (!ndb) return 0;
    int use = !in_frontend && career >= 0 ? career : sel;
    if (use < 0 || !dbs[use].own_sounds) return 0;
    char dir[1300]; snprintf(dir, sizeof dir, "%s/databases/%s", cm_game_dir(), dbs[use].dir);
    if (find_in(dir, "sounds", out, n)) return 1;
    return 0;
}
int cm2_backgrounds_available(void) {            /* the F1 menu greys out "CM 2" without them */
    scan(); if (!ndb) return 0;
    char host[1400];
    for (int i = 1; i <= 5; i++) { char s[16]; snprintf(s, sizeof s, "STADPIC%d.PCX", i); if (cm2_find(s, host, sizeof host, 1)) return 1; }
    return 0;
}
static int cm2_background(const char *b, char *host, size_t n) {
    static int idx = 1;                                 /* 0..4 = STADPIC1..5; a game starts on STADPIC2 */
    static long day = -1;
    if (!strcasecmp(b, "EIDOS.PCX") || !strcasecmp(b, "SILOGO.PCX")) return 0;   /* logos stay */
    if (strncasecmp(b, "STADPIC", 7) && cm2_find(b, host, n, 0)) return 1;   /* a club with a CM2 picture */
    if (in_frontend) { day = -1; idx = 1; }
    else {
        int d = RD8(A(0xA4496C)), m = RD8(A(0xA4496E)), y = 1900 + RD8(A(0xA4496F));
        if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            long t = bg_days(y, m, d);
            if (day < 0 || t < day) idx = 1;            /* a game (re)started: STADPIC2, as CM2 */
            else idx = (int)((idx + (t - day)) % 5);    /* the next one for each day gone by */
            day = t;
        }
    }
    for (int tries = 0; tries < 5; tries++) {          /* (one missing: the next that's there) */
        char s[16]; snprintf(s, sizeof s, "STADPIC%d.PCX", (idx + tries) % 5 + 1);
        if (cm2_find(s, host, n, 1)) return 1;
    }
    return 0;                                           /* no CM2 backgrounds at all: 97/98's */
}

/* dos.c: a .PCX picture -> the database's pictures folder, else 9798's, else the game folder */
int pic_redirect(const char *dos_path, char *host, size_t n) {
    const char *b = base_name(dos_path); size_t L = strlen(b);
    if (L < 5 || strcasecmp(b + L - 4, ".pcx")) return 0;
    scan(); if (!ndb) return 0;
    if (g_cm2_backgrounds && cm2_background(b, host, n)) {
        if (getenv("CM_FILELOG")) fprintf(stderr, "[pic]    %s -> %s\n", b, host);
        return 1;
    }
    int use = !in_frontend && career >= 0 ? career : sel;
    char dir[1300];
    for (int k = 0; k < 2; k++) {
        const char *db = k == 0 ? (use >= 0 ? dbs[use].dir : "9798") : "9798";
        if (k == 1 && !strcmp(db, use >= 0 ? dbs[use].dir : "9798")) break;
        snprintf(dir, sizeof dir, "%s/databases/%s/pictures", cm_game_dir(), db);
        if (find_in(dir, b, host, n)) return 1;
    }
    return 0;
}

/* "Choose Leagues to Run": may this league ("English League" -> "english") be chosen with the
   chosen database? (database.cfg "leagues = english scottish"; no list = all of them) */
int db_league_allowed(const char *label) {
    scan(); if (sel < 0 || !dbs[sel].leagues[0]) return 1;
    char w[40]; size_t k = 0;
    while (label[k] && label[k] != ' ' && k < sizeof w - 3) { w[k + 1] = (char)tolower((unsigned char)label[k]); k++; }
    w[0] = ' '; w[k + 1] = ' '; w[k + 2] = 0;
    char l[170]; size_t n = 0;
    for (const char *p = dbs[sel].leagues; *p && n < sizeof l - 1; p++) l[n++] = (char)tolower((unsigned char)*p);
    l[n] = 0;
    return strstr(l, w) != NULL;
}
