/* dbcheck.c - the Database Editor's Test Database: reads a database's files the way a new game
 * does and says what would stop it or go wrong, so a player can check an edited database before
 * starting a career on it.
 *
 * Problems (a new game would stop, or be wrong):
 *   - a file that can't be read, or whose record count doesn't match its size
 *   - a player of a nation the game doesn't know (it stops with "Unrecognised Nation"): nations are
 *     the national teams' names, in any of the seven languages
 *   - a club with a division code the game doesn't have
 *   - a playable division (of the leagues database.cfg offers) with fewer than 2 clubs
 *   - a club in one with no players at all (as Rio Ave was before 1.05), or two clubs of one name there
 *   - a club in a division whose nation is a continent or EXTINCT (as national and former teams have):
 *     the game reads such a team as a nation's, not a club (Seraing in the first 1995/96 imports)
 * Notes (usually harmless): a playable division with another number of clubs than 97/98's (the game
 * draws up a full fixture list for it: 22 clubs play 42 matches each), clubs there with fewer than 11
 * players, no manager or more than one, and players or managers whose club isn't in the database. Each says whether the original 97/98 database has it too,
 * as it has a few of each.
 *
 * Players and managers name their club by its full or short English name, or a shortened form of the
 * full one ("Sheff U", "Partick T": each word the start of one of its words), as the game's data does.
 * Integers in .DB1 files are big-endian; strings Latin-1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include "dbcheck.h"

typedef struct { char name[40]; int type, size, off; } Field;
typedef struct { Field f[80]; int nf, rs, n; uint8_t *rec; int ok; char why[160]; } DB;

static void db_free(DB *d) { free(d->rec); memset(d, 0, sizeof *d); }
static int db_read(DB *d, const char *dir, const char *file) {
    memset(d, 0, sizeof *d);
    char p[1400]; snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "rb");
    if (!f) { char lower[64]; size_t i = 0; for (; file[i] && i < 63; i++) lower[i] = (char)tolower((unsigned char)file[i]); lower[i] = 0;
              snprintf(p, sizeof p, "%s/%s", dir, lower); f = fopen(p, "rb"); }
    if (!f) { snprintf(d->why, sizeof d->why, "%s is missing", file); return 0; }
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc((size_t)len + 1); if (!b || fread(b, 1, (size_t)len, f) != (size_t)len) { fclose(f); free(b); snprintf(d->why, sizeof d->why, "%s can't be read", file); return 0; }
    fclose(f);
    int nf = len > 2 ? b[0] | b[1] << 8 : 0, o = 2, off = 0, uid = 0;
    if (nf <= 0 || nf > 80) { free(b); snprintf(d->why, sizeof d->why, "%s isn't a CM database file", file); return 0; }
    for (int i = 0; i < nf; i++) {
        if (o + 1 >= len || o + 1 + b[o] + 4 > len) { free(b); snprintf(d->why, sizeof d->why, "%s: its list of fields is cut short", file); return 0; }
        int l = b[o]; Field *F = &d->f[i];
        snprintf(F->name, sizeof F->name, "%.*s", l, (char *)b + o + 1); o += 1 + l;
        F->type = b[o]; F->size = b[o + 1]; F->off = off; off += F->size; o += 4;
        if (!strcmp(F->name, "Unique ID") && F->type == 0 && F->size == 4) uid = 1;
    }
    d->nf = nf; d->rs = off;
    int pre = uid ? 6 : 2;
    if (off <= 0 || o + pre > len) { free(b); snprintf(d->why, sizeof d->why, "%s has no records", file); return 0; }
    d->n = (int)((len - o - pre) / off);
    int counted = b[o] << 7 | b[o + 1];
    if ((long)o + pre + (long)d->n * off != len) snprintf(d->why, sizeof d->why, "%s: its size isn't a whole number of records (%ld bytes over)", file, (long)(len - o - pre) % off);
    else if (counted != d->n) snprintf(d->why, sizeof d->why, "%s: it says %d records, but holds %d", file, counted, d->n);
    d->rec = malloc((size_t)d->n * off + 1); memcpy(d->rec, b + o + pre, (size_t)d->n * off);
    free(b); d->ok = 1; return 1;
}
static const Field *fld(const DB *d, const char *name) { for (int i = 0; i < d->nf; i++) if (!strcmp(d->f[i].name, name)) return &d->f[i]; return NULL; }
static void gets_(const DB *d, int i, const char *name, char *out, size_t n) {
    const Field *f = fld(d, name); out[0] = 0; if (!f || i < 0 || i >= d->n) return;
    const uint8_t *r = d->rec + (size_t)i * d->rs; size_t k = 0;
    while (k < (size_t)f->size && r[f->off + k] && k + 1 < n) { out[k] = (char)r[f->off + k]; k++; }
    out[k] = 0; while (k && out[k - 1] == ' ') out[--k] = 0;
}

/* Latin-1 -> lower-case ASCII, accents dropped, '-' as a space */
static void fold(const char *s, char *out, size_t n) {
    static const char *const hi = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && k + 1 < n; p++) {
        int c = *p;
        if (c >= 0xC0) c = (unsigned char)hi[c - 0xC0];
        if (c == '-') c = ' ';
        out[k++] = (char)tolower(c);
    }
    out[k] = 0;
}
static int is_continent(const char *s) {
    static const char *const c[] = { "Europe", "Africa", "Asia", "South America", "CONCACAF", "Oceania", "North America", "EXTINCT", NULL };
    for (int i = 0; c[i]; i++) if (!strcmp(s, c[i])) return 1;
    return 0;
}

