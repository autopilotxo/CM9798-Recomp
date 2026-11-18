/* dos.c - DOS / DOS4GW (DPMI) / BIOS emulation for the recompiled CM2E16.
 *
 * Guest linear memory map (M):
 *   0x00000000-0x000FFFFF  emulated conventional memory (IVT, BDA, PSP, env, DOS heap, VGA window)
 *   0x01010000-0x01AFEAB0  game image (code + data + BSS + stack), relocated +16 MB
 *   0x02000000-0x03FFFFFF  DPMI / DOS4GW heap
 *
 * Set CM_TRACE=1 to log every interrupt.
 */
#include "rt_internal.h"
#include <stdio.h>
extern unsigned long g_polls;
#include "backup.h"
#include "autosave.h"
#include "database.h"
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#ifdef _MSC_VER
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif
#include <sys/stat.h>

extern uint32_t desc_base[8192], desc_limit[8192]; extern uint8_t desc_used[8192];

#define PSP_LIN   0x00020000u
#define ENV_LIN   0x00021000u
#define DOSMEM_LO 0x00030000u
#define DOSMEM_HI 0x0009F000u
#define HEAP_LO   0x02000000u
#define HEAP_HI   0x04000000u
#define SEL_PSP   0x0024u
#define SEL_ENV   0x002Cu
#define SEL_CODE  0x0170u
#define SEL_DATA  0x0178u

static int trace;
static time_t start_time;
extern int g_replaying;
uint32_t host_ms(void);
void host_poll(void); void host_present(void); void host_tick(void); void host_idle(void); void rt_dump_trace(void);
uint32_t dos_console_read(int blocking); int dos_console_ready(void);
static char gamedir[1024] = ".";
static uint32_t dta_lin = PSP_LIN + 0x80;
static uint32_t dosmem_next = DOSMEM_LO, heap_next = HEAP_LO;
static uint32_t pm_vec_sel[256], pm_vec_off[256], rm_vec[256];

#define CF_SET()   (g_cf = 1)
#define CF_CLR()   (g_cf = 0)
#define AX (eax & 0xFFFF)
#define AH ((eax >> 8) & 0xFF)
#define AL (eax & 0xFF)
#define LIN(seg, off) (segbase_[seg] + (off))
static void fail(uint32_t code) { SET16(eax, code); CF_SET(); }

/* ---------------- descriptors ---------------- */
static uint32_t new_selector(uint32_t base, uint32_t limit) {
    for (uint32_t i = 64; i < 8192; i++) if (!desc_used[i]) {
        desc_used[i] = 1; desc_base[i] = base; desc_limit[i] = limit; return (i << 3) | 7; }
    return 0;
}
static void set_desc(uint32_t sel, uint32_t base, uint32_t limit) {
    desc_used[sel >> 3] = 1; desc_base[sel >> 3] = base; desc_limit[sel >> 3] = limit;
}

/* ---------------- files ---------------- */
#define MAXH 64
static FILE *hfile[MAXH];

static void copy_guest_str(char *dst, uint32_t lin, size_t max) {
    size_t i = 0; for (; i + 1 < max && M[lin + i]; i++) dst[i] = (char)M[lin + i]; dst[i] = 0;
}
/* DOS character devices: NUL, CON, PRN, AUX, LPT1-3, COM1-4 (with or without extension/path) */
static const char *dos_device(const char *dos) {
    const char *b = dos; for (const char *q = dos; *q; q++) if (*q == '\\' || *q == '/' || *q == ':') b = q + 1;
    char nm[9]; int i = 0; for (; b[i] && b[i] != '.' && i < 8; i++) nm[i] = (char)toupper((unsigned char)b[i]); nm[i] = 0;
    static const char *devs[] = { "NUL", "CON", "PRN", "AUX", "LPT1", "LPT2", "LPT3", "COM1", "COM2", "COM3", "COM4", 0 };
    for (int k = 0; devs[k]; k++) if (!strcmp(nm, devs[k])) return devs[k];
    return 0;
}
static FILE *open_device(const char *dev) {
    if (!strcmp(dev, "CON")) return stdout;
#ifdef _WIN32
    return fopen("NUL", "w+b");
#else
    return fopen("/dev/null", "w+b");
#endif
}

