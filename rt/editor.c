/* editor.c - the Database Editor: Championship Manager 2's own data editor (CM2ED.EXE, v2.00, which
 * came with CM 97/98), recompiled like the game and run inside it, from the Options screen.
 *
 * The editor and the game were both built to run at the same addresses, so each keeps its own
 * state: guest memory, registers and descriptors (rt.c), DOS (dos.c: files, memory, vectors,
 * palette), the mouse driver settings (input.c) and the screen (video.c). Choosing Database Editor
 * puts the game's state aside, starts the editor from scratch with its own (the state the program
 * started with, saved at start-up), and when the editor quits (int 21h 4Ch) puts the game back as
 * it was, so the Options screen carries on. Its code is gen/'s ed_*.c (cmgen, from the player's
 * CM2ED.EXE), with its own names: e_XXXXXX and e_funcs (rt/editor_none.c when there is no editor).
 *
 * Files: the editor works in the chosen database's folder (databases/<folder>/: players.db1 and the
 * rest), with nothing redirected; what isn't there (its pictures and fonts) comes from the game
 * folder. The first change to each file in a session is preceded by a copy of it, in
 * databases/<folder>/backups/<date>_<time>/. Edits are used by new games, as CM2ED's help says.
 *
 * editor/patches.txt changes a few things, with the hooks below: sharp text (as in the game), one
 * click per action, "Back to main menu" in place of "Exit program", a line saying which database
 * is being edited, Load Database as a choice of Players, Managers or Teams, and Save Database as a
 * Yes/No "Save all changes?" to the file that was loaded.
 */
#include "rt_internal.h"
#include "database.h"
#include "textlayer.h"
#include "dbcheck.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <time.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

extern const struct gfunc e_funcs[]; extern const unsigned e_nfuncs;       /* gen/edtable.c (or editor_none.c) */
extern const uint32_t e_retsites[]; extern const unsigned e_nretsites;     /* gen/edretsites.c */
#define ED_ENTRY     0x010254C4u     /* _cstart_ (0x0254C4) */
#define ED_BUTTONS   0x0103AAD2u     /* word: mouse buttons down, kept by its mouse handler (0x1A590) */
#define ED_CANCELLED 0x010A89AFu     /* byte: the last text box was left with Escape */
#define ED_TEXT_W    0x0101FF88u     /* width of a string: AL size, EDX string (a call returns to 0x102089D) */
#define ED_TEXT      0x0101FFD0u     /* draw a string: EAX colour, DL size, EBX x, ECX y, the string pushed (a call returns to 0x10209F9) */
#define ED_STRINGS   0x00028000u     /* our texts, in free conventional memory (below the DOS memory at 0x30000) */
#define ED_STACK_TOP 0x0114A850u     /* the LE header's stack: object 2 + 0x11A850 */

extern const char *g_game_dir;
extern void dos_setup(const char *dir);
extern int rt_load_exe(const char *path);
void dos_plain_files(const char *dir, const char *alt);
extern void (*g_dos_exit)(int code);
void fpu_setcw(void);
int host_pick_database(void);           /* sdl.c / headless.c: which database to edit, -1 = cancelled */
void host_message(const char *title, const char *text);

int g_editor_running;                   /* the editor has the screen (sdl.c: no game overlays, no OS pointer) */
static char edit_dir[1200];             /* the database's folder it works in */
static char db_title[160];              /* "Editing <name> Database" */
void host_wait(void); void input_dispatch_mouse(void); uint32_t host_ms(void);
int host_pick_list(const char *title, const char *choose, const char *head1, const char *head2,
                   const char *const *col1, const char *const *col2, int n, int sel);   /* sdl.c / headless.c */