/* ---- a database, read for checking ---- */
typedef struct {
    DB t, p, m;
    int nclub; struct club { char lng[52], sht[32], fl[52], fs[32], div[8]; int players, managers, row; char nat[16]; } *club;
    char (*nation)[40]; int nnat;
} Data;
static void data_free(Data *x) { db_free(&x->t); db_free(&x->p); db_free(&x->m); free(x->club); free(x->nation); memset(x, 0, sizeof *x); }
static int club_find(const Data *x, const char *s) {
    char f[64]; fold(s, f, sizeof f); if (!f[0]) return -1;
    for (int i = 0; i < x->nclub; i++) if (!strcmp(f, x->club[i].fl) || (x->club[i].fs[0] && !strcmp(f, x->club[i].fs))) return i;
    char q[64]; snprintf(q, sizeof q, "%s", f);                     /* "sheff u": each word the start of one */
    char *qw[12]; int nq = 0; for (char *t = strtok(q, " "); t && nq < 12; t = strtok(NULL, " ")) qw[nq++] = t;
    for (int i = 0; i < x->nclub; i++) {
        char w[64]; snprintf(w, sizeof w, "%s", x->club[i].fl);
        char *ww[16]; int nw = 0; for (char *t = strtok(w, " "); t && nw < 16; t = strtok(NULL, " ")) ww[nw++] = t;
        if (nq > nw) continue;
        int ok = 1; for (int k = 0; k < nq && ok; k++) if (strncmp(ww[k], qw[k], strlen(qw[k]))) ok = 0;
        if (ok) return i;
    }
    return -1;
}
static int nation_known(const Data *x, const char *s) {
    for (int i = 0; i < x->nnat; i++) if (!strcasecmp(x->nation[i], s)) return 1;
    return 0;
}
static int data_read(Data *x, const char *dir) {
    memset(x, 0, sizeof *x);
    db_read(&x->t, dir, "TMDATA.DB1"); db_read(&x->p, dir, "PLAYERS.DB1"); db_read(&x->m, dir, "MGDATA.DB1");
    if (!x->t.ok) return 0;
    static const char *const nf[] = { "UK Long Name", "UK Short Name", "Ita Long Name", "Ita Short Name", "Ger Long Name",
        "Ger Short Name", "Spa Long Name", "Spa Short Name", "Fre Long Name", "Fre Short Name", "Por Long Name",
        "Por Short Name", "Dut Long Name", "Dut Short Name", NULL };
    x->club = calloc((size_t)x->t.n + 1, sizeof *x->club); x->nation = calloc((size_t)x->t.n * 14 + 1, sizeof *x->nation);
    for (int i = 0; i < x->t.n; i++) {
        char nt[40]; gets_(&x->t, i, "Nation", nt, sizeof nt);
        char dv[8]; gets_(&x->t, i, "Division", dv, sizeof dv);
        if (is_continent(nt) && !dv[0]) { for (int k = 0; nf[k]; k++) { gets_(&x->t, i, nf[k], x->nation[x->nnat], sizeof x->nation[0]); if (x->nation[x->nnat][0]) x->nnat++; } continue; }
        struct club *c = &x->club[x->nclub++]; c->row = i;
        gets_(&x->t, i, "UK Long Name", c->lng, sizeof c->lng); gets_(&x->t, i, "UK Short Name", c->sht, sizeof c->sht);
        gets_(&x->t, i, "Division", c->div, sizeof c->div);
        if (is_continent(nt)) snprintf(c->nat, sizeof c->nat, "%s", nt);
        fold(c->lng, c->fl, sizeof c->fl); fold(c->sht, c->fs, sizeof c->fs);
    }
    char s[64];
    for (int i = 0; i < x->p.n; i++) { gets_(&x->p, i, "Current Club", s, sizeof s); int c = club_find(x, s); if (c >= 0) x->club[c].players++; }
    for (int i = 0; i < x->m.n; i++) { gets_(&x->m, i, "Current Club", s, sizeof s); int c = club_find(x, s); if (c >= 0) x->club[c].managers++; }
    return 1;
}