/* DOS path -> host path inside gamedir, case-insensitive per component */
static void map_path(const char *dos, char *out, size_t max) {
    const char *p = dos;
    if (p[0] && p[1] == ':') p += 2;
    while (*p == '\\' || *p == '/') p++;
    snprintf(out, max, "%s", gamedir);
    char comp[260];
    while (*p) {
        size_t n = 0; while (p[n] && p[n] != '\\' && p[n] != '/') n++;
        if (n >= sizeof comp) n = sizeof comp - 1;
        memcpy(comp, p, n); comp[n] = 0; p += n; while (*p == '\\' || *p == '/') p++;
        if (!strcmp(comp, ".")) continue;
        char found[260]; snprintf(found, sizeof found, "%s", comp);
        DIR *d = opendir(out);
        if (d) { struct dirent *e; while ((e = readdir(d))) if (!strcasecmp(e->d_name, comp)) { snprintf(found, sizeof found, "%s", e->d_name); break; } closedir(d); }
        size_t L = strlen(out); snprintf(out + L, max - L, "/%s", found);
    }
}
/* The match commentary (ENG_MTCH.TXT, MTCHDSC*.TXT) puts a space before "!" and "?" ("Penalty
   to <T> !!"). The game is given a corrected copy instead; the original file is left alone. */
static FILE *open_commentary(const char *dos_path, const char *host) {
    const char *b = dos_path + strlen(dos_path);
    while (b > dos_path && b[-1] != '\\' && b[-1] != '/' && b[-1] != ':') b--;
    if (strcasecmp(b, "ENG_MTCH.TXT") && strncasecmp(b, "MTCHDSC", 7)) return NULL;
    FILE *in = fopen(host, "rb"); if (!in) return NULL;
#ifdef _WIN32
    /* tmpfile() can try the root of C:, which isn't writable: use the user's temp folder */
    char tmp[1100]; const char *td = getenv("TEMP"); if (!td) td = getenv("TMP");
    if (!td) { fclose(in); return NULL; }
    snprintf(tmp, sizeof tmp, "%s\\cm9798_%s", td, b);
    FILE *out = fopen(tmp, "w+b");
#else
    FILE *out = tmpfile();
#endif
    if (!out) { fclose(in); return NULL; }   /* fall back to the original file */
    int prev = '\n', c, pending_space = 0, fixed = 0;
    while ((c = fgetc(in)) != EOF) {
        if (pending_space) {                       /* a space after a word: keep it unless "!" or "?" follows */
            pending_space = 0;
            if (c == '!' || c == '?') fixed++; else fputc(' ', out);
        }
        if (c == ' ' && prev != ' ' && prev != '"' && prev != '|' && prev != '\n' && prev != '\t') { pending_space = 1; prev = c; continue; }
        fputc(c, out); prev = c;
    }
    if (pending_space) fputc(' ', out);
    fclose(in); rewind(out);
    if (getenv("CM_TEXTLOG")) fprintf(stderr, "text: %s: removed %d spaces before ! and ?\n", b, fixed);
    return out;
}

static int alloc_handle(FILE *f) { for (int i = 5; i < MAXH; i++) if (!hfile[i]) { hfile[i] = f; return i; } return -1; }