/* ---- hooks (editor/patches.txt) ---- */
/* one click, one action: a press that has chosen something in a menu chooses nothing else */
unsigned input_press_serial(void);
static unsigned used_press = ~0u;
void hook_ed_menu_begin(void) {
    if ((RD16(ED_BUTTONS) & 3) && input_press_serial() == used_press) WR16(ED_BUTTONS, RD16(ED_BUTTONS) & ~3u);
}
void hook_ed_menu_end(void) {
    if ((int8_t)(eax & 0xFF) != -1 && (RD16(ED_BUTTONS) & 3)) used_press = input_press_serial();
}
/* a text box is starting: wait for the click that opened it to be let go */
void hook_ed_fresh_click(void) {
    uint32_t t0 = host_ms();
    while ((RD16(ED_BUTTONS) & 3) && host_ms() - t0 < 3000) { host_wait(); input_dispatch_mouse(); }
}
/* the texts: in free memory (they're longer than the originals), written when the editor starts */
enum { T_BACK = 0, T_BACK_Q = 0x40, T_BACK_TITLE = 0x80, T_FILE = 0xC0, T_CAPTION = 0x100, T_SAVE_Q = 0x200, T_MENU = 0x240 };
#define ED_LOADED_NAME 0x01081328u   /* the name of the file that was loaded (Save's name box started with it) */
static void put_text(uint32_t at, const char *s) { size_t n = strlen(s); memcpy(&M[ED_STRINGS + at], s, n + 1); }
uint32_t ed_text_back(void) { return ED_STRINGS + T_BACK; }
uint32_t ed_text_back_question(void) { return ED_STRINGS + T_BACK_Q; }
uint32_t ed_text_back_title(void) { return ED_STRINGS + T_BACK_TITLE; }
uint32_t ed_text_save_question(void) { return ED_STRINGS + T_SAVE_Q; }
uint32_t ed_text_menu(void) { return ED_STRINGS + T_MENU; }
/* Test Database (choice 3 on the main menu): the saved files, checked the way a new game reads them,
   against the game folder's original 97/98 files */
static char ed_leagues[160];
void host_report(const char *title, char **lines, int n);    /* sdl.c / headless.c */
void hook_ed_menu_choice(void) {
    if ((int32_t)edx != 3) return;
    char **lines; int n;
    int problems = dbcheck_run(edit_dir, strcmp(edit_dir, g_game_dir) ? g_game_dir : NULL, ed_leagues, &lines, &n);
    fprintf(stderr, "editor: Test Database: %d problem%s\n", problems, problems == 1 ? "" : "s");
    host_report("Test Database", lines, n);
}
/* Save Database: the file that was loaded, with no box to type it in */
uint32_t ed_save_name(void) { WR8(ED_CANCELLED, 0); return ED_LOADED_NAME; }
/* call one of the editor's functions: EAX, EDX, EBX, ECX, then one value on the stack (or none);
   ret is the return site of a real call to it. Every register is kept. */