/* the playable divisions and their sizes (the game's competitions are built for these) */
static const struct { const char *league, *div; int clubs; } divs[] = {
    { "english", "EPR", 20 }, { "english", "ED1", 24 }, { "english", "ED2", 24 }, { "english", "ED3", 24 },
    { "scottish", "SPR", 10 }, { "scottish", "SD1", 10 }, { "scottish", "SD2", 10 }, { "scottish", "SD3", 10 },
    { "italian", "ISA", 18 }, { "italian", "ISB", 20 }, { "spanish", "SP1", 20 }, { "spanish", "SP2", 22 },
    { "german", "GD1", 18 }, { "german", "GD2", 18 }, { "french", "FD1", 18 }, { "french", "FD2", 22 },
    { "dutch", "HD1", 18 }, { "dutch", "HD2", 18 }, { "portuguese", "PD1", 18 }, { "portuguese", "PD2", 18 },
    { "belgian", "BD1", 18 }, { "belgian", "BD2", 18 }, { NULL, NULL, 0 } };
static const char *const known_divs[] = { "", "BD1", "BD2", "BNL", "DB2", "DD1", "DD2", "DD3", "DNL", "ED1", "ED2", "ED3", "ENL",
    "EPR", "FD1", "FD2", "FNL", "GD1", "GD2", "GNL", "HD1", "HD2", "HNL", "ISA", "ISB", "ISC", "J-L", "MLS", "ND1", "ND2", "NNL",
    "PD1", "PD2", "PNL", "SD1", "SD2", "SD3", "SNL", "SP1", "SP2", "SPN", "SPR", "TR1", "TR2", "WD1", "WD2", "WD3", "WNL", NULL };
static int league_offered(const char *leagues, const char *league) {
    if (!leagues || !leagues[0]) return 1;
    char l[300]; fold(leagues, l, sizeof l);
    for (char *t = strtok(l, " ,"); t; t = strtok(NULL, " ,")) if (!strcmp(t, league)) return 1;
    return 0;
}
static int playable(const char *div, const char *leagues) {
    for (int i = 0; divs[i].div; i++) if (!strcmp(div, divs[i].div)) return league_offered(leagues, divs[i].league);
    return 0;
}