/* ---------------- find first / next ---------------- */
static DIR *find_dir; static char find_pat[64], find_dirpath[1024];
static int wild_match(const char *pat, const char *name) {
    for (; *pat; pat++, name++) {
        if (*pat == '*') { if (!pat[1]) return 1; for (; *name; name++) if (wild_match(pat + 1, name)) return 1; return wild_match(pat + 1, name); }
        if (!*name) return 0;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*name)) return 0;
    }
    return *name == 0;
}
static int dos_pat_match(const char *pat, const char *name) {
    if (!strcmp(pat, "*.*")) return 1;
    if (wild_match(pat, name)) return 1;
    if (!strchr(name, '.')) { char t[300]; snprintf(t, sizeof t, "%s.", name); return wild_match(pat, t); }
    return 0;
}
static int find_next(void) {
    struct dirent *e;
    while (find_dir && (e = readdir(find_dir))) {
        if (e->d_name[0] == '.' || strlen(e->d_name) > 12) continue;
        if (!dos_pat_match(find_pat, e->d_name)) continue;
        char full[1400]; struct stat st; snprintf(full, sizeof full, "%s/%s", find_dirpath, e->d_name);
        if (stat(full, &st)) continue;
        uint32_t d = dta_lin;
        M[d + 0x15] = S_ISDIR(st.st_mode) ? 0x10 : 0x20;
        struct tm *tm = localtime(&st.st_mtime);
        WR16(d + 0x16, (tm->tm_hour << 11) | (tm->tm_min << 5) | (tm->tm_sec / 2));
        WR16(d + 0x18, ((tm->tm_year - 80) << 9) | ((tm->tm_mon + 1) << 5) | tm->tm_mday);
        WR32(d + 0x1A, (uint32_t)st.st_size);
        for (int i = 0; i < 13; i++) M[d + 0x1E + i] = 0;
        for (int i = 0; e->d_name[i] && i < 12; i++) M[d + 0x1E + i] = (uint8_t)toupper((unsigned char)e->d_name[i]);
        return 1;
    }
    return 0;
}

/* ---------------- setup ---------------- */
void dos_setup(const char *dir) {
    trace = getenv("CM_TRACE") != NULL;
    { extern int g_deterministic; g_deterministic = getenv("CM_RECORD") || getenv("CM_REPLAY");
      g_replaying = getenv("CM_REPLAY") != NULL;
      start_time = g_deterministic ? (time_t)873000000 : time(0); }   /* fixed start in record/replay mode */
    snprintf(gamedir, sizeof gamedir, "%s", dir);
    set_desc(0, 0, 0xFFFFFFFF);
    set_desc(SEL_CODE, 0, 0xFFFFFFFF); set_desc(SEL_DATA, 0, 0xFFFFFFFF);
    set_desc(SEL_PSP, PSP_LIN, 0xFF); set_desc(SEL_ENV, ENV_LIN, 0xFFF);
    /* PSP */
    M[PSP_LIN] = 0xCD; M[PSP_LIN + 1] = 0x20;
    WR16(PSP_LIN + 0x2C, SEL_ENV);
    M[PSP_LIN + 0x80] = 0; M[PSP_LIN + 0x81] = 0x0D;
    /* environment + program path */
    static const char env[] = "PATH=C:\\\0DOS4G=QUIET\0COMSPEC=C:\\COMMAND.COM\0";
    memcpy(M + ENV_LIN, env, sizeof env);       /* ends with the double NUL */
    uint32_t p = ENV_LIN + sizeof env;
    WR16(p, 1); memcpy(M + p + 2, "C:\\CM9798\\CM2E16.EXE", 21);
    /* BIOS data area: 80x25 colour text, tick counter */
    M[0x449] = 3; WR16(0x44A, 80);
    hfile[0] = stdin; hfile[1] = stdout; hfile[2] = stderr; hfile[3] = stderr; hfile[4] = stderr;
    rt_set_seg(1, SEL_CODE); rt_set_seg(2, SEL_DATA); rt_set_seg(3, SEL_DATA);
    rt_set_seg(4, SEL_DATA); rt_set_seg(5, 0); rt_set_seg(0, SEL_PSP);
}

