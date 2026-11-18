/* sound.c - the match sounds of Championship Manager 2, which 97/98 dropped.
 *
 * CM2 (CM2.EXE, Miles Sound System 3.02) plays six samples from its SOUNDS folder, 8-bit unsigned mono
 * at 11025 Hz: CROWD.PCM, a crowd loop from kick-off to the final whistle (volume 20 of 127), and on top
 * of it, for the events of the match commentary, REFLONG / REFSHT (whistles), CRCHEER, CRBOO and CRGASP
 * (volume 100). Eight voices; an effect with none free isn't played. 97/98 still has the code (its sound
 * module at 0x10BC768, never started: 0x1AE00E0 is never set) and still gives its commentary the same
 * event numbers as CM2 (and CM2's SND.LIS), so this follows CM2's event table (CM2.EXE 0x8DBA0):
 *
 *     0-4     cheer + short whistle (a goal)      550-552   gasp       900-911    long whistle + boo
 *     100-103 cheer                               600-605   cheer      1000-1004  boo
 *     200-203 boo                                 650-651   boo        1300, 1400, 1500, 1600: short whistle
 *     500-505 cheer                               700-710   gasp
 *                                                 800-805   cheer
 *
 * CM2 pans its effects to 0 (hard left in Miles); here they're in the middle.
 * The hooks (patches.txt): the crowd starts where CM2 starts it (97/98's kick-off, 0x105AFBF), stops at the
 * four places 97/98 still ends the sample, and every commentary line (0x105D72C) gives its event.
 * The files: <game>/SOUNDS, shared by every database (the setup copies CM2's there); a database whose
 * database.cfg says "sounds = own" uses its own sounds folder first.
 */
#include "rt.h"
#include "sound.h"
#include "backup.h"
#include "database.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <stdatomic.h>

int g_sound = 1, g_sound_volume = 100;
enum { CROWD, REFLONG, REFSHT, CHEER, BOO, GASP, NSAMPLES };
static const char *const files[NSAMPLES] = { "CROWD.PCM", "REFLONG.PCM", "REFSHT.PCM", "CRCHEER.PCM", "CRBOO.PCM", "CRGASP.PCM" };
static struct { uint8_t *d; long n; } smp[NSAMPLES];
static int loaded;                       /* 0 not yet, 1 found, -1 not there */
#define VOICES 8
static struct voice { int s, vol, loop; double pos; } voice[VOICES];   /* s < 0: free */
static atomic_flag lock = ATOMIC_FLAG_INIT;
static void lk(void) { while (atomic_flag_test_and_set(&lock)) {} }
static void ul(void) { atomic_flag_clear(&lock); }
static int crowd_on;

static int find_ci(const char *dir, const char *name, char *out, size_t n) {
    DIR *d = opendir(dir); struct dirent *e; int ok = 0;
    if (!d) return 0;
    while ((e = readdir(d))) if (!strcasecmp(e->d_name, name)) { snprintf(out, n, "%s/%s", dir, e->d_name); ok = 1; break; }
    closedir(d); return ok;
}
/* where the sounds come from, in order: the database's own sounds folder (database.cfg "sounds = own"),
   the game folder's SOUNDS (shared by all databases; the setup copies CM2's there), and, for installs made
   before that, any database's sounds folder. Each file separately, so a database's own folder may hold only
   the sounds it changes. */
static int file_in(const char *dir, const char *name, char *out, size_t n) { return dir[0] && find_ci(dir, name, out, n); }
static char loaded_own[1300];            /* the own folder the samples were loaded with ("" = none) */
static int load_all(const char *own) {
    char shared[1300] = "", legacy[1300] = "", p[1600]; const char *g = cm_game_dir();
    find_ci(g, "SOUNDS", shared, sizeof shared);
    {   char dir[1300], sub[1400]; snprintf(dir, sizeof dir, "%s/databases", g);
        DIR *d = opendir(dir); struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
            if (find_ci(sub, "sounds", legacy, sizeof legacy) && find_ci(legacy, files[0], p, sizeof p)) break;
            legacy[0] = 0;
        }
        if (d) closedir(d);
    }
    uint8_t *d[NSAMPLES] = { 0 }; long n[NSAMPLES] = { 0 }; int own_used = 0;
    for (int i = 0; i < NSAMPLES; i++) {
        int k = file_in(own, files[i], p, sizeof p) ? 1 : file_in(shared, files[i], p, sizeof p) ? 2 : file_in(legacy, files[i], p, sizeof p) ? 3 : 0;
        if (!k) goto fail;
        own_used += k == 1;
        FILE *f = fopen(p, "rb"); if (!f) goto fail;
        fseek(f, 0, SEEK_END); n[i] = ftell(f); fseek(f, 0, SEEK_SET);
        d[i] = n[i] > 0 ? malloc((size_t)n[i]) : NULL;
        if (!d[i] || fread(d[i], 1, (size_t)n[i], f) != (size_t)n[i]) { fclose(f); goto fail; }
        fclose(f);
    }
    lk();
    for (int v = 0; v < VOICES; v++) voice[v].s = -1;
    for (int i = 0; i < NSAMPLES; i++) { free(smp[i].d); smp[i].d = d[i]; smp[i].n = n[i]; }
    ul();
    fprintf(stderr, "sound: CM2's match sounds from %s%s\n", shared[0] ? shared : legacy, own_used ? " (and the database's own)" : "");
    return 1;
