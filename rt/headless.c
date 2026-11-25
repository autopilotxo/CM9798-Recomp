/* headless.c - display/input backend without a window (testing on machines without SDL).
 *
 * CM_SHOTDIR=<dir>   write a PPM whenever the picture has changed (checked every 32 polls)
 * CM_SHOTMAX=<n>     maximum frames to write (default 100)
 * CM_MAXPOLLS=<n>    exit after n input polls
 * CM_REPLAY=<file>   replay a session recorded by the SDL build with CM_RECORD
 * CM_INPUT=<script>  scripted input, ';'-separated steps executed at polls:
 *     wait N | click X Y | rclick X Y | move X Y | down | up | key C | enter | esc | shot
 */
#include "rt_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include "textlayer.h"
#include <ctype.h>

int video_frame(const uint8_t **pixels, int *w, int *h, int *stride, uint8_t pal[256][3]);
void input_key(uint16_t k); void input_mouse_move(int x, int y); void input_mouse_button(int b, int down);
void input_dispatch_mouse(void); int input_key_ready(void);
extern unsigned long g_polls;

static uint64_t last_hash; static int shots;

static uint64_t hash(const uint8_t *p, int w, int h, int stride, uint8_t pal[256][3]) {
    uint64_t x = 1469598103934665603ull;
    for (int y = 0; y < h; y++) for (int i = 0; i < w; i++) { x ^= p[y * stride + i]; x *= 1099511628211ull; }
    for (int i = 0; i < 768; i++) { x ^= ((uint8_t *)pal)[i]; x *= 1099511628211ull; }
    return x;
}
static void present(int force) {
    const char *dir = getenv("CM_SHOTDIR"); if (!dir) return;
    int maxs = getenv("CM_SHOTMAX") ? atoi(getenv("CM_SHOTMAX")) : 100;
    if (shots >= maxs) return;
    const uint8_t *p; int w, h, st; uint8_t pal[256][3];
    if (!video_frame(&p, &w, &h, &st, pal)) return;
    uint64_t hv = hash(p, w, h, st, pal); if (hv == last_hash && !force) return; last_hash = hv;
    char path[1024]; snprintf(path, sizeof path, "%s/frame_%03d.ppm", dir, shots++);
    FILE *f = fopen(path, "wb"); if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) fwrite(pal[p[y * st + x]], 1, 3, f);
    fclose(f);
    fprintf(stderr, "[video] saved %s (poll %lu)\n", path, g_polls);
    if (getenv("CM_TEXTDEBUG")) {          /* development: which letters the sharp-text layer would redraw */
        static int vis[8192]; int n = text_visible(p, st, w, h, vis, 8192);
        extern int g_nglyphs;
        snprintf(path, sizeof path, "%s/text_%03d.ppm", dir, shots - 1);
        static uint8_t rgb[640 * 480 * 3];
        if (w == 640 && h == 480) {
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { const uint8_t *c = pal[p[y * st + x]];
                uint8_t *o = rgb + (y * w + x) * 3; o[0] = o[1] = o[2] = (uint8_t)((c[0] + c[1] + c[2]) / 6); }
            for (int k = 0; k < n; k++) { struct tglyph *g = &g_glyphs[vis[k]];
                for (int y = 0; y < g->h; y++) for (int x = 0; x < g->w; x++) if (text_is_ink(vis[k], x, y)) {
                    int X = g->x + g_text_dx[vis[k]] + x, Y = g->y + g_text_dy[vis[k]] + y; if (X < 0 || X >= w || Y < 0 || Y >= h) continue; uint8_t *o = rgb + (Y * w + X) * 3; o[0] = 255; o[1] = 40; o[2] = 40; } }
            f = fopen(path, "wb"); if (f) { fprintf(f, "P6\n%d %d\n255\n", w, h); fwrite(rgb, 1, sizeof rgb, f); fclose(f); }
        }
        fprintf(stderr, "[text] %d letters recorded, %d still on screen\n", g_nglyphs, n);
        if (getenv("CM_TEXTDEBUG")[0] == '2') for (int i = 0; i < g_nglyphs; i++) { struct tglyph *g = &g_glyphs[i];
            if (g->x >= 150 && g->x < 230 && g->y >= 190 && g->y < 230)
                fprintf(stderr, "  glyph %d '%c' at %d,%d %dx%d font %d colour %d live %d\n", i, g->ch, g->x, g->y, g->w, g->h, g->font, g->colour, g->live); }
    }
}
int guest_pointer(uint32_t argb[11 * 11], int *shape);
static void ptrdump(void) {
    if (getenv("CM_PTRDUMP")) {           /* development: show what the OS pointer would be */
        static int last = -2; uint32_t img[121]; int shape = 0, vis = guest_pointer(img, &shape);
        if (vis * 2 + shape != last) { last = vis * 2 + shape;
            fprintf(stderr, "[pointer] poll %lu: %s", g_polls, vis ? (shape ? "football" : "arrow") : "hidden");
            if (vis) fprintf(stderr, " colours %06x %06x", img[0] & 0xFFFFFF, img[11 * 1 + 1] & 0xFFFFFF);
            fprintf(stderr, "\n"); } }
}
void host_present(void) { present(0); }

