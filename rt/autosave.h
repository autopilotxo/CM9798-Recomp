/* autosave.h - weekly autosaves into autosaves/slotN (see autosave.c) */
#pragma once
#include <stddef.h>
extern int g_autosave;                                   /* 1 = on (cm9798.cfg: autosave = on/off) */
int  autosave_active(void);                              /* the game is writing an autosave right now */
int  autosave_redirect(const char *dos_path, char *host, size_t n);   /* dos.c, before writing */
void autosave_before_read(const char *dos_path);         /* dos.c, before opening for reading */