fail:
    for (int i = 0; i < NSAMPLES; i++) free(d[i]);
    return 0;
}
static int load(void) {
    char own[1300] = "";
    db_own_sounds_dir(own, sizeof own);
    if (loaded && !strcmp(own, loaded_own)) return loaded > 0;
    if (!loaded) for (int i = 0; i < VOICES; i++) voice[i].s = -1;
    snprintf(loaded_own, sizeof loaded_own, "%s", own);
    loaded = load_all(own) ? 1 : -1;
    return loaded > 0;
}
int sound_available(void) { return load(); }

static void play(int s, int vol, int loop) {
    if (!load()) return;
    lk();
    for (int i = 0; i < VOICES; i++) if (voice[i].s < 0) { voice[i].s = s; voice[i].vol = vol; voice[i].loop = loop; voice[i].pos = 0; break; }
    ul();
    if (getenv("CM_SOUNDLOG")) fprintf(stderr, "[sound] %s\n", files[s]);
}
static void crowd(int on) {
    if (on == crowd_on) return;
    crowd_on = on;
    if (on) { play(CROWD, 20, 1); return; }
    lk(); for (int i = 0; i < VOICES; i++) voice[i].s = -1; ul();   /* the final whistle: everything stops */
    if (getenv("CM_SOUNDLOG")) fprintf(stderr, "[sound] crowd off\n");
}
static void event(int c) {
    if (!crowd_on) return;                       /* a match being watched */
    if (getenv("CM_SOUNDLOG")) fprintf(stderr, "[sound] event %d\n", c);
    if (c >= 0 && c <= 4) { play(CHEER, 100, 0); play(REFSHT, 100, 0); }
    else if ((c >= 100 && c <= 103) || (c >= 500 && c <= 505) || (c >= 600 && c <= 605) || (c >= 800 && c <= 805)) play(CHEER, 100, 0);
    else if ((c >= 200 && c <= 203) || (c >= 650 && c <= 651) || (c >= 1000 && c <= 1004)) play(BOO, 100, 0);
    else if ((c >= 550 && c <= 552) || (c >= 700 && c <= 710)) play(GASP, 100, 0);
    else if (c >= 900 && c <= 911) { play(REFLONG, 100, 0); play(BOO, 100, 0); }
    else if (c == 1300 || c == 1400 || c == 1500 || c == 1600) play(REFSHT, 100, 0);
}

/* patches.txt */
void hook_crowd_start(void) { crowd(1); }
void hook_crowd_stop(void) { crowd(0); }
void hook_commentary_event(void) { event((int)(eax & 0xFFFF)); }

void sound_mix(int16_t *out, int frames, int rate) {
    memset(out, 0, (size_t)frames * 2 * sizeof *out);
    if (loaded <= 0) return;                     /* (muted: the sounds go on, unheard, so unmuting mid-match */
    if (!g_sound) { lk();                        /*  picks the crowd up where it would be) */
        for (int v = 0; v < VOICES; v++) { struct voice *x = &voice[v]; if (x->s < 0) continue;
            x->pos += frames * 11025.0 / rate;
            if (x->pos >= smp[x->s].n) { if (x->loop) while (x->pos >= smp[x->s].n) x->pos -= (double)smp[x->s].n; else x->s = -1; } }
        ul(); return; }
    lk();
    double step = 11025.0 / rate;
    int master = g_sound_volume < 0 ? 0 : g_sound_volume > 100 ? 100 : g_sound_volume;
    for (int v = 0; v < VOICES; v++) {
        struct voice *x = &voice[v]; if (x->s < 0) continue;
        const uint8_t *d = smp[x->s].d; long n = smp[x->s].n;
        for (int f = 0; f < frames; f++) {
            long i = (long)x->pos;
            if (i >= n) { if (x->loop) { x->pos -= (double)n; i = (long)x->pos; } else { x->s = -1; break; } }
            int smpv = ((int)d[i] - 128) * 256 * x->vol / 127 * master / 100;
            int l = out[2 * f] + smpv, r = out[2 * f + 1] + smpv;
            out[2 * f] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            out[2 * f + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
            x->pos += step;
        }
    }
    ul();
}