static uint32_t call_editor(uint32_t fn, uint32_t ret, uint32_t a, uint32_t d, uint32_t b, uint32_t c, int push, uint32_t st) {
    gfn f = rt_lookup(fn); if (!f) return 0;
    uint32_t r[8] = { eax, ebx, ecx, edx, esi, edi, ebp, esp }, saved_ret = g_ret;
    if (push) PUSH32(st);
    eax = a; edx = d; ebx = b; ecx = c;
    PUSH32(ret); f();
    uint32_t v = eax;
    eax = r[0]; ebx = r[1]; ecx = r[2]; edx = r[3]; esi = r[4]; edi = r[5]; ebp = r[6]; esp = r[7]; g_ret = saved_ret;
    return v;
}
/* the main menu, once drawn: which database this is, above the bottom button, in its own lettering */
#define CAPTION_SIZE   5               /* the editor's size 5 (its font 2) */
#define CAPTION_COLOUR 0x82
#define CAPTION_SHADOW 0x00
void hook_ed_caption(void) {
    int w = (int)(int16_t)call_editor(ED_TEXT_W, 0x0102089Du, CAPTION_SIZE, ED_STRINGS + T_CAPTION, 0, 0, 0, 0);
    int x = 320 - w / 2, y = 419;
    call_editor(ED_TEXT, 0x010209F9u, CAPTION_SHADOW, CAPTION_SIZE, (uint32_t)(x + 1), (uint32_t)(y + 1), 1, ED_STRINGS + T_CAPTION);
    call_editor(ED_TEXT, 0x010209F9u, CAPTION_COLOUR, CAPTION_SIZE, (uint32_t)x, (uint32_t)y, 1, ED_STRINGS + T_CAPTION);
}
/* Load Database: which file, from a list */
uint32_t ed_pick_file(void) {
    static const char *const names[] = { "Players", "Managers", "Teams" };
    static const char *const files[] = { "players.db1", "mgdata.db1", "tmdata.db1" };
    static int last;
    int i = host_pick_list("Load Database", "Load", "Data", "File", names, files, 3, last);
    WR8(ED_CANCELLED, i < 0);
    if (i >= 0) { last = i; put_text(T_FILE, files[i]); } else put_text(T_FILE, "");
    return ED_STRINGS + T_FILE;
}

/* ---- the state every program starts with: taken at start-up, before anything has run ---- */
struct part { size_t (*size)(void); void (*swap)(void *); unsigned char *fresh; };
static struct part parts[] = {
    { rt_state_size, rt_state_swap, 0 }, { dos_state_size, dos_state_swap, 0 },
    { input_state_size, input_state_swap, 0 }, { video_state_size, video_state_swap, 0 },
};
#define NPARTS (int)(sizeof parts / sizeof parts[0])
void editor_startup(void) {
    for (int i = 0; i < NPARTS; i++) {
        size_t n = parts[i].size(); unsigned char *b = calloc(1, n);
        if (!b) return;
        parts[i].swap(b);                    /* b: the starting state (the program now has zeros) */
        parts[i].fresh = malloc(n); if (parts[i].fresh) memcpy(parts[i].fresh, b, n);
        parts[i].swap(b);                    /* and back */
        free(b);
    }
}

/* the editor needs its code (CM2ED.EXE made into gen/ed_*.c) and the EXE itself (its data) */
static int find_exe(char *out, size_t n) {
    const char *v[2] = { "CM2ED.EXE", "cm2ed.exe" };
    for (int i = 0; i < 2; i++) { snprintf(out, n, "%s/%s", g_game_dir, v[i]); struct stat st; if (!stat(out, &st)) return 1; }
    return 0;
}
int editor_available(void) { char p[1200]; return e_nfuncs > 0 && parts[0].fresh && find_exe(p, sizeof p); }