/* ---- replay of a recorded session ---- */
static FILE *rep; static unsigned long rep_poll; static char rep_kind[8]; static int rep_a, rep_b, rep_have;
static void replay(void) {
    if (!rep) return;
    for (;;) {
        if (!rep_have) {
            char line[128];
            if (!fgets(line, sizeof line, rep)) { fclose(rep); rep = NULL; fprintf(stderr, "[replay] end of recording at poll %lu\n", g_polls); return; }
            rep_a = rep_b = 0;
            if (sscanf(line, "%lu %7s %d %d", &rep_poll, rep_kind, &rep_a, &rep_b) < 3) continue;
            rep_have = 1;
        }
        if (rep_poll > g_polls) return;
        if (!strcmp(rep_kind, "move")) input_mouse_move(rep_a, rep_b);
        else if (!strcmp(rep_kind, "down")) { input_mouse_button(rep_a, 1); fprintf(stderr, "[replay] poll %lu: down\n", g_polls); }
        else if (!strcmp(rep_kind, "up")) { input_mouse_button(rep_a, 0); fprintf(stderr, "[replay] poll %lu: up\n", g_polls); }
        else if (!strcmp(rep_kind, "key")) { input_key((uint16_t)rep_a); fprintf(stderr, "[replay] poll %lu: key %04x\n", g_polls, rep_a); }
        rep_have = 0;
    }
}