/* ---------------- INT 21h ---------------- */
static void int21(void) {
    uint32_t dsedx = LIN(3, edx);
    char path[260], host[1400];
    CF_CLR();
    switch (AH) {
    case 0x02: putchar(edx & 0xFF); break;
    case 0x01: case 0x07: case 0x08: SET8L(eax, dos_console_read(1)); break;
    case 0x06: if ((edx & 0xFF) != 0xFF) putchar(edx & 0xFF);
               else { uint32_t c = dos_console_read(0); if (c == 0x100) { g_zf = 1; SET8L(eax, 0); } else { g_zf = 0; SET8L(eax, c); } } break;
    case 0x0B: SET8L(eax, dos_console_ready() ? 0xFF : 0); break;
    case 0x0C: SET8L(eax, 0); break;
    case 0x09: { uint32_t q = dsedx; while (M[q] != '$') putchar(M[q++]); break; }
    case 0x0E: SET8L(eax, 26); break;                         /* select drive: 26 drives */
    case 0x19: SET8L(eax, 2); break;                          /* current drive C: */
    case 0x1A: dta_lin = dsedx; break;
    case 0x2F: rt_set_seg(0, SEL_DATA); ebx = dta_lin; break;
    case 0x25: break;                                          /* set vector (real mode, ignored) */
    case 0x35: rt_set_seg(0, 0); ebx = 0; break;
    case 0x2A: { time_t t = start_time + host_ms() / 1000; struct tm *tm = localtime(&t);
        SET16(ecx, tm->tm_year + 1900); SET16(edx, ((tm->tm_mon + 1) << 8) | tm->tm_mday); SET8L(eax, tm->tm_wday); break; }
    case 0x2C: { host_idle(); uint32_t ms = host_ms(); time_t t = start_time + ms / 1000; struct tm *tm = localtime(&t);
        SET16(ecx, (tm->tm_hour << 8) | tm->tm_min); SET16(edx, (tm->tm_sec << 8) | ((ms % 1000) / 10));
        if (getenv("CM_CLOCKLOG")) { static uint32_t n; if ((n++ % 20000) == 0)
            fprintf(stderr, "[clock] call %u: game time %02d:%02d:%02d.%02u  (host ms %u)\n", n, tm->tm_hour, tm->tm_min, tm->tm_sec, (ms % 1000) / 10, ms); }
        break; }
    case 0x30: SET16(eax, 0x0007); ebx &= 0xFFFF0000u; ecx &= 0xFFFF0000u; break;  /* DOS 7.0 */
    case 0x33: SET8L(edx, 0); break;
    case 0x36: SET16(eax, 64); SET16(ebx, 0x7FFF); SET16(ecx, 512); SET16(edx, 0xFFFF); break; /* free space */
    case 0x3B: break;                                          /* chdir: accept */
    case 0x3C: case 0x5B: copy_guest_str(path, dsedx, sizeof path);
        if (dos_device(path)) { int h = alloc_handle(open_device(dos_device(path))); if (h < 0) fail(4); else SET16(eax, h); break; }
        map_path(path, host, sizeof host);
        if (getenv("CM_FILELOG")) fprintf(stderr, "[file] create %s (poll %lu)\n", path, g_polls);
        backup_before_write(path);
        save_redirect(path, host, sizeof host, 1);
        autosave_redirect(path, host, sizeof host);
        { FILE *f = fopen(host, "w+b"); int h = f ? alloc_handle(f) : -1; if (h < 0) fail(3); else SET16(eax, h); } break;
    case 0x3D: { copy_guest_str(path, dsedx, sizeof path);
        if (dos_device(path)) { int h = alloc_handle(open_device(dos_device(path))); if (h < 0) fail(4); else SET16(eax, h); break; }
        int mode = AL & 3;
        if (!mode && getenv("CM_FILELOG")) fprintf(stderr, "[file] read %s (poll %lu)\n", path, g_polls);
        if (mode) backup_before_write(path); else autosave_before_read(path);
        map_path(path, host, sizeof host);
        save_redirect(path, host, sizeof host, mode != 0);
        if (mode) autosave_redirect(path, host, sizeof host); else if (!db_redirect(path, host, sizeof host)) pic_redirect(path, host, sizeof host);
        FILE *f = mode == 0 ? open_commentary(path, host) : NULL;
        { struct stat st_; if (!stat(host, &st_) && S_ISDIR(st_.st_mode)) { fail(2); break; } }   /* a folder (e.g. the game
            asks for ""): DOS and Windows refuse it, Linux would open it and the game read rubbish */
        if (!f) f = fopen(host, mode == 0 ? "rb" : "r+b");
        if (mode && getenv("CM_FILELOG")) fprintf(stderr, "[file] open for writing %s (poll %lu)\n", path, g_polls);
        if (trace) fprintf(stderr, "[dos] open \"%s\" -> %s%s\n", path, host, f ? "" : " (not found)");
        { const char *bk = getenv("CM_BREAK_OPEN"); static int hits;   /* e.g. silogo.pcx:2 */
          if (bk) { char nm[128]; int nth = 1; sscanf(bk, "%127[^:]:%d", nm, &nth);
            if (!strcasecmp(nm, path) && ++hits == nth) { fprintf(stderr, "[break] open #%d of %s\n", nth, path); rt_dump_trace(); } } }
        if (!f) { fail(2); break; } int h = alloc_handle(f); if (h < 0) { fclose(f); fail(4); } else SET16(eax, h); break; }
    case 0x3E: { int h = ebx & 0xFFFF; if (h >= 5 && h < MAXH && hfile[h]) { if (hfile[h] != stdout) fclose(hfile[h]); hfile[h] = 0; } break; }
    case 0x3F: { int h = ebx & 0xFFFF; FILE *f = h < MAXH ? hfile[h] : 0; if (!f) { fail(6); break; }
        if (h == 0) { eax = 0; break; }
        if (trace) fprintf(stderr, "[dos] read h=%d n=%u -> %08x\n", h, ecx, dsedx);
        eax = (uint32_t)fread(M + dsedx, 1, ecx, f); break; }
    case 0x40: { int h = ebx & 0xFFFF; FILE *f = h < MAXH ? hfile[h] : 0; if (!f) { fail(6); break; }
        if (ecx == 0) { /* truncate at current position: not portable, ignore */ eax = 0; break; }
        eax = (uint32_t)fwrite(M + dsedx, 1, ecx, f); fflush(f); break; }
    case 0x41: copy_guest_str(path, dsedx, sizeof path); map_path(path, host, sizeof host);
        save_redirect(path, host, sizeof host, 0);              /* Delete Game: the chosen database's slot */
        if (getenv("CM_FILELOG")) fprintf(stderr, "[file] delete %s -> %s\n", path, host);
        if (remove(host)) fail(2); break;
    case 0x42: { int h = ebx & 0xFFFF; FILE *f = h < MAXH ? hfile[h] : 0; if (!f || h < 5) { fail(6); break; }
        int32_t off = (int32_t)(((ecx & 0xFFFF) << 16) | (edx & 0xFFFF));
        if (fseek(f, off, AL == 0 ? SEEK_SET : AL == 1 ? SEEK_CUR : SEEK_END)) { fail(25); break; }
        long pos = ftell(f); SET16(eax, pos & 0xFFFF); SET16(edx, (pos >> 16) & 0xFFFF); break; }
    case 0x43: copy_guest_str(path, dsedx, sizeof path); map_path(path, host, sizeof host); save_redirect(path, host, sizeof host, 0);
        { struct stat st; if (stat(host, &st)) fail(2); else SET16(ecx, S_ISDIR(st.st_mode) ? 0x10 : 0x20); } break;
    case 0x44: if (AL == 0x00) { int h = ebx & 0xFFFF; SET16(edx, (h < 5 || (h < MAXH && hfile[h] == stdout)) ? 0x80D3 : 0x0002); }
               else if (AL == 0x08) SET16(eax, 1); else if (AL == 0x09) SET16(edx, 0);
               break;
    case 0x47: M[LIN(3, esi)] = 0; break;                      /* cwd = root */
    case 0x48: { uint32_t bytes = (ebx & 0xFFFF) * 16u; if (heap_next + bytes > HEAP_HI) { fail(8); SET16(ebx, 0); break; }
        uint32_t sel = new_selector(heap_next, bytes - 1); memset(M + heap_next, 0, bytes); heap_next = (heap_next + bytes + 15) & ~15u;
        SET16(eax, sel); break; }
    case 0x49: break;
    case 0x4A: break;                                          /* resize: accept */
    case 0x4C: fflush(stdout); host_present(); if (trace) rt_dump_trace(); exit(AL);
    case 0x4E: { copy_guest_str(path, dsedx, sizeof path);
        char *slash = strrchr(path, '\\'); char dirpart[260] = "";
        if (slash) { memcpy(dirpart, path, (size_t)(slash - path)); dirpart[slash - path] = 0; snprintf(find_pat, sizeof find_pat, "%s", slash + 1); }
        else snprintf(find_pat, sizeof find_pat, "%s", path[1] == ':' ? path + 2 : path);
        map_path(dirpart, find_dirpath, sizeof find_dirpath);
        if (getenv("CM_FILELOG")) fprintf(stderr, "[file] find %s\n", path);
        if (find_dir) closedir(find_dir); find_dir = opendir(find_dirpath);
        if (!find_next()) fail(18); break; }
    case 0x4F: if (!find_next()) fail(18); break;
    case 0x56: { char p2[260], h2[1400]; copy_guest_str(path, dsedx, sizeof path); copy_guest_str(p2, LIN(0, edi), sizeof p2);
        map_path(path, host, sizeof host); map_path(p2, h2, sizeof h2);
        save_redirect(path, host, sizeof host, 1); save_redirect(p2, h2, sizeof h2, 1);
        if (rename(host, h2)) fail(5); break; }
    case 0x57: if (AL == 0) { SET16(ecx, 0); SET16(edx, 0x21); } break;
    case 0x58: SET16(eax, 0); break;
    case 0x59: SET16(eax, 0); break;
    case 0x62: SET16(ebx, SEL_PSP); break;
    case 0x67: break;
    case 0x68: break;
    case 0xFF: if (AX == 0xFF00 && (edx & 0xFFFF) == 0x0078) { SET8L(eax, 1); rt_set_seg(5, 0); }  /* DOS/4G present */
        break;
    default:
        fprintf(stderr, "[dos] unhandled INT 21h AX=%04X\n", AX); fail(1); break;
    }
}

