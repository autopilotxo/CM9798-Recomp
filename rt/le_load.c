/* le_load.c - load CM2E16.EXE (MZ + bound DOS/4GW + LE) straight into guest memory.
 *
 * Objects are placed at their LE base + ADDR_SHIFT and all internal fixups are applied,
 * exactly as tools/img.py does, so the recompiled code finds code/data where it expects.
 * The player's own copy of the game is the only source of the original bytes.
 */
#include "rt_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define ADDR_SHIFT 0x01000000u

static uint32_t u32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t u16(const uint8_t *p) { return p[0] | (p[1] << 8); }

int rt_load_exe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return -2; }
    fclose(f);

    /* find the MZ stub whose e_lfanew points at an "LE\0\0" header (skips the DOS/4GW loader) */
    long mz = -1, le = -1;
    for (long i = 0; i + 0x40 < n; i++) {
        if (d[i] != 'M' || d[i + 1] != 'Z') continue;
        uint32_t o = u32(d + i + 0x3C);
        if ((uint64_t)i + o + 4 <= (uint64_t)n && d[i + o] == 'L' && d[i + o + 1] == 'E' && !d[i + o + 2] && !d[i + o + 3]) {
            mz = i; le = i + (long)o; break;
        }
    }
    if (le < 0) { free(d); return -3; }
    const uint8_t *h = d + le;
    uint32_t npages = u32(h + 0x14), psize = u32(h + 0x28), lastp = u32(h + 0x2C);
    uint32_t objtab = u32(h + 0x40), nobj = u32(h + 0x44), pagemap = u32(h + 0x48);
    uint32_t fixpage = u32(h + 0x68), fixrec = u32(h + 0x6C), datapages = (uint32_t)mz + u32(h + 0x80);
    if (nobj > 8 || psize != 4096) { free(d); return -4; }

    uint32_t base[8], size[8], first[8], cnt[8];
    for (uint32_t k = 0; k < nobj; k++) {
        const uint8_t *o = h + objtab + 24 * k;
        size[k] = u32(o); base[k] = u32(o + 4) + ADDR_SHIFT; first[k] = u32(o + 12); cnt[k] = u32(o + 16);
        if ((uint64_t)base[k] + size[k] > GUEST_MEM_SIZE) { free(d); return -5; }
        memset(M + base[k], 0, size[k]);
        for (uint32_t j = 0; j < cnt[k]; j++) {
            uint32_t pg = first[k] + j;                                 /* 1-based */
            const uint8_t *pm = h + pagemap + 4 * (pg - 1);
            uint32_t num = (pm[0] << 16) | (pm[1] << 8) | pm[2];
            uint32_t len = pg == npages ? lastp : psize;
            uint32_t off = j * psize; if (off >= size[k]) break;
            if (len > size[k] - off) len = size[k] - off;
            uint64_t src = (uint64_t)datapages + (uint64_t)(num - 1) * psize;
            if (src + len > (uint64_t)n) len = src < (uint64_t)n ? (uint32_t)(n - src) : 0;
            memcpy(M + base[k] + off, d + src, len);
        }
    }
    /* fixups: only internal-reference, 32-bit offset, source type 7 occur in CM2E16 */
    unsigned applied = 0;
    for (uint32_t k = 0; k < nobj; k++) for (uint32_t j = 0; j < cnt[k]; j++) {
        uint32_t pg = first[k] + j;
        uint32_t a = le + fixrec + u32(h + fixpage + 4 * (pg - 1)), b = le + fixrec + u32(h + fixpage + 4 * pg);
        uint32_t page_lin = base[k] + j * psize;
        while (a < b) {
            uint8_t src = d[a], flg = d[a + 1]; a += 2;
            uint32_t count = 1; int16_t soff = 0;
            if (src & 0x20) { count = d[a]; a += 1; } else { soff = (int16_t)u16(d + a); a += 2; }
            if ((flg & 3) != 0) { free(d); return -6; }            /* imports never occur here */
            uint32_t tobj = (flg & 0x40) ? u16(d + a) : d[a]; a += (flg & 0x40) ? 2 : 1;
            uint32_t toff = 0;
            if ((src & 0x0F) != 2) { if (flg & 0x10) { toff = u32(d + a); a += 4; } else { toff = u16(d + a); a += 2; } }
            uint32_t val = base[tobj - 1] + toff;
            for (uint32_t c = 0; c < count; c++) {
                int32_t so = (src & 0x20) ? (int16_t)u16(d + a + 2 * c) : soff;
                int64_t at = (int64_t)page_lin + so;
                for (int byte = 0; byte < 4; byte++) {                 /* fixups may straddle pages */
                    int64_t p = at + byte;
                    if (p >= base[k] && p < (int64_t)base[k] + size[k]) M[p] = (uint8_t)(val >> (8 * byte));
                }
                applied++;
            }
            if (src & 0x20) a += 2 * count;
        }
    }
    free(d);
    if (getenv("CM_TRACE")) fprintf(stderr, "[load] %s: %u objects, %u fixups\n", path, nobj, applied);
    return 0;
}
