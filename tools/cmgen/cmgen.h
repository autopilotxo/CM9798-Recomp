#pragma once
#include <stddef.h>
/* Make gen/ (the C translation of the game's code) from the player's own CM2E16.EXE v2.93.
   datadir holds functions.json, labels.csv, patches.txt, overrides.txt and relocations.txt.
   progress (may be NULL) gets 0..1000. Returns 0, or -1 with a message in err. */
int cmgen_run(const char *exe, const char *datadir, const char *outdir,
              void (*progress)(int permille, const char *what, void *ctx), void *ctx,
              char *err, size_t errn);
#define CMGEN_EXE_SIZE  1407137u
#define CMGEN_EXE_CRC32 0x359E908Eu