/* ---- input script ---- */
static const char *script; static unsigned long wait_until, release_at; static int pending_release = -1;
static char until_text[96]; static unsigned long until_since;
static int retry_x = -1, retry_y; static unsigned long retry_next;   /* "clickto": click again until the text shows */
static int click_found;                     /* "clickon <text>": wait for the text, then click its middle */
static int press_x = -1, press_y; static unsigned long press_at;   /* a click: move there, then press a moment later */
static void press_later(int x, int y) { input_mouse_move(x, y); press_x = x; press_y = y; press_at = g_polls + 10; }
/* CM_SCREENLOG (testing): the screen's text, a line per row, before each "clickon" click */
static void screen_log(const uint8_t *p, int w, int h, int st) {
    static int idx[4096]; int n = text_visible(p, st, w, h, idx, 4096); if (n <= 0) return;
    for (int i = 1; i < n; i++) for (int j = i; j > 0; j--) {          /* by row, then left to right */
        const struct tglyph *a = &g_glyphs[idx[j - 1]], *b = &g_glyphs[idx[j]];
        int ay = a->y / 8, by = b->y / 8;
        if (ay > by || (ay == by && a->x > b->x)) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; } else break;
    }
    char line[512]; int k = 0, row = -1, lastx = -1;
    fprintf(stderr, "[screen] poll %lu\n", g_polls);
    for (int i = 0; i <= n; i++) {
        const struct tglyph *g = i < n ? &g_glyphs[idx[i]] : NULL;
        if (!g || g->y / 8 != row) {
            if (k) { line[k] = 0; fprintf(stderr, "[screen]   %s\n", line); }
            k = 0; if (!g) break; row = g->y / 8; lastx = -1;
        }
        if (lastx >= 0 && g->x - lastx > 4 && k < 500) line[k++] = ' ';
        if (k < 500) line[k++] = (char)g->ch;
        lastx = g->x + g->w;
    }
}
/* "until <text>": wait until the game shows that text (then a short pause for its input loop) */
static int until_done(void) {
    if (!until_text[0]) return 1;
    const uint8_t *p; int w, h, st; uint8_t pal[256][3]; int x0, y0, x1, y1;
    if (video_frame(&p, &w, &h, &st, pal) && text_find(p, st, until_text, &x0, &y0, &x1, &y1)) {
        fprintf(stderr, "[input] found \"%s\" (poll %lu)\n", until_text, g_polls);
        until_text[0] = 0; retry_x = -1; wait_until = g_polls + 150;
        if (click_found && getenv("CM_SCREENLOG")) screen_log(p, w, h, st);
        if (click_found) {                  /* (text_find gives the 640x480 box) */
            click_found = 0; press_later((x0 + x1) / 2, (y0 + y1) / 2); wait_until = g_polls + 60;
        }
        return 0;
    }
    if (retry_x >= 0 && g_polls >= retry_next) {
        press_later(retry_x, retry_y); retry_next = g_polls + 1500;
        fprintf(stderr, "[input] click %d,%d again (poll %lu)\n", retry_x, retry_y, g_polls);
        if (getenv("CM_RETRYSHOT")) present(1);           /* (testing: what's in the way) */
    }
    if (g_polls - until_since > 400000) { fprintf(stderr, "[input] gave up waiting for \"%s\"\n", until_text); until_text[0] = 0; click_found = 0; }
    return 0;
}
/* CM_MEMDUMP=<file> (testing): the guest's memory, written when the poll limit is reached */
static void mem_dump(void) {
    const char *p = getenv("CM_MEMDUMP"); if (!p) return;
    FILE *f = fopen(p, "wb"); if (!f) return;
    fwrite(M, 1, GUEST_MEM_SIZE, f); fclose(f); fprintf(stderr, "[host] memory written to %s\n", p);
}
static void run_script(void) {
    if (!script) { script = getenv("CM_INPUT"); if (!script) script = ""; }
    if (pending_release >= 0) { if (g_polls < release_at) return; input_mouse_button(pending_release, 0); pending_release = -1; return; }
    if (press_x >= 0) { if (g_polls < press_at) return;
        input_mouse_move(press_x, press_y); input_mouse_button(0, 1); press_x = -1; pending_release = 0; release_at = g_polls + 20; return; }
    if (!until_done()) return;
    while (*script && g_polls >= wait_until) {
        while (*script == ' ' || *script == ';') script++;
        if (!*script) break;
        char cmd[16] = ""; int a = 0, b = 0, n = 0; char ch;
        if (sscanf(script, "%15[^ ;]%n", cmd, &n) != 1) break;
        const char *args = script + n;
        const char *end = strchr(script, ';'); script = end ? end + 1 : script + strlen(script);
        if (!strcmp(cmd, "wait") && sscanf(args, "%d", &a) == 1) { wait_until = g_polls + (unsigned long)a; return; }
        if (!strcmp(cmd, "clickto") && sscanf(args, "%d %d %n", &a, &b, &n) >= 2) {
            const char *t = args + n; size_t len = strcspn(t, ";"); if (len >= sizeof until_text) len = sizeof until_text - 1;
            memcpy(until_text, t, len); until_text[len] = 0; until_since = g_polls;
            retry_x = a; retry_y = b; retry_next = g_polls;      /* first click straight away */
            return; }
        if (!strcmp(cmd, "until") || !strcmp(cmd, "clickon")) {
            click_found = cmd[0] == 'c';
            while (*args == ' ') args++;
            size_t len = strcspn(args, ";"); if (len >= sizeof until_text) len = sizeof until_text - 1;
            memcpy(until_text, args, len); until_text[len] = 0; until_since = g_polls; return; }
        if ((!strcmp(cmd, "click") || !strcmp(cmd, "rclick")) && sscanf(args, "%d %d", &a, &b) == 2) {
            int btn = cmd[0] == 'r' ? 1 : 0;
            fprintf(stderr, "[input] %s %d,%d (poll %lu)\n", cmd, a, b, g_polls);
            input_mouse_move(a, b); input_mouse_button(btn, 1); pending_release = btn; wait_until = g_polls + 4; return; }
        if (!strcmp(cmd, "move") && sscanf(args, "%d %d", &a, &b) == 2) { input_mouse_move(a, b); continue; }
        if (!strcmp(cmd, "down")) { fprintf(stderr, "[input] down (poll %lu)\n", g_polls); input_mouse_button(0, 1); continue; }
        if (!strcmp(cmd, "up"))   { fprintf(stderr, "[input] up (poll %lu)\n", g_polls); input_mouse_button(0, 0); continue; }
        if (!strcmp(cmd, "enter")) { input_key(0x1C0D); continue; }
        if (!strcmp(cmd, "esc")) { input_key(0x011B); continue; }
        if (!strcmp(cmd, "shot")) { present(1); continue; }
        if (!strcmp(cmd, "key")) {
            if (sscanf(args, " '%c'", &ch) == 1) input_key((uint16_t)(unsigned char)ch);
            else if (sscanf(args, "%d", &a) == 1) input_key((uint16_t)a);
            continue; }
        fprintf(stderr, "[input] bad script step '%s'\n", cmd);
    }
}