/* ---------------- INT 31h (DPMI) ---------------- */
static void int31(void) {
    CF_CLR();
    switch (AX) {
    case 0x0000: { uint32_t n = ecx & 0xFFFF, first = 0;
        for (uint32_t i = 0; i < n; i++) { uint32_t s = new_selector(0, 0); if (!i) first = s; }
        if (!first) fail(0x8011); else SET16(eax, first); break; }
    case 0x0001: desc_used[(ebx & 0xFFFF) >> 3] = 0; break;
    case 0x0002: SET16(eax, new_selector((ebx & 0xFFFF) << 4, 0xFFFF)); break;   /* segment to descriptor */
    case 0x0003: SET16(eax, 8); break;
    case 0x0006: { uint32_t b = desc_base[(ebx & 0xFFFF) >> 3]; SET16(ecx, b >> 16); SET16(edx, b); break; }
    case 0x0007: desc_base[(ebx & 0xFFFF) >> 3] = ((ecx & 0xFFFF) << 16) | (edx & 0xFFFF);
        for (int i = 0; i < 6; i++) if (sel_[i] == (ebx & 0xFFFF)) segbase_[i] = desc_base[(ebx & 0xFFFF) >> 3]; break;
    case 0x0008: desc_limit[(ebx & 0xFFFF) >> 3] = ((ecx & 0xFFFF) << 16) | (edx & 0xFFFF); break;
    case 0x0009: break;
    case 0x000A: { uint32_t s = ebx & 0xFFFF; SET16(eax, new_selector(desc_base[s >> 3], desc_limit[s >> 3])); break; }
    case 0x000B: { uint32_t s = ebx & 0xFFFF, d = LIN(0, edi), b = desc_base[s >> 3], l = desc_limit[s >> 3];
        WR16(d, l); WR16(d + 2, b); M[d + 4] = b >> 16; M[d + 5] = 0xF2; M[d + 6] = 0xCF; M[d + 7] = b >> 24; break; }
    case 0x000C: { uint32_t s = ebx & 0xFFFF, d = LIN(0, edi);
        desc_base[s >> 3] = RD16(d + 2) | (M[d + 4] << 16) | (M[d + 7] << 24); desc_limit[s >> 3] = RD16(d) | ((M[d + 6] & 0xF) << 16); break; }
    case 0x0100: { uint32_t bytes = (ebx & 0xFFFF) * 16u;
        if (dosmem_next + bytes > DOSMEM_HI) { fail(8); SET16(ebx, (DOSMEM_HI - dosmem_next) / 16); break; }
        SET16(eax, dosmem_next >> 4); SET16(edx, new_selector(dosmem_next, bytes - 1));
        dosmem_next = (dosmem_next + bytes + 15) & ~15u; break; }
    case 0x0101: case 0x0102: break;
    case 0x0200: { uint32_t v = rm_vec[ebx & 0xFF]; SET16(ecx, v >> 16); SET16(edx, v); break; }
    case 0x0201: rm_vec[ebx & 0xFF] = ((ecx & 0xFFFF) << 16) | (edx & 0xFFFF); break;
    case 0x0202: case 0x0203: SET16(ecx, SEL_CODE); edx = 0; break;
    case 0x0204: SET16(ecx, pm_vec_sel[ebx & 0xFF] ? pm_vec_sel[ebx & 0xFF] : SEL_CODE); edx = pm_vec_off[ebx & 0xFF]; break;
    case 0x0205: pm_vec_sel[ebx & 0xFF] = ecx & 0xFFFF; pm_vec_off[ebx & 0xFF] = edx;
        if (trace) fprintf(stderr, "[dpmi] PM vector %02X -> %04X:%08X\n", ebx & 0xFF, ecx & 0xFFFF, edx); break;
    case 0x0300: case 0x0301: case 0x0302: {
        extern void rm_interrupt(uint32_t intno, uint32_t rmregs);
        rm_interrupt(ebx & 0xFF, LIN(0, edi)); break; }
    case 0x0303: SET16(ecx, 0xF000); SET16(edx, 0); break;    /* RM callback: give a dummy address */
    case 0x0400: SET16(eax, 0x005A); SET16(ebx, 0x0005); SET8L(ecx, 4); SET16(edx, 0x0870); break;
    case 0x0500: { uint32_t d = LIN(0, edi); memset(M + d, 0xFF, 0x30);
        uint32_t freeb = HEAP_HI - heap_next; WR32(d, freeb); WR32(d + 4, freeb / 4096); WR32(d + 8, freeb / 4096);
        WR32(d + 0x0C, (HEAP_HI - HEAP_LO) / 4096); WR32(d + 0x14, freeb / 4096); WR32(d + 0x18, (HEAP_HI - HEAP_LO) / 4096);
        WR32(d + 0x20, freeb / 4096); break; }
    case 0x0501: { uint32_t bytes = ((ebx & 0xFFFF) << 16) | (ecx & 0xFFFF);
        heap_next = (heap_next + 0xFFF) & ~0xFFFu;
        if (heap_next + bytes > HEAP_HI) { fail(0x8013); break; }
        uint32_t a = heap_next; heap_next += bytes; memset(M + a, 0, bytes);
        SET16(ebx, a >> 16); SET16(ecx, a); SET16(esi, a >> 16); SET16(edi, a); break; }
    case 0x0502: break;
    case 0x0503: { /* resize: allocate new and copy (bump allocator) */
        uint32_t bytes = ((ebx & 0xFFFF) << 16) | (ecx & 0xFFFF), old = ((esi & 0xFFFF) << 16) | (edi & 0xFFFF);
        heap_next = (heap_next + 0xFFF) & ~0xFFFu;
        if (heap_next + bytes > HEAP_HI) { fail(0x8013); break; }
        uint32_t a = heap_next; heap_next += bytes; memmove(M + a, M + old, bytes);
        SET16(ebx, a >> 16); SET16(ecx, a); SET16(esi, a >> 16); SET16(edi, a); break; }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: break;
    case 0x0604: SET16(ebx, 0); SET16(ecx, 4096); break;
    case 0x0800: { extern uint32_t vbe_lfb_linear(void); uint32_t a = vbe_lfb_linear(); SET16(ebx, a >> 16); SET16(ecx, a); break; }
    case 0x0801: break;
    case 0x0900: SET8L(eax, 1); break;
    case 0x0901: SET8L(eax, 0); break;
    case 0x0902: SET8L(eax, 1); break;
    default:
        fprintf(stderr, "[dpmi] unhandled INT 31h AX=%04X\n", AX); fail(0x8001); break;
    }
}