/* ---- a copy of each file before the editor first changes it ---- */
static char backup_dir[1400]; static int backup_made;
static char backed_up[32][64]; static int n_backed_up;
static int copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb"); if (!in) return 0;
    FILE *out = fopen(to, "wb"); if (!out) { fclose(in); return 0; }
    static char buf[1 << 16]; size_t k; int ok = 1;
    while ((k = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, k, out) != k) ok = 0;
    fclose(in); if (fclose(out)) ok = 0; return ok;
}
void editor_before_write(const char *host) {
    struct stat st; if (stat(host, &st) || !S_ISREG(st.st_mode)) return;       /* a new file: nothing to keep */
    const char *b = host + strlen(host); while (b > host && b[-1] != '/' && b[-1] != '\\') b--;
    for (int i = 0; i < n_backed_up; i++) if (!strcmp(backed_up[i], b)) return;
    if (n_backed_up < 32) snprintf(backed_up[n_backed_up++], sizeof backed_up[0], "%s", b);
    if (!backup_made) {
        char top[1300]; snprintf(top, sizeof top, "%s/backups", edit_dir); MKDIR(top);
        time_t now = time(NULL); struct tm *t = localtime(&now);
        snprintf(backup_dir, sizeof backup_dir, "%s/%04d-%02d-%02d_%02d%02d%02d", top,
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
        MKDIR(backup_dir); backup_made = 1;
    }
    char to[1600]; snprintf(to, sizeof to, "%s/%s", backup_dir, b);
    fprintf(stderr, "editor: %s %s before it is changed\n", copy_file(host, to) ? "kept a copy of" : "COULDN'T keep a copy of", b);
}

/* ---- running it ---- */
static jmp_buf back_to_game;
static void editor_quit(int code) { (void)code; longjmp(back_to_game, 1); }

void host_database_editor(void) {
    char exe[1200];
    if (!editor_available() || !find_exe(exe, sizeof exe)) return;
    /* which database: its folder (databases/<folder>), or the game folder when there are none */
    if (db_count() > 0) {
        int i = host_pick_database(); const char *f = db_folder(i), *name; int dd, mm, yy;
        if (!f) return;
        snprintf(edit_dir, sizeof edit_dir, "%s/databases/%s", g_game_dir, f);
        if (!db_info(i, &name, &dd, &mm, &yy)) name = f;
        snprintf(db_title, sizeof db_title, "Editing %s%s", name, strstr(name, "atabase") ? "" : " Database");
        snprintf(ed_leagues, sizeof ed_leagues, "%s", db_leagues_of(i));
    } else {
        snprintf(edit_dir, sizeof edit_dir, "%s", g_game_dir);
        snprintf(db_title, sizeof db_title, "Editing the Championship Manager 97/98 Database");
        ed_leagues[0] = 0;
    }
    backup_made = 0; n_backed_up = 0;
    fprintf(stderr, "editor: starting CM2ED in %s\n", edit_dir);

    /* the game's state aside; the editor's from scratch */
    unsigned char *game[NPARTS];
    for (int i = 0; i < NPARTS; i++) {
        size_t n = parts[i].size(); game[i] = malloc(n);
        if (!game[i]) { while (i--) free(game[i]); return; }
        memcpy(game[i], parts[i].fresh, n);
        parts[i].swap(game[i]);              /* the program: fresh; game[i]: the game's */
    }
    text_use_editor(1);                      /* its letters, for sharp text (the game's are forgotten) */
    g_editor_running = 1;
    int ok = 0;
    M = calloc(1, GUEST_MEM_SIZE);
    if (M) {
        rt_set_program(e_funcs, e_nfuncs, e_retsites, e_nretsites);
        fpu_setcw();
        dos_setup(edit_dir);
        dos_plain_files(edit_dir, g_game_dir);
        g_dos_exit = editor_quit;
        if (rt_load_exe(exe) == 0) {
            put_text(T_BACK, "Back to main menu");
            put_text(T_BACK_Q, "Go back to the main menu?");
            put_text(T_BACK_TITLE, "Back to Main Menu");
            put_text(T_CAPTION, db_title);
            put_text(T_SAVE_Q, "Save all changes?");
            put_text(T_MENU, "Load Database|Save Database|Edit Database|Test Database|");
            gfn start = rt_lookup(ED_ENTRY);
            if (start && !setjmp(back_to_game)) {
                esp = ED_STACK_TOP;
                PUSH32(0xFFFFFFF0u);         /* a return to nowhere: the editor ends with int 21h 4Ch */
                start();
            }
            ok = 1;
        }
    }
    /* back to the game */
    if (M) { dos_close_files(); free(M); }
    for (int i = 0; i < NPARTS; i++) { parts[i].swap(game[i]); free(game[i]); }
    fpu_setcw();
    text_use_editor(0);                      /* the game draws its Options screen again */
    g_editor_running = 0;
    fprintf(stderr, "editor: %s, back to the game\n", ok ? "closed" : "couldn't start");
    if (!ok) host_message("Database Editor", "The Database Editor couldn't be started (CM2ED.EXE couldn't be loaded).");
}
