/* overrides.c - hand-written replacements for specific recompiled guest functions.
 *
 * Every function listed in ../overrides.txt is generated as f_XXXXXX_orig; the
 * f_XXXXXX defined here is what all guest code (direct and indirect calls) reaches.
 * An override must behave like the guest function it replaces, including popping its
 * return address (g_ret = POP32()) when it does not delegate to the original.
 *
 * Addresses below are original CM2E16 addresses; A() relocates them (+16 MB).
 */
#include "rt_internal.h"
#include "game_funcs.h"
#include "version.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define A(x) ((uint32_t)(x) + 0x01000000u)

/* ---- game state used by the mouse pointer ---- */
#define PTR_DRAG_ITEM   A(0x0F41E8)   /* int16: item attached to the pointer while dragging, -1 = none
                                         (the game reads it as the high half of the dword at 0x0F41E6) */
#define PTR_ENABLED     A(0x0F43D1)   /* byte: pointer may be shown */
#define PTR_SHAPE       A(0x0F43D0)   /* byte: 0 = arrow, else alternative (football, busy) */
#define PTR_IMG_ARROW   A(0x0F41EC)   /* 11x11 uint16 palette indices, 0 = transparent */
#define PTR_IMG_ALT     A(0x0F42DE)
#define PTR_NEW_X       A(0xADFE5E)   /* position from the mouse handler */
#define PTR_NEW_Y       A(0xADFE60)
#define PTR_DRAWN_X     A(0xADFE58)   /* position the pointer was last drawn at */
#define PTR_DRAWN_Y     A(0xADFE5A)

/* 1 = the window system draws the pointer (smooth, never lags); 0 = original software pointer */
int g_os_pointer = 1;

/* 0x013714 show_mouse_pointer: the game saves the screen under the pointer and draws it into
   the framebuffer, which can only move as fast as the game loop runs. With the OS pointer we
   skip the drawing but keep the bookkeeping, except while an item is being dragged: then the
   original runs so the dragged item still follows the mouse. */
void f_1013714(void) {
    if (!g_os_pointer || (int16_t)RD16(PTR_DRAG_ITEM) != -1) { f_1013714_orig(); return; }
    WR16(PTR_DRAWN_X, RD16(PTR_NEW_X));
    WR16(PTR_DRAWN_Y, RD16(PTR_NEW_Y));
    g_ret = POP32();                         /* behave like the guest 'ret' */
}

/* For the display backend: the pointer the game wants right now.
   Returns 1 if a pointer should be visible, filling rgba (11x11 ARGB8888) from the current palette. */
