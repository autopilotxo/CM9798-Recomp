/* autosave.c - saves the game automatically at the start of every in-game week.
 *
 * Each save slot gets its own autosave in saves/<database>/saveN/autosave/, outside the game's
 * seven slots, so your own saves are never written by it.
 *
 * Saving: the game's main screen ("Please Select", routine 0x106F4) is overridden so that, each
 * time the game comes back to it in a new week, the game's own save routine (0x45B48, mode 0 =
 * silent save of the current slot, number at 0xA44977) is run with every file it writes
 * redirected into the slot's autosave folder (dos.c asks autosave_redirect()). The slot's
 * other files are copied there first, so the autosave is always a complete set. The game's
 * "last saved" date is put back afterwards, so quitting still asks whether to save.
 *
 * Loading: the game's load routine (0x44C94, which includes the Restore Game list) is wrapped.
 * The first time it opens a file of the chosen slot, if that slot's autosave is newer than the
 * slot's own save, the player is asked whether to load the autosave instead; if so, the slot is
 * backed up (backups/) and the autosave's files are copied over it before the game reads them.
 */
#include "rt_internal.h"
#include "game_funcs.h"
#include "autosave.h"
#include "backup.h"
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

#define A(x) ((uint32_t)(x) + 0x01000000u)
#define G_DAY      A(0xA4496C)   /* byte: day of month */
#define G_MONTH    A(0xA4496E)   /* byte: 1 = January */
#define G_YEAR     A(0xA4496F)   /* byte: years since 1900 */
#define G_WEEKDAY  A(0xA44970)   /* byte: 1 = Sunday .. 7 = Saturday */
#define G_SLOT     A(0xA44977)   /* byte: save slot of the game being played, 1..7 */
#define RETSITE_AFTER_SAVE 0x101092Eu   /* a genuine return site after a call to the save routine */

int g_autosave = 1;
int host_ask(const char *title, const char *text);   /* sdl.c: yes/no dialog; headless.c */
void host_notify(const char *text);                   /* sdl.c: brief note on screen */

static int redirect_slot;            /* while autosaving: slot whose writes go to its autosave folder */
static int loading, load_decided, have_week;
static long last_week;

int autosave_active(void) { return redirect_slot != 0; }

/* the slot's autosave folder (write: of the game being played; 0: of the chosen database) */
static void slot_dir(int slot, int write, char *out, size_t n) {
    char s[1100]; cm_slot_dir(slot, write, s, sizeof s); snprintf(out, n, "%s/autosave", s);
}

static const char *base_name(const char *path) {
    const char *b = path + strlen(path);
    while (b > path && b[-1] != '\\' && b[-1] != '/' && b[-1] != ':') b--;
    return b;
}

/* dos.c: the game is creating / opening for writing `dos_path`: redirect into the autosave? */
int autosave_redirect(const char *dos_path, char *host, size_t n) {
    if (!redirect_slot) return 0;
    int hist; const char *b = base_name(dos_path);
    if (cm_save_slot(b, &hist) != redirect_slot && strcasecmp(b, "GAMESS16.IDX")) return 0;
    char dir[1200]; slot_dir(redirect_slot, 1, dir, sizeof dir);
    /* the game asks for "pldata1.S16", the folder has "PLDATA1.S16": use the existing name, or
       on Linux (case-sensitive) the autosave would end up with two copies */
    const char *name = b; DIR *d = opendir(dir); struct dirent *e; char found[260] = "";
    if (d) { while ((e = readdir(d))) if (!strcasecmp(e->d_name, b)) { snprintf(found, sizeof found, "%s", e->d_name); break; }
             closedir(d); }
    if (found[0]) name = found;
    snprintf(host, n, "%s/%s", dir, name);
    return 1;
}

static long days_from_civil(int y, int m, int d) {          /* days since 1970-01-01 */
    y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
static long game_week(void) {                                  /* the Monday starting the game's current week */
    int d = RD8(G_DAY), m = RD8(G_MONTH), y = 1900 + RD8(G_YEAR), wd = RD8(G_WEEKDAY);
    if (m < 1 || m > 12 || d < 1 || d > 31 || wd < 1 || wd > 7) return -1;
    return days_from_civil(y, m, d) - (wd + 5) % 7;              /* Sunday -> 6 days after Monday */
}
static void game_date(char *out, size_t n) {
    static const char *wd[] = { "", "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *mo[] = { "", "January", "February", "March", "April", "May", "June", "July", "August",
                                "September", "October", "November", "December" };
    int w = RD8(G_WEEKDAY), m = RD8(G_MONTH);
    snprintf(out, n, "%s %d %s %d", w >= 1 && w <= 7 ? wd[w] : "", (int)RD8(G_DAY), m >= 1 && m <= 12 ? mo[m] : "?", 1900 + (int)RD8(G_YEAR));
}

/* name of the file in `dir` matching `name` regardless of case (or `name` itself) */
static const char *existing_name(const char *dir, const char *name) {
    static char found[260]; DIR *d = opendir(dir); struct dirent *e;
    snprintf(found, sizeof found, "%s", name);
    if (d) { while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(found, sizeof found, "%s", e->d_name); break; }
             closedir(d); }
    return found;
}

static int copy_slot_files(const char *from_dir, const char *to_dir, int slot) {
    DIR *d = opendir(from_dir); if (!d) return 0;
    struct dirent *e; int n = 0; char a[1400], b[1400];
    while ((e = readdir(d))) {
        int hist; if (cm_save_slot(e->d_name, &hist) != slot) continue;
        snprintf(a, sizeof a, "%s/%s", from_dir, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to_dir, existing_name(to_dir, e->d_name));
        if (cm_copy_file(a, b)) n++; else fprintf(stderr, "autosave: couldn't copy %s\n", e->d_name);
    }
    closedir(d);
    return n;
}