/* ---------------- BIOS ---------------- */
void video_int10(void); void kbd_int16(void); void mouse_int33(void);
void rm_interrupt(uint32_t n, uint32_t r);
static uint32_t ticks(void) { return (uint32_t)(host_ms() * 0.0182065); }   /* BIOS ticks, 18.2 Hz */

volatile uint32_t g_int_count;
unsigned long g_polls;                 /* input delivery points: used for record/replay */
int g_deterministic;                   /* CM_RECORD / CM_REPLAY: clock derived from interrupt count */
/* milliseconds since start: real time, or (deterministic mode) 1 ms per 8 interrupts */
/* wall-clock milliseconds since the first call (portable C11) */
static uint32_t wall_ms(void) {
    static struct timespec t0; static int init;
    struct timespec t; timespec_get(&t, TIME_UTC);
    if (!init) { t0 = t; init = 1; }
    return (uint32_t)((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000);
}
void host_sleep_ms(uint32_t ms);       /* provided by the display backend */
int g_replaying;                       /* headless replay: run as fast as possible */
uint32_t host_ms(void) {
    if (!g_deterministic) return wall_ms();
    /* record/replay: time is a pure function of the interrupt count (so a replay sees exactly
       the same clock), but while recording it is never allowed to run ahead of real time */
    uint32_t t = g_int_count / 8;
    if (!g_replaying) { uint32_t w = wall_ms(); if (t > w + 2) host_sleep_ms(t - w); }
    return t;
}
static int is_poll(uint32_t n) {
    if (n == 0x16 || n == 0x33) return 1;
    if (n == 0x21) { uint32_t ah = (eax >> 8) & 0xFF;
        return ah == 0x0B || ah == 0x01 || ah == 0x07 || ah == 0x08 || (ah == 0x06 && (edx & 0xFF) == 0xFF); }
    return 0;
}
void rt_int(uint32_t n) {
    g_int_count++;
    host_tick();
    if (is_poll(n)) { g_polls++; host_poll(); }
    if (trace && n != 0x16 && n != 0x33) fprintf(stderr, "[int %02X] EAX=%08X EBX=%08X ECX=%08X EDX=%08X\n", n, eax, ebx, ecx, edx);
    switch (n) {
    case 0x21: int21(); break;
    case 0x31: int31(); break;
    case 0x10: video_int10(); break;
    case 0x16: kbd_int16(); break;
    case 0x1A: host_idle(); if (AH == 0) { uint32_t t = ticks(); SET16(ecx, t >> 16); SET16(edx, t); SET8L(eax, 0); } break;
    case 0x2F: if (AX == 0x1600) SET8L(eax, 0); else if (AX == 0x1686) SET16(eax, 0); break;
    case 0x33: mouse_int33(); break;
    case 0x17: SET8H(eax, 0x90); break;     /* printer: ready */
    default: fprintf(stderr, "[int] unhandled INT %02Xh AX=%04X\n", n, AX); break;
    }
}

/* ---------------- I/O ports ---------------- */
uint8_t vga_dac[256][3]; static uint32_t dac_w, dac_r; static uint32_t retrace;
uint32_t rt_in(uint32_t port, int size) {
    (void)size;
    switch (port) {
    case 0x3DA: retrace ^= 0x09; return retrace;               /* toggle vertical retrace */
    case 0x3C9: { uint32_t v = vga_dac[(dac_r / 3) & 255][dac_r % 3]; dac_r++; return v; }
    case 0x60: return 0;
    case 0x40: return host_ms() & 0xFF;
    default: if (trace) fprintf(stderr, "[io] in %04X\n", port); return 0;
    }
}
void rt_out(uint32_t port, uint32_t v, int size) {
    (void)size;
    switch (port) {
    case 0x3C8: dac_w = (v & 0xFF) * 3; break;
    case 0x3C7: dac_r = (v & 0xFF) * 3; break;
    case 0x3C9: vga_dac[(dac_w / 3) & 255][dac_w % 3] = v & 0x3F; dac_w++; break;
    case 0x20: case 0xA0: case 0x40: case 0x43: case 0x21: case 0xA1: break;   /* PIC / PIT */
    default: if (trace) fprintf(stderr, "[io] out %04X <- %X\n", port, v); break;
    }
}
