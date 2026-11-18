/* backup.h - automatic copies of saved games before they are overwritten (see backup.c) */
#pragma once
#include <stddef.h>
extern int g_backup_keep;                         /* backups kept per slot; 0 = off */
void backup_init(const char *game_dir);
void backup_before_write(const char *dos_path);   /* dos.c: game is about to write this file */
void backup_slot_now(int slot, int write, const char *why);  /* copy slot N to saves/<db>/saveN/backups/<date>_<time> now
                                                              (write: the career's database, 0: the chosen one) */
/* shared helpers */
const char *cm_game_dir(void);
int  cm_save_slot(const char *path, int *history); /* "PLDATA3.S16" -> 3 (0 = not a save file) */
int  cm_copy_file(const char *from, const char *to);

/* restoring a backup (Restore Backup on the Restore Game screen) - the chosen database's saves */
struct backup_info {
    char name[24];        /* folder name, YYYY-MM-DD_HHMMSS (first: the list is sorted by it) */
    char when[48];        /* "Mon 9 Nov 2026, 22:41:05" */
    char manager[40];     /* the slot's name in that backup */
    char last[96];        /* the player's last match: "15.9.98  Chelsea 4-0 Lierse (UEFA Cup)" (backup_describe) */
};
int backup_list(int slot, struct backup_info *out, int max);   /* newest first; count */
void backup_describe(int slot, struct backup_info *b);          /* fills b->last ("" if none) */
int backup_restore(int slot, const char *name);                 /* files restored, -1 = failed */
int backup_slot_name(int slot, char *out, size_t n);           /* the slot's name now; 0 = unused */