int guest_pointer(uint32_t argb[11 * 11], int *shape) {
    extern uint8_t vga_dac[256][3];
    /* the Database Editor (rt/editor.c, editor_text.c) has the same pointer code, its variables elsewhere */
    uint32_t enabled = PTR_ENABLED, drag = PTR_DRAG_ITEM, shp = PTR_SHAPE, arrow = PTR_IMG_ARROW, alt = PTR_IMG_ALT;
    if (g_editor_running) { enabled = 0x0103ACBDu; drag = 0x0103AAD4u; shp = 0x0103ACBCu; arrow = 0x0103AAD8u; alt = 0x0103ABCAu; }
    if (!g_os_pointer || !RD8(enabled) || (int16_t)RD16(drag) != -1) return 0;
    *shape = RD8(shp) ? 1 : 0;
    uint32_t img = *shape ? alt : arrow;
    for (int i = 0; i < 121; i++) {
        uint32_t c = RD16(img + 2 * i) & 0xFF;
        if (!c) { argb[i] = 0; continue; }
        uint32_t r = (vga_dac[c][0] << 2) | (vga_dac[c][0] >> 4), g = (vga_dac[c][1] << 2) | (vga_dac[c][1] >> 4),
                 b = (vga_dac[c][2] << 2) | (vga_dac[c][2] >> 4);
        argb[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
    }
    return 1;
}

/* ---- match commentary speed: normal, double or triple ----
 * The game has one speed switch, the byte below (Space flips it during a match): when it is set,
 * each pause between commentary lines is divided by 3. patches.txt makes the divisor come from
 * match_speed_divisor() instead (1, 2 or 3), routes Space here to cycle through the three
 * speeds, and stops the game resetting the speed at every kick-off. The speed is kept in the
 * settings file, and can also be chosen with the buttons drawn on the match screen (sdl.c). */
#define MATCH_FAST A(0xA42B1A)
int  host_match_speed(void);           /* saved setting 1..3 (sdl.c / headless.c) */
void host_set_match_speed(int speed);

void match_speed_apply(void) { WR8(MATCH_FAST, host_match_speed() > 1); }
uint32_t match_speed_divisor(void) { int s = host_match_speed(); return s < 1 ? 1u : s > 3 ? 3u : (uint32_t)s; }

void hook_match_speed_kickoff(void) {
    match_speed_apply();               /* start every match at the saved speed */
    if (getenv("CM_SPEEDLOG")) fprintf(stderr, "[speed] kick-off: speed %d\n", host_match_speed());
}
void hook_match_speed_toggled(void) {  /* Space: normal -> double -> triple -> normal */
    host_set_match_speed(host_match_speed() % 3 + 1);
    match_speed_apply();
    if (getenv("CM_SPEEDLOG")) fprintf(stderr, "[speed] Space: speed %d\n", host_match_speed());
}

/* ---- text changes ----
 * Strings in the game's data, replaced after loading. Each is checked against the original
 * first and the new text must fit in the old space (it is zero-padded). */
static const struct { uint32_t at; const char *old, *new_; } text_patches[] = {
    { 0x0E281C, "Quit and return to DOS/Windows ?", "Quit and return to desktop?" },
    { 0x0E7588, "Delete Game*5|Change Directory*6|Hall Of Fame*7", "Delete Game*5|Change Database*6|Hall Of Fame*7" },
    /* the version on the main menu and the league screen ("9798 v2.93 GBR", sprintf'd with "2.93"): the port's */
    { 0x0E754C, "9798 v%s GBR", "9798-R " CM_PORT_VERSION },
};
/* Remove the space the game puts before "!" and "?" ("Proceed ?" -> "Proceed?") in every text
   string of its data: only inside a run of printable text ending in a zero, and only after a
   non-space character, so nothing but punctuation is touched. */
static void fix_punctuation_spacing(void) {
    uint32_t lo = A(0x0E0000), hi = A(0x0F4000);              /* the program's string literals */
    int fixed = 0;
    for (uint32_t i = lo + 1; i + 1 < hi; i++) {
        if (M[i] != ' ' || (M[i + 1] != '!' && M[i + 1] != '?') || M[i - 1] == ' ' || M[i - 1] < 0x21) continue;
        uint32_t s0 = i, e = i;                                /* the zero-terminated string around it */
        while (s0 > lo && M[s0 - 1]) s0--;
        while (e < hi && M[e]) e++;
        int text = e < hi && e - s0 >= 3;
        for (uint32_t k = s0; k < e && text; k++) text = (M[k] >= 0x20 && M[k] < 0x7F) || M[k] == '\n';
        if (!text) continue;
        memmove(&M[i], &M[i + 1], e - i);                      /* close the gap; the string ends one earlier */
        fixed++;
    }
    if (getenv("CM_TEXTLOG")) fprintf(stderr, "text: removed %d spaces before ! and ?\n", fixed);
}

void apply_text_patches(void) {
    for (unsigned i = 0; i < sizeof text_patches / sizeof text_patches[0]; i++) {
        uint32_t a = A(text_patches[i].at); size_t n = strlen(text_patches[i].old);
        if (memcmp(&M[a], text_patches[i].old, n + 1) != 0 || strlen(text_patches[i].new_) > n) {
            fprintf(stderr, "text patch %u skipped (original text not found)\n", i); continue;
        }
        memset(&M[a], 0, n);
        memcpy(&M[a], text_patches[i].new_, strlen(text_patches[i].new_));
    }
    fix_punctuation_spacing();
}

/* ---- text: the game's only text-drawing routine; record what it draws (textlayer.c) ---- */
#include "textlayer.h"
void f_101a68c(void) {
    text_call_begin();
    f_101a68c_orig();
    text_call_end();
}

/* ---- intro logos ----
 * The Sports Interactive and Eidos logos (in 0x44C94) wait until a key is pressed, but only check
 * that one is waiting, without reading it. The key was then read by the Options menu that follows,
 * where Enter chose "Quit Program". patches.txt calls this once the logos are done, so a key
 * still skips them but doesn't carry on into the menu. */
void input_flush_keys(void);
void hook_intro_done(void) { input_flush_keys(); }

/* ---- starting year (CM2 database: careers starting in 1995) ----
 * A new game normally starts on Wednesday 16 July 1997. patches.txt takes the year and weekday from
 * here, and makes the engine's "first season" rules compare with the year the career began (byte
 * 0xA44976, written once at new game and saved with the game) instead of with 1997. */
#define CAREER_START_YEAR A(0xA44976)
#include "database.h"                   /* the chosen database (databases/<folder>) gives the start year */
int cm_start_year(void);
static int start_year(void) { return cm_start_year(); }
int cm_start_year(void) {
    const char *e = getenv("CM_START_YEAR");
    int y = e ? atoi(e) : db_start_year();
    return y >= 1980 && y <= 2030 ? y : 1997;
}
uint32_t start_year_byte(void) { return (uint32_t)(start_year() - 1900); }
static int env_year(void) { return getenv("CM_START_YEAR") != NULL; }      /* testing: 16 July of that year */
uint32_t start_day(void)   { return env_year() ? 16 : (uint32_t)db_start_day(); }
uint32_t start_month(void) { return env_year() ? 7 : (uint32_t)db_start_month(); }
uint32_t start_weekday(void) {          /* of the start date; 1 = Sunday .. 7 = Saturday (Zeller) */
    int y = start_year(), m = (int)start_month(), q = (int)start_day();
    if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100, h = (q + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;   /* 0 = Saturday */
    return (uint32_t)((h + 6) % 7 + 1);
}
uint32_t first_season_year(void) {
    uint32_t y = RD8(CAREER_START_YEAR);
    return y >= 80 && y <= 130 ? y : 0x61;   /* not set yet: as before */
}

/* "Change Database" on the Options screen: a panel listing the databases (folders under databases/,
   database.c) to choose from.
   Saved in cm9798.cfg as database = <folder>. */
void host_set_database(int start_year);       /* sdl.c / headless.c: remember the choice */
/* the game read the slot list (GAMESS16.IDX: "Manager", "Unused Game #2", ...) when the front end
   started; read it again (another database's saves folder, or a restored backup). 0x36704 is the
   routine the front end calls for that (at 0x44DB1); it makes the file if there isn't one yet. */
static void reload_slot_list(void) {
    uint32_t r[8] = { eax, ebx, ecx, edx, esi, edi, ebp, esp }; uint32_t saved_ret = g_ret;
    PUSH32(0x1044DB6u);                                 /* its genuine return site */
    f_1036704();
    eax = r[0]; ebx = r[1]; ecx = r[2]; edx = r[3]; esi = r[4]; edi = r[5]; ebp = r[6]; esp = r[7];
    g_ret = saved_ret;
}
int host_choose_database(void);               /* sdl.c: the Choose Database panel; headless.c. 1 = changed */
void hook_change_database(void) {
    if (!host_choose_database()) return;
    host_set_database(db_start_year());
    reload_slot_list();
    if (getenv("CM_START_YEAR")) fprintf(stderr, "note: CM_START_YEAR overrides the database choice\n");
}

/* ---- competition histories for careers starting before 1997 (patches.txt) ----
 * Competition i's record is at 0x1172D22 + i * 0x7D4 in the original (0x4070000 + 0x1A + i * 0x7D4 since relocations.txt) (count: word at 0x1A3AF2C); its history is
 * 8-byte entries from +0x5BA, newest first: year of the final (97 = May 1997), then the winner
 * (+3) and runner-up (+5) as club numbers; an entry with year 0 ends the list. */
#define COMP_BASE   (0x04070000u + 0x1A)                 /* 0x1172D22 before relocations.txt moved them */
#define COMP_SIZE   0x7D4
#define COMP_COUNT  A(0xA3AF2C)
#define HIST_OFF    0x5BA
#define HIST_MAX    ((COMP_SIZE - HIST_OFF) / 8)
void hook_trim_histories(void) {
    int last = start_year() - 1900;                     /* the latest final there can be: May of the start year */
    if (last >= 97) return;                              /* 97/98: the lists end there anyway */
    int n = (int)(int16_t)RD16(COMP_COUNT), removed = 0;
    for (int c = 0; c < n && c < 64; c++) {
        uint32_t h = COMP_BASE + (uint32_t)c * COMP_SIZE + HIST_OFF;
        int k = 0, w = 0;
        for (; k < HIST_MAX; k++) {
            uint8_t y = RD8(h + (uint32_t)k * 8);
            if (!y) break;
            if (y > last) { removed++; continue; }
            if (w != k) for (int b = 0; b < 8; b++) WR8(h + (uint32_t)w * 8 + (uint32_t)b, RD8(h + (uint32_t)k * 8 + (uint32_t)b));
            w++;
        }
        for (; w < k; w++) for (int b = 0; b < 8; b++) WR8(h + (uint32_t)w * 8 + (uint32_t)b, 0);
        if (getenv("CM_HISTLOG")) {
            fprintf(stderr, "[hist] comp %d:", c);
            for (int e = 0; e < 4 && RD8(h + (uint32_t)e * 8); e++)
                fprintf(stderr, " %d:%d/%d", RD8(h + (uint32_t)e * 8), (int16_t)RD16(h + (uint32_t)e * 8 + 3), (int16_t)RD16(h + (uint32_t)e * 8 + 5));
            fprintf(stderr, "\n");
        }
    }
    fprintf(stderr, "histories: %d results after May %d taken out (career starts %d)\n", removed, 1900 + last, start_year());
}

/* ---- Options screen: "Restore Backup" (patches.txt) ----
 * The menu's text ends "Delete Game*5|Change Directory*6|Hall Of Fame*7" (0xE7588); the version with
 * the extra box is longer than that space, so it lives in free conventional memory, between the
 * DOS environment (0x21000) and the DOS memory the game can allocate (from 0x30000). */
#define OPTIONS_MENU_TAIL 0x00028000u
uint32_t options_menu_tail(void) {
    /* with the Database Editor (rt/editor.c): New Game, Restore Game, Delete Game, Restore Backup over
       Change Database, Database Editor, Hall Of Fame (seven boxes: the game makes it four and three) */
    static const char with_editor[] = "Delete Game*5|Restore Backup*8|Change Database*6|Database Editor*9|Hall Of Fame*7";
    static const char without[] = "Delete Game*5|Change Database*6|Restore Backup*8|Hall Of Fame*7";
    if (editor_available()) memcpy(&M[OPTIONS_MENU_TAIL], with_editor, sizeof with_editor);
    else memcpy(&M[OPTIONS_MENU_TAIL], without, sizeof without);
    return OPTIONS_MENU_TAIL;
}
void host_restore_backup(void);                  /* sdl.c: the Restore Backup panel; headless.c */
void host_database_editor(void);                 /* the Database Editor (CM2ED); editor.c */
void hook_options_choice(void) {                 /* after the menu's "is it Hall Of Fame?" test */
    if ((int32_t)eax == 9) { host_database_editor(); return; }
    if ((int32_t)eax != 8) return;
    host_restore_backup();
    reload_slot_list();                          /* names and empty slots may have changed */
}

/* How many leagues a new game may run (patches.txt 045817). The 16MB build's own limit is 3, and the
 * game's tables stop at 5 (6 gets stuck on the first day, 7 or more runs out of league-table slots:
 * "alloctabs 1"). With relocations.txt's bigger tables, calendar, fixtures, competitions, club and player
 * arrays and match ids, all 9 have been played through two seasons, so a new game may always choose up
 * to 9 (fewer is the player's choice on the league screen). CM_LEAGUES sets another limit for testing. */
uint32_t max_leagues(void) {
    const char *e = getenv("CM_LEAGUES");
    if (e) return (uint32_t)(atoi(e) < 1 ? 1 : atoi(e) > 9 ? 9 : atoi(e));
    return 9u;
}

/* The league screen's Done (patches.txt 045a33, 045a88): the chosen leagues' tables must fit the
 * game's 80 (see patches.txt). The choices are flags at esp+0x1e0, in the screen's order: English,
 * Italian, Spanish, German, French, Scottish, Dutch, Portuguese, Belgian. */
#define LEAGUE_TABLES_FREE 91                 /* 127 (relocations.txt), less the European competitions' 36 */
/* Competitions, too: 13 always (European and international), then each league's divisions and cups
 * (all nine: 81). The game had room for 50 - competition records, the calendar, the clubs' byte for
 * each - and has 128 now (relocations.txt); a competition's number is a signed byte. */
#define COMPETITIONS_MAX 127
static int leagues_tables_needed, leagues_comps_needed;   /* the last choice's, when it didn't fit; else 0 */
void hook_check_leagues(void) {
    static const int tables[9] = { 20, 2, 2, 2, 2, 4, 18, 2, 3 };
    static const int comps[9]  = { 11, 8, 6, 6, 6, 10, 7, 7, 7 };
    leagues_tables_needed = leagues_comps_needed = 0;
    if ((eax & 0xFF) == 0) return;            /* the game's own check already said no */
    int need = 0, nc = 13;
    for (int i = 0; i < 9; i++) if (RD8(esp + 0x1e0 + i)) { need += tables[i]; nc += comps[i]; }
    if (need > LEAGUE_TABLES_FREE) { leagues_tables_needed = need; eax &= ~0xFFu; }
    else if (nc > COMPETITIONS_MAX) { leagues_comps_needed = nc; eax &= ~0xFFu; }
}
void hook_leagues_message(void) {
    if (!leagues_tables_needed && !leagues_comps_needed) return;
    char t[300];
    if (leagues_tables_needed)
        snprintf(t, sizeof t, "These leagues need %d league tables; there is room for %d. England needs 20, Holland 18, "
                 "Scotland 4, Belgium 3, the rest 2.", leagues_tables_needed, LEAGUE_TABLES_FREE);   /* (no longer than the game's own) */
    else
        snprintf(t, sizeof t, "These leagues have %d competitions; the game has room for %d. England has 11, Scotland 10, "
                 "Italy 8, the rest 6 or 7.", leagues_comps_needed, COMPETITIONS_MAX);
    leagues_comps_needed = 0;
    uint32_t dst = RD32(esp);                 /* sprintf's buffer (its arguments are still on the stack) */
    for (size_t i = 0; i <= strlen(t); i++) WR8(dst + (uint32_t)i, (uint8_t)t[i]);
    leagues_tables_needed = 0;
}

/* ---- league tables beyond the original 80 (relocations.txt, patches.txt 037bac / 038275) ----
 * The tables are at TABLES_BASE now, 127 of them. CPDATA has the first 80 as the game always wrote
 * them (0x20670 bytes, the last thing in the file); the rest follow, written and read with the
 * game's own wrappers on its open file: 0x1e028 writes (save_cpdata, 0x3818c), 0x1dfdc reads
 * (load_cpdata, 0x37238). A save from before, or from the DOS game, has none: they're left empty. */
#define TABLES_BASE  0x04000000u
#define TABLE_SIZE   0x67Bu
#define TABLES_OLD   80u
#define TABLES_NOW   127u
#define MORE_TABLES  (TABLES_BASE + 2u + TABLES_OLD * TABLE_SIZE)   /* (table 0 is at TABLES_BASE + 2) */
#define MORE_BYTES   ((TABLES_NOW - TABLES_OLD) * TABLE_SIZE)
/* call the game's fread / fwrite wrapper: (buffer, size, count 1, its FILE); returns the count done */
static uint32_t guest_file_io(void (*fn)(void), uint32_t ret_site, uint32_t buf, uint32_t size, uint32_t file) {
    uint32_t r[8] = { eax, ebx, ecx, edx, esi, edi, ebp, esp }; uint32_t saved_ret = g_ret;
    eax = buf; edx = size; ebx = 1; ecx = file;
    PUSH32(ret_site);                                   /* (the return site of the call it follows) */
    fn();
    uint32_t got = eax;
    eax = r[0]; ebx = r[1]; ecx = r[2]; edx = r[3]; esi = r[4]; edi = r[5]; ebp = r[6]; esp = r[7];
    g_ret = saved_ret;
    return got;
}
#define GAME_FREAD  f_101dfdc
#define GAME_FWRITE f_101e028
void hook_save_more_tables(void) { guest_file_io(GAME_FWRITE, 0x103827Au, MORE_TABLES, MORE_BYTES, esi); }
/* eax: what the game's own fread of the 80 returned - 1 if they were there. If they weren't (a file
   that ends sooner), memory is left as it is, as the game leaves the first 80. */
void hook_load_more_tables(void) {
    if (eax != 1) return;
    memset(M + MORE_TABLES, 0, MORE_BYTES);                       /* a save from before: none */
    guest_file_io(GAME_FREAD, 0x1037BB1u, MORE_TABLES, MORE_BYTES, ebp);
}

/* ---- human managers 17-32 (relocations.txt: the table of human managers, 0x11FB bytes each, moved to
 * HUMANS for 32) ----
 * MGDATA ends with the game's own 16 (0x11FB0 bytes, read and written by the game, now from HUMANS); the
 * other 16 are written after them. A save from before has none: they are left empty. */
#define HUMANS        0x05700000u
#define HUMAN_SIZE    0x11FBu
#define HUMANS_OLD    16u
#define HUMANS_EXTRA  (HUMANS + HUMANS_OLD * HUMAN_SIZE)
#define HUMANS_XBYTES (16u * HUMAN_SIZE)
#define HUMANS_READ   0x05730000u                /* the extra 16 as read, before they are kept */
void hook_save_humans(void) { guest_file_io(GAME_FWRITE, 0x103BC01u, HUMANS_EXTRA, HUMANS_XBYTES, esi); }
void hook_load_humans(void) {                    /* eax: the game's read of the first 16 (1 = there) */
    if (eax != 1) return;
    memset(M + HUMANS_EXTRA, 0, HUMANS_XBYTES);
    memset(M + HUMANS_READ, 0, HUMANS_XBYTES);
    if (guest_file_io(GAME_FREAD, 0x103BAE5u, HUMANS_READ, HUMANS_XBYTES, RD32(esp + 0x2FCu)) == 1)
        memcpy(M + HUMANS_EXTRA, M + HUMANS_READ, HUMANS_XBYTES);
}

/* ---- the competition calendar, for 128 competitions (relocations.txt, patches.txt 036e9d...) ----
 * [32 days][13 months][128 competitions] at CAL_BASE; the game's saves have [32][13][50]. */
#define CAL_BASE      0x04040000u
#define CAL_COMPS     128u
#define CAL_SIZE      (32u * 13u * CAL_COMPS)
#define CAL_OLD_COMPS 50u
#define CAL_OLD_SIZE  (32u * 13u * CAL_OLD_COMPS)
#define CAL_OLD_COPY  0x04050000u               /* the old layout, for the save file's own block */
#define CAL_READ      0x04060000u               /* the whole calendar, as read from the end of a save */
static void calendar_convert(uint32_t to, uint32_t to_comps, uint32_t from, uint32_t from_comps) {
    uint32_t n = to_comps < from_comps ? to_comps : from_comps;
    for (uint32_t d = 0; d < 32; d++) for (uint32_t m = 0; m < 13; m++)
        memcpy(M + to + (d * 13 + m) * to_comps, M + from + (d * 13 + m) * from_comps, n);
}
uint32_t calendar_pack(void) {
    memset(M + CAL_OLD_COPY, 0, CAL_OLD_SIZE);
    calendar_convert(CAL_OLD_COPY, CAL_OLD_COMPS, CAL_BASE, CAL_COMPS);
    return CAL_OLD_COPY;
}
/* ---- fixtures, 250 a date (relocations.txt; patches.txt 036e87...) ----
 * 300 date blocks: FIX_PER fixtures of 14 bytes, then the date (3 bytes) and the count (word). The
 * game's saves have 120 a date in blocks of 0x695, 0x7b69c bytes of them (the last block's footer
 * isn't saved: the next table starts there). */
#define FIX_BASE      0x04100000u
#define FIX_DAYS      300u
#define FIX_PER       250u
#define FIX_STRIDE    (FIX_PER * 14u + 5u)
#define FIX_SIZE      (FIX_DAYS * FIX_STRIDE)
#define FIX_OLD_PER   120u
#define FIX_OLD_STRIDE 0x695u
#define FIX_OLD_SAVED 0x7B69Cu
#define FIX_OLD_COPY  0x04300000u               /* the old layout, for the save file's own block */
#define FIX_READ      0x04400000u               /* all of them, as read from the end of a save */
static void fixtures_convert(uint32_t to, uint32_t to_per, uint32_t to_stride, uint32_t to_limit,
                             uint32_t from, uint32_t from_per, uint32_t from_stride, uint32_t from_limit) {
    for (uint32_t d = 0; d < FIX_DAYS; d++) {
        uint32_t f = from + d * from_stride, t = to + d * to_stride;
        if (f + from_per * 14 + 5 > from + from_limit || t + to_per * 14 + 5 > to + to_limit) break;
        uint32_t n = RD16(f + from_per * 14 + 3); if ((int16_t)n < 0) n = 0;
        if (n > to_per) n = to_per;
        if (n > from_per) n = from_per;
        memcpy(M + t, M + f, n * 14);
        memcpy(M + t + to_per * 14, M + f + from_per * 14, 3);       /* the date */
        WR16(t + to_per * 14 + 3, n);
    }
}
uint32_t fixtures_pack(void) {
    memset(M + FIX_OLD_COPY, 0, FIX_DAYS * FIX_OLD_STRIDE);
    fixtures_convert(FIX_OLD_COPY, FIX_OLD_PER, FIX_OLD_STRIDE, FIX_DAYS * FIX_OLD_STRIDE, FIX_BASE, FIX_PER, FIX_STRIDE, FIX_SIZE);
    return FIX_OLD_COPY;
}
uint32_t fixtures_old_buffer(void) { memset(M + FIX_OLD_COPY, 0, FIX_DAYS * FIX_OLD_STRIDE); return FIX_OLD_COPY; }
static int fx_loaded;                            /* the game's read of the old block worked */
void hook_load_fixtures(void) {                  /* (eax: that read's result - see hook_load_more_tables) */
    fx_loaded = eax == 1;
    if (!fx_loaded) return;
    memset(M + FIX_BASE, 0, FIX_SIZE);
    fixtures_convert(FIX_BASE, FIX_PER, FIX_STRIDE, FIX_SIZE, FIX_OLD_COPY, FIX_OLD_PER, FIX_OLD_STRIDE, FIX_OLD_SAVED);
}

void hook_save_calendar(void) {                  /* the end of FXDATA (0x36ec4): the whole calendar, then all the fixtures */
    guest_file_io(GAME_FWRITE, 0x1036F75u, CAL_BASE, CAL_SIZE, esi);
    guest_file_io(GAME_FWRITE, 0x1036F75u, FIX_BASE, FIX_SIZE, esi);
}
uint32_t calendar_old_buffer(void) { memset(M + CAL_OLD_COPY, 0, CAL_OLD_SIZE); return CAL_OLD_COPY; }
void hook_load_calendar(void) {
    if (eax != 1) return;                        /* (as hook_load_more_tables) */
    memset(M + CAL_BASE, 0, CAL_SIZE);
    calendar_convert(CAL_BASE, CAL_COMPS, CAL_OLD_COPY, CAL_OLD_COMPS);
    memset(M + CAL_READ, 0, CAL_SIZE);
    if (guest_file_io(GAME_FREAD, 0x1036EA9u, CAL_READ, CAL_SIZE, esi) == 1) memcpy(M + CAL_BASE, M + CAL_READ, CAL_SIZE);
    memset(M + FIX_READ, 0, FIX_SIZE);
    if (fx_loaded && guest_file_io(GAME_FREAD, 0x1036EA9u, FIX_READ, FIX_SIZE, esi) == 1) memcpy(M + FIX_BASE, M + FIX_READ, FIX_SIZE);
}

/* ---- the clubs' byte for each competition, 128 of them (relocations.txt, patches.txt 038294...) ----
 * Club c's byte for competition k is at CLUBCOMP_BASE + c * 0x5BA + k (the game's own was in the
 * club record, +0x9E, for 50 competitions). TMDATA keeps those 50 in the records too, and ends with
 * "CLUBCOMP", the number of clubs (4 bytes), then 128 bytes a club. */
#define CLUBCOMP_BASE   0x04600000u
#define CLUB_STRIDE     0x5BAu
#define CLUB_REC_ARRAY  0x01585306u                  /* club 0's record, +0x9E */
#define CLUB_COUNT      0x01A3AF68u                  /* word: how many clubs there are */
#define CLUBCOMP_OLD    50u
#define CLUBCOMP_NOW    128u
#define CLUBS_MAX       2400u
#define CLUBCOMP_FILE   0x04F00000u                  /* the block as it's written: header, then the bytes */
static uint32_t club_count(void) { int n = (int16_t)RD16(CLUB_COUNT); return n < 0 ? 0 : (uint32_t)n > CLUBS_MAX ? CLUBS_MAX : (uint32_t)n; }
void hook_load_tmdata_begin(void) {               /* al = 0: a new game, the clubs coming from the database */
    if ((eax & 0xFF) == 0) memset(M + CLUBCOMP_BASE, 0, CLUBS_MAX * CLUB_STRIDE);
}
void hook_save_tmdata_begin(void) {
    for (uint32_t c = 0, n = club_count(); c < n; c++)
        memcpy(M + CLUB_REC_ARRAY + c * CLUB_STRIDE, M + CLUBCOMP_BASE + c * CLUB_STRIDE, CLUBCOMP_OLD);
}
void hook_save_club_comps(void) {
    uint32_t n = club_count();
    memcpy(M + CLUBCOMP_FILE, "CLUBCOMP", 8); WR32(CLUBCOMP_FILE + 8, n);
    for (uint32_t c = 0; c < n; c++) memcpy(M + CLUBCOMP_FILE + 12 + c * CLUBCOMP_NOW, M + CLUBCOMP_BASE + c * CLUB_STRIDE, CLUBCOMP_NOW);
    guest_file_io(GAME_FWRITE, 0x1039A6Cu, CLUBCOMP_FILE, 12 + n * CLUBCOMP_NOW, esi);
}
void hook_load_club_comps(void) {                  /* after the last read of a saved game's TMDATA */
    uint32_t n = club_count();
    memset(M + CLUBCOMP_FILE, 0, 12);
    if (eax == 1 && guest_file_io(GAME_FREAD, 0x1039410u, CLUBCOMP_FILE, 12, esi) == 1 &&
        !memcmp(M + CLUBCOMP_FILE, "CLUBCOMP", 8) && RD32(CLUBCOMP_FILE + 8) == n &&
        guest_file_io(GAME_FREAD, 0x1039410u, CLUBCOMP_FILE + 12, n * CLUBCOMP_NOW, esi) == 1) {
        for (uint32_t c = 0; c < n; c++) memcpy(M + CLUBCOMP_BASE + c * CLUB_STRIDE, M + CLUBCOMP_FILE + 12 + c * CLUBCOMP_NOW, CLUBCOMP_NOW);
        return;
    }
    for (uint32_t c = 0; c < n; c++) {             /* a save from before: the 50 in the club records */
        memset(M + CLUBCOMP_BASE + c * CLUB_STRIDE, 0, CLUBCOMP_NOW);
        memcpy(M + CLUBCOMP_BASE + c * CLUB_STRIDE, M + CLUB_REC_ARRAY + c * CLUB_STRIDE, CLUBCOMP_OLD);
    }
}

/* ---- Competition Info with more than eight leagues ----
   The screen (0x10109d8) builds its button list at 0x18bf5b0: two European entries, one per league run
   (flags at 0x1a448fb, nine of them, strings from 0x10e1f00), then "Monthly Awards*11|Annual Awards*12|"
   (0x10e1fdc), and the menu builder (0x10141f0) has room for 12. With more than eight leagues the countries
   move into a second list behind "Domestic Competitions*13|": choosing it (an id the dispatch ignores, so the
   screen is built again) shows the countries, and Done there comes back to the first list. */
static int ci_domestic;
static const uint32_t ci_country_str[9] = { 0x10e1f00, 0x10e1f18, 0x10e1f30, 0x10e1f48, 0x10e1f60,
                                            0x10e1f78, 0x10e1f90, 0x10e1fa8, 0x10e1fc0 };
static void guest_strcat(uint32_t dst, const char *s) {
    while (RD8(dst)) dst++;
    for (; *s; s++) WR8(dst++, (uint8_t)*s);
    WR8(dst, 0);
}
static void guest_strcat_guest(uint32_t dst, uint32_t src) {
    char t[64]; size_t n = 0;
    while (n < sizeof t - 1 && RD8(src + (uint32_t)n)) { t[n] = (char)RD8(src + (uint32_t)n); n++; }
    t[n] = 0; guest_strcat(dst, t);
}
uint32_t competition_menu_tail(void) {
    const uint32_t buf = 0x18bf5b0u, awards = 0x10e1fdcu;
    int n = 0;
    for (int k = 0; k < 9; k++) if (RD8(0x1a448fbu + (uint32_t)k)) n++;
    if (n <= 8) { ci_domestic = 0; return awards; }
    WR8(buf, 0);
    if (ci_domestic) {
        for (int k = 0; k < 9; k++) if (RD8(0x1a448fbu + (uint32_t)k)) guest_strcat_guest(buf, ci_country_str[k]);
    } else {
        guest_strcat(buf, "International Competition*0|European Competition*1|Domestic Competitions*13|");
        guest_strcat_guest(buf, awards);
    }
    uint32_t end = buf; while (RD8(end)) end++;
    return end;                                /* the game appends this: an empty string */
}
void hook_competition_choice(void) {
    int8_t c = (int8_t)(eax & 0xFF);
    if (c == 13) { ci_domestic = 1; return; }
    if (ci_domestic && c == 0x63) {           /* Done in the countries: back to the first list */
        ci_domestic = 0;
        eax = (eax & ~0xFFu) | 13u; edx = 13; WR8(esp + 0x10, 13);
    }
}

/* ---- Player Search & Transfers with nine leagues ----
   The screen (0x1012194) builds its list on the stack: "Player Search*0|View Shortlist*1|Latest Transfers*2|"
   (0x10e2694), one "<country> Transfers*3..10|" per country run (English and Scottish share "British"),
   then "Other Transfers*11|Players Released*12|" (0x10e2784). Nine leagues make 13 buttons, one more than
   the menu builder (0x10141f0) takes, and the screen came up empty. Then the countries move into a second
   list behind "Transfers by Country*13|", as on Competition Info. */
static int tr_country;
void hook_transfers_enter(void) { tr_country = 0; }
uint32_t transfers_menu_tail(void) {
    const uint32_t buf = esp, tail = 0x10e2784u;
    int n = 0; for (uint32_t a = buf; RD8(a); a++) if (RD8(a) == '|') n++;
    if (n + 2 <= 12) { tr_country = 0; return tail; }     /* the original list fits */
    char t[400]; size_t k = 0;
    for (uint32_t a = buf; RD8(a) && k < sizeof t - 1; a++) t[k++] = (char)RD8(a);
    t[k] = 0;
    const char *countries = t;                             /* after the third entry */
    for (int bars = 0; *countries && bars < 3; countries++) if (*countries == '|') bars++;
    char out[400];
    if (tr_country) snprintf(out, sizeof out, "%s", countries);
    else {
        char first[200]; snprintf(first, sizeof first, "%.*s", (int)(countries - t), t);
        snprintf(out, sizeof out, "%sTransfers by Country*13|", first);
    }
    WR8(buf, 0); guest_strcat(buf, out);
    if (tr_country) { uint32_t end = tail; while (RD8(end)) end++; return end; }   /* "": no Other/Released */
    return tail;                                           /* the game appends Other Transfers, Players Released */
}
void hook_transfers_choice(void) {                         /* after the choice is stored at 0x190(%esp) */
    int8_t c = (int8_t)(eax & 0xFF);
    if (c == 13) { tr_country = 1; return; }
    if (tr_country && c == 0x63) {                         /* Done in the countries: back to the first list */
        tr_country = 0;
        eax = (eax & ~0xFFu) | 13u; edx = 13; WR8(esp + 0x190, 13);
    }
}

/* ---- Select Team with more than ten pages ----
   0x1075b2f pushes the number of buttons on the bottom bar (Done and one per page) on top of eleven
   widths already pushed (Done's 3, ten pages' 1). With more than ten pages the layout routine read
   widths past them. In its place: as many widths as buttons, and the matching pop after the call. */
static uint32_t st_extra;                                   /* bytes pushed beyond the original 0x30 */
void hook_select_team_layout_args(void) {                   /* for "push %eax" */
    uint32_t n = eax;
    if ((int32_t)n <= 11) { st_extra = 0; esp -= 4; WR32(esp, n); return; }
    if (n > 40) n = 40;                                     /* the button tables hold 50 */
    st_extra = 4 * (n - 11);
    esp -= 4 + st_extra;                                    /* the block now ends where the original did */
    WR32(esp, n); WR32(esp + 4, 3);
    for (uint32_t k = 0; k < n - 1; k++) WR32(esp + 8 + 4 * k, 1);
}
void hook_select_team_layout_pop(void) { esp += 0x30 + st_extra; st_extra = 0; }