#ifndef _WIN32
#include <signal.h>
#include <unistd.h>
extern volatile uint32_t g_int_count; extern unsigned g_trace_pos;
static void on_alarm(int sig) {             /* watchdog: report if no interrupt for 3 s (CM_WATCHDOG=seconds; 0 = off) */
    (void)sig; static uint32_t last; static int quiet, limit = -1;
    if (limit < 0) { const char *e = getenv("CM_WATCHDOG"); limit = e ? atoi(e) : 3; }
    if (g_int_count != last) { last = g_int_count; quiet = 0; }
    else if (limit > 0 && ++quiet == limit) {
        fprintf(stderr, "\n[watchdog] stuck at poll %lu, trace pos %u. EAX=%08x EBX=%08x ECX=%08x EDX=%08x ESI=%08x EDI=%08x EBP=%08x ESP=%08x\n",
                g_polls, g_trace_pos, eax, ebx, ecx, edx, esi, edi, ebp, esp);
        rt_dump_trace();
        /* Watcom heap descriptor list (root pointer at original 0x0F6EDC) */
        uint32_t d = RD32(0x010F6EDCu); int n = 0;
        fprintf(stderr, "[heap] root %08x\n", d);
        while (d && d < GUEST_MEM_SIZE - 0x30 && n++ < 12) {
            fprintf(stderr, "[heap] desc %08x:", d); for (int i = 0; i < 12; i++) fprintf(stderr, " %08x", RD32(d + 4 * i)); fprintf(stderr, "\n");
            d = RD32(d + 8);
        }
        const char *dp = getenv("CM_DUMP");
        if (dp) { uint32_t a = (uint32_t)strtoul(dp, 0, 16); fprintf(stderr, "[dump] %08x:", a);
                  for (int i = 0; i < 16; i++) fprintf(stderr, " %08x", RD32(a + 4 * i)); fprintf(stderr, "\n"); }
        _exit(9);
    }
    alarm(1);
}
#endif
void host_init(void) {
#ifndef _WIN32
    signal(SIGALRM, on_alarm); alarm(1);
#endif
    if (getenv("CM_REPLAY")) {
        rep = fopen(getenv("CM_REPLAY"), "r");
        fprintf(stderr, rep ? "[replay] %s\n" : "[replay] cannot open %s\n", getenv("CM_REPLAY"));
    }
}
void host_tick(void) {}
void host_idle(void) {}
void host_message(const char *title, const char *text) { fprintf(stderr, "%s\n%s\n", title, text); }
#ifdef _WIN32
#include <windows.h>
void host_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <time.h>
void host_sleep_ms(uint32_t ms) { struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&t, NULL); }
#endif