/* ---- the report ---- */
static char **lines; static int nlines, cap;
static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void line(const char *fmt, ...) {
    if (nlines == cap) { cap = cap ? cap * 2 : 64; lines = realloc(lines, sizeof *lines * (size_t)cap); }
    char b[300]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    lines[nlines++] = strdup(b);
}
static const struct club *ref_club(const Data *ref, const char *name) {
    if (!ref) return NULL;
    char f[64]; fold(name, f, sizeof f);
    for (int i = 0; i < ref->nclub; i++) if (!strcmp(ref->club[i].fl, f)) return &ref->club[i];
    return NULL;
}

int dbcheck_run(const char *dir, const char *ref_dir, const char *leagues, char ***out, int *nout) {
    for (int i = 0; i < nlines; i++) free(lines[i]);
    nlines = 0;
    Data d, r; int have_ref = ref_dir && strcmp(ref_dir, dir) && data_read(&r, ref_dir);
    const Data *ref = have_ref ? &r : NULL;
    int problems = 0, notes = 0;
    char ln[300][160]; int nl = 0;                       /* problems first, then notes: gathered, then written */
    char nt[200][160]; int nn = 0;
#define PROBLEM(...) do { problems++; if (nl < 300) snprintf(ln[nl++], 160, __VA_ARGS__); } while (0)
#define NOTE(...)    do { notes++; if (nn < 200) snprintf(nt[nn++], 160, __VA_ARGS__); } while (0)

    data_read(&d, dir);
    const DB *files[3] = { &d.t, &d.p, &d.m };
    for (int k = 0; k < 3; k++) if (!files[k]->ok || files[k]->why[0]) PROBLEM("%s", files[k]->why);
    if (d.t.ok && d.p.ok) {
        /* players' nations */
        char s[64], club[64]; int bad = 0;
        for (int i = 0; i < d.p.n; i++) {
            gets_(&d.p, i, "Nation", s, sizeof s);
            if (s[0] && !nation_known(&d, s)) { char a[40], b[40]; gets_(&d.p, i, "First Name", a, sizeof a); gets_(&d.p, i, "Second Name", b, sizeof b);
                if (bad++ < 12) PROBLEM("Player %s %s: nation \"%s\" isn't one the game knows (it would stop)", a, b, s); }
        }
        if (bad > 12) PROBLEM("... and %d more players with a nation the game doesn't know", bad - 12);
        /* clubs: division codes, playable divisions */
        for (int i = 0; i < d.nclub; i++) {
            int ok = 0; for (int k = 0; known_divs[k]; k++) if (!strcmp(d.club[i].div, known_divs[k])) ok = 1;
            if (!ok) PROBLEM("%s: division \"%s\" isn't one the game has", d.club[i].lng, d.club[i].div);
        }
        for (int k = 0; divs[k].div; k++) {
            if (!league_offered(leagues, divs[k].league)) continue;
            int n = 0; for (int i = 0; i < d.nclub; i++) if (!strcmp(d.club[i].div, divs[k].div)) n++;
            if (n < 2) PROBLEM("Division %s (%s league): %d club%s - a league needs at least 2", divs[k].div, divs[k].league, n, n == 1 ? "" : "s");
            else if (n != divs[k].clubs) NOTE("Division %s (%s league): %d clubs, 97/98 has %d - the game plays %d matches each", divs[k].div, divs[k].league, n, divs[k].clubs, 2 * (n - 1));
        }
        for (int i = 0; i < d.nclub; i++) {
            const struct club *c = &d.club[i]; if (!playable(c->div, leagues)) continue;
            const struct club *o = ref_club(ref, c->lng);
            const char *too = o && playable(o->div, NULL) ? " (as in the 97/98 database)" : "";
            if (c->nat[0]) PROBLEM("%s (%s): nation is \"%s\" - a club in a league needs its country", c->lng, c->div, c->nat);
            if (!c->players) PROBLEM("%s (%s): no players", c->lng, c->div);
            else if (c->players < 11) { int same = o && o->players < 11; NOTE("%s (%s): only %d player%s%s", c->lng, c->div, c->players, c->players == 1 ? "" : "s", same ? too : ""); }
            if (c->managers > 1) NOTE("%s (%s): %d managers - only one can manage it", c->lng, c->div, c->managers);
            if (!c->managers) { int same = o && !o->managers; NOTE("%s (%s): no manager%s", c->lng, c->div, same ? too : ""); }
            for (int j = i + 1; j < d.nclub; j++) if (!strcmp(d.club[j].fl, c->fl)) PROBLEM("Two clubs called %s (%s and %s): players and managers can only name one", c->lng, c->div, d.club[j].div[0] ? d.club[j].div : "no division");
        }
        /* players and managers whose club isn't there: those the 97/98 database has too, and new ones */
        for (int pass = 0; pass < 2; pass++) {
            const DB *x = pass ? &d.m : &d.p; if (!x->ok) continue;
            const char *who = pass ? "managers" : "players";
            struct un { char name[64]; int n, old; } *u = calloc((size_t)x->n + 1, sizeof *u); int nu = 0, total = 0, olds = 0;
            for (int i = 0; i < x->n; i++) {
                gets_(x, i, "Current Club", club, sizeof club);
                if (!club[0] || club_find(&d, club) >= 0) continue;
                total++;
                int k = 0; while (k < nu && strcmp(u[k].name, club)) k++;
                if (k == nu) { snprintf(u[nu].name, sizeof u[0].name, "%s", club); nu++; }
                u[k].n++;
            }
            if (ref) {                                       /* names the original's own players name too */
                const DB *y = pass ? &ref->m : &ref->p;
                for (int k = 0; k < nu; k++) for (int i = 0; i < y->n && !u[k].old; i++) {
                    gets_(y, i, "Current Club", club, sizeof club); if (!strcmp(club, u[k].name) && club_find(ref, club) < 0) u[k].old = 1; }
                for (int k = 0; k < nu; k++) if (u[k].old) olds += u[k].n;
            }
            char ex[200] = ""; int shown = 0, newn = total - olds, newnames = 0;
            for (int k = 0; k < nu; k++) if (!u[k].old) {
                newnames++;
                if (shown < 4) { size_t L = strlen(ex); snprintf(ex + L, sizeof ex - L, "%s%s (%d)", shown ? ", " : "", u[k].name, u[k].n); shown++; } }
            if (newn) NOTE("%d %s name a club that isn't in the database: %s%s", newn, who, ex, newnames > shown ? ", ..." : "");
            if (olds) {
                ex[0] = 0; shown = 0;
                for (int k = 0; k < nu && shown < 3; k++) if (u[k].old) { size_t L = strlen(ex); snprintf(ex + L, sizeof ex - L, "%s%s", shown ? ", " : "", u[k].name); shown++; }
                NOTE("%s%d %s name a club that isn't in the database, as in the 97/98 database (e.g. %s)", newn ? "And " : "", olds, who, ex);
            }
            free(u);
        }
    }
    line("Checked the saved files: %d clubs, %d players, %d managers (save first to include your changes).", d.nclub, d.p.n, d.m.n);
    if (!problems) line("No problems: a new game can start with this database.");
    else line("%d problem%s - a new game could stop or go wrong:", problems, problems == 1 ? "" : "s");
    for (int i = 0; i < nl; i++) line("  %s", ln[i]);
    if (notes) { line(" "); line("%d note%s (usually harmless):", notes, notes == 1 ? "" : "s"); for (int i = 0; i < nn; i++) line("  %s", nt[i]); }
    data_free(&d); if (have_ref) data_free(&r);
    *out = lines; *nout = nlines;
    return problems;
}