static void do_autosave(int slot) {
    char dir[1200], tmp[1300], save[1100];
    cm_slot_dir(slot, 1, save, sizeof save);
    slot_dir(slot, 1, dir, sizeof dir); MKDIR(dir);
    /* start from a complete copy of the slot (history files are kept up to date while playing) */
    copy_slot_files(save, dir, slot);

    /* the save routine updates the game's "last saved" state: keep the real one */
    uint8_t keep_a = RD8(A(0xA44974)), keep_b = RD8(A(0xA44968)), keep_c = RD8(A(0xA44967));
    uint32_t keep_d = RD32(A(0xA06EE8)), keep_e = RD32(A(0xA06D50));
    uint32_t r[8] = { eax, ebx, ecx, edx, esi, edi, ebp, esp };
    uint32_t saved_ret = g_ret;

    redirect_slot = slot;
    eax = 0;                                  /* mode 0: save the current slot without asking */
    PUSH32(RETSITE_AFTER_SAVE);
    f_1045b48();
    redirect_slot = 0;

    eax = r[0]; ebx = r[1]; ecx = r[2]; edx = r[3]; esi = r[4]; edi = r[5]; ebp = r[6]; esp = r[7];
    g_ret = saved_ret;
    WR8(A(0xA44974), keep_a); WR8(A(0xA44968), keep_b); WR8(A(0xA44967), keep_c);
    WR32(A(0xA06EE8), keep_d); WR32(A(0xA06D50), keep_e);

    char date[64]; game_date(date, sizeof date);
    snprintf(tmp, sizeof tmp, "%s/autosave.txt", dir);
    FILE *f = fopen(tmp, "w");
    if (f) { fprintf(f, "Autosave of slot %d\ngame date: %s\n", slot, date); fclose(f); }
    fprintf(stderr, "autosave: slot %d saved (%s) -> %s\n", slot, date, dir);
    char note[160]; snprintf(note, sizeof note, "Autosaved  -  %s", date);
    host_notify(note);
}

/* the game is back on its main screen: autosave if a new week has started */
void autosave_main_screen(void) {
    int slot = RD8(G_SLOT);
    long wk = game_week();
    if (getenv("CM_AUTOSAVE_NOW") || getenv("CM_DATELOG")) fprintf(stderr, "main screen: slot %d, date %d/%d/%d weekday %d, season #%d, career began %d\n", slot,
        (int)RD8(G_DAY), (int)RD8(G_MONTH), 1900 + (int)RD8(G_YEAR), (int)RD8(G_WEEKDAY), (int)RD8(A(0xA44978)), 1900 + (int)RD8(A(0xA44976)));
    if (wk < 0) return;
    if (!have_week) {                                            /* just loaded / started: not yet */
        have_week = 1; last_week = wk;
        if (getenv("CM_AUTOSAVE_NOW") && g_autosave && slot >= 1 && slot <= 7) do_autosave(slot);   /* testing */
        return;
    }
    if (wk == last_week) return;
    last_week = wk;
    if (g_autosave && slot >= 1 && slot <= 7) do_autosave(slot);
}

/* ---- loading ---- */
void autosave_load_begin(void) { loading = 1; load_decided = 0; }
void autosave_load_end(void) { loading = 0; have_week = 0; }

static time_t mtime(const char *p) { struct stat st; return stat(p, &st) == 0 ? st.st_mtime : 0; }

/* dos.c: the game is about to open `dos_path` for reading */
void autosave_before_read(const char *dos_path) {
    if (!loading || load_decided) return;
    int hist, slot = cm_save_slot(base_name(dos_path), &hist);
    if (slot < 1 || slot > 7) return;
    /* the slot list reads every slot's GNDATA (name, date); a real load starts with the others */
    if (!strncasecmp(base_name(dos_path), "GNDATA", 6)) return;
    load_decided = 1;
    char dir[1200], save[1100], a[1300], b[1300], date[200] = "";
    slot_dir(slot, 0, dir, sizeof dir);
    cm_slot_dir(slot, 0, save, sizeof save);
    { char nm[32]; snprintf(nm, sizeof nm, "PLDATA%d.S16", slot);
      snprintf(a, sizeof a, "%s/%s", dir, existing_name(dir, nm)); snprintf(b, sizeof b, "%s/%s", save, existing_name(save, nm)); }
    if (!mtime(a) || mtime(a) <= mtime(b)) return;               /* no autosave newer than the save */
    snprintf(a, sizeof a, "%s/autosave.txt", dir);
    FILE *f = fopen(a, "r");
    if (f) { char line[200]; while (fgets(line, sizeof line, f)) if (!strncmp(line, "game date: ", 11)) {
                 snprintf(date, sizeof date, "%s", line + 11); date[strcspn(date, "\r\n")] = 0; } fclose(f); }
    char msg[600];
    snprintf(msg, sizeof msg, "This slot has an autosave from %s, which is newer than the saved game.\n\n"
             "Load the autosave instead?\n\n(Your saved game is kept in the backups folder.)", date[0] ? date : "a later date");
    if (!host_ask("Championship Manager 97/98 - autosave", msg)) return;
    backup_slot_now(slot, 0, "before loading autosave");
    int n = copy_slot_files(dir, save, slot);
    fprintf(stderr, "autosave: loaded slot %d's autosave (%d files)\n", slot, n);
}

/* ---- the overridden game routines ---- */
void f_10106f4(void) { autosave_main_screen(); f_10106f4_orig(); }       /* main screen */
void f_1044c94(void) { db_frontend(1); autosave_load_begin(); f_1044c94_orig(); autosave_load_end(); db_frontend(0); }   /* load game */