/* loop back-edge in guest code: deliver input asynchronously, like a hardware IRQ */
void host_async(void) {
    replay();
    run_script();
    input_dispatch_mouse();
    const char *mx = getenv("CM_MAXPOLLS");
    if (mx && g_polls >= strtoul(mx, 0, 10)) { present(0); fprintf(stderr, "[host] poll limit reached\n"); mem_dump(); exit(0); }
}
/* the game polls keyboard/mouse/console */
void host_poll(void) {
    { static long every = -1;                /* CM_TRACESAMPLE=<polls>,<from> (call-trace build): the recent functions, now and then */
      if (every < 0) { const char *e = getenv("CM_TRACESAMPLE"); every = e ? atol(e) : 0; }
      if (every > 0 && g_polls % (unsigned long)every == 0) {
          const char *c = strchr(getenv("CM_TRACESAMPLE"), ','); if (!c || g_polls >= strtoul(c + 1, 0, 10)) {
              fprintf(stderr, "[sample] poll %lu\n", g_polls); rt_dump_trace(); } } }
    replay();
    run_script();
    input_dispatch_mouse();
    if ((g_polls & 31) == 1) { present(0); ptrdump(); }
    const char *mx = getenv("CM_MAXPOLLS");
    if (mx && g_polls >= strtoul(mx, 0, 10)) { present(0); fprintf(stderr, "[host] poll limit reached\n"); mem_dump(); exit(0); }
}
/* the game is blocked waiting for a key */
void host_wait(void) {
    static unsigned long idle;
    g_polls++;
    host_poll();
    if (input_key_ready()) { idle = 0; return; }
    if (++idle > 200000) { present(0); fprintf(stderr, "[host] waiting for a key with no input left\n"); exit(0); }
}

/* match speed setting: CM_MATCH_SPEED=1..3 */
static int hl_speed = -1;
int  host_match_speed(void) { if (hl_speed < 0) hl_speed = getenv("CM_MATCH_SPEED") ? atoi(getenv("CM_MATCH_SPEED")) : 1;
                              return hl_speed < 1 || hl_speed > 3 ? 1 : hl_speed; }
void host_set_match_speed(int speed) { hl_speed = speed; }

/* tests run on copies of the game: no save backups unless asked (CM_BACKUPS=n) */
#include "backup.h"
__attribute__((constructor)) static void headless_backups(void) { g_backup_keep = getenv("CM_BACKUPS") ? atoi(getenv("CM_BACKUPS")) : 0; }

/* autosave.c asks whether to load a newer autosave: CM_ASK=yes answers yes */
int host_ask(const char *title, const char *text) {
    int yes = getenv("CM_ASK") && !strcmp(getenv("CM_ASK"), "yes");
    fprintf(stderr, "[ask] %s: %s -> %s\n", title, text, yes ? "yes" : "no");
    return yes;
}
void host_notify(const char *text) { fprintf(stderr, "[note] %s\n", text); }
void host_set_database(int start_year) { fprintf(stderr, "[database] now starting in %d\n", start_year); }

/* the Options screen's Restore Backup (overrides.c): no panel without a window. For testing,
   CM_BK_RESTORE=<slot> restores that saved game's newest backup. */
#include "backup.h"
#include "database.h"
void host_recomp_options(void) { fprintf(stderr, "[options] Recomp Options chosen\n"); }
void host_restore_backup(void) {
    const char *e = getenv("CM_BK_RESTORE");
    fprintf(stderr, "[backup] Restore Backup chosen\n");
    if (!e) return;
    int slot = atoi(e); struct backup_info b[64]; int n = backup_list(slot, b, 64);
    if (n > 0) fprintf(stderr, "[backup] restoring slot %d from %s: %d\n", slot, b[0].name, backup_restore(slot, b[0].name));
}

/* Change Database (overrides.c): no panel without a window; the next database, as before */
int host_choose_database(void) { if (db_count() < 2) return 0; db_next(); return 1; }
/* Database Editor: CM_EDIT_DB=<number> (default: the one chosen for the game); "cancel" = none */
/* a report (the Database Editor's Test Database): to stderr */
void host_report(const char *title, char **lines, int n) { fprintf(stderr, "[report] %s\n", title); for (int i = 0; i < n; i++) fprintf(stderr, "[report] %s\n", lines[i]); }
/* a list (the Database Editor's Load Database): CM_PICK=<number>, default the one suggested; "cancel" = none */
int host_pick_list(const char *title, const char *choose, const char *head1, const char *head2,
                   const char *const *col1, const char *const *col2, int n, int sel) {
    (void)choose; (void)head1; (void)head2; (void)col2;
    const char *e = getenv("CM_PICK"); int i = e ? (strcmp(e, "cancel") ? atoi(e) : -1) : sel;
    if (i >= n) i = -1;
    fprintf(stderr, "[pick] %s: %s\n", title, i >= 0 ? col1[i] : "(cancelled)");
    return i;
}
int host_pick_database(void) { const char *e = getenv("CM_EDIT_DB"); if (e && !strcmp(e, "cancel")) return -1;
    return e ? atoi(e) : db_selected(); }
