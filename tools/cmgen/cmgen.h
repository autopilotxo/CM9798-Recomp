#pragma once
#include <stddef.h>
/* Make gen/ (the C translation of the game's code) from the player's own CM2E16.EXE v2.93 (or the editor's
   from CM2ED.EXE, below).
   datadir holds functions.json, labels.csv, patches.txt, overrides.txt and relocations.txt.
   progress (may be NULL) gets 0..1000. Returns 0, or -1 with a message in err. */
int cmgen_run(const char *exe, const char *datadir, const char *outdir,
              void (*progress)(int permille, const char *what, void *ctx), void *ctx,
              char *err, size_t errn);
#define CMGEN_EXE_SIZE  1407137u
#define CMGEN_EXE_CRC32 0x359E908Eu
/* ...or the data editor that came with the game (CM2ED.EXE v2.00): the same, with its own names
   (e_XXXXXX, e_funcs, e_retsites) and file names (edfuncs.h, ed_NNN.c, edtable.c, edretsites.c),
   so both can be built into one program */
#define CMGEN_ED_SIZE   389163u
#define CMGEN_ED_CRC32  0x11A01E20u
