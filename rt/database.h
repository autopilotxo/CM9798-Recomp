/* database.h - one folder per game database under databases/ (see database.c) */
#pragma once
#include <stddef.h>
void        db_set_choice(const char *dir);   /* cm9798.cfg "database = <folder>" */
const char *db_choice(void);                  /* folder of the database chosen for new games */
const char *db_name(void);                    /* its name, e.g. "97/98" */
int         db_start_year(void);              /* the year its first season starts */
int         db_start_day(void);               /* the day and month a new game starts (16, 7 for 97/98) */
int         db_start_month(void);
int         db_count(void);
int         db_players(void);                 /* players in the chosen database's PLAYERS.DB1 (-1: unknown) */
void        db_next(void);                    /* the next one (headless tests) */
int         db_selected(void);                /* index of the chosen one (-1: none found) */
int         db_info(int i, const char **name, int *day, int *month, int *year);   /* database i; 0 = no such */
void        db_select(int i);                 /* Choose Database: use database i for new games */
const char *db_folder(int i);                 /* database i's folder under databases/ (NULL: no such) */
const char *db_leagues_of(int i);             /* its database.cfg "leagues =" ("": all) */
int         db_in_frontend(void);             /* in the front end (Options, New/Restore Game screens)? */
void        db_frontend(int in);              /* 1 while in the front end (0x44C94), where the choice applies */
int         db_redirect(const char *dos_path, char *host, size_t n);   /* dos.c: a database file? its path */
/* saved games: saves/<database>/GAMESS16.IDX and saves/<database>/save<N>/ (made if missing).
   write = 1: where the game being played saves; 0: where the front end reads (the chosen database) */
int cm_saves_dir(int write, char *out, size_t n);
int cm_slot_dir(int slot, int write, char *out, size_t n);
int save_redirect(const char *dos_path, char *host, size_t n, int write);   /* dos.c */
int db_league_allowed(const char *label);   /* "English League": allowed by database.cfg leagues = ...? */
int pic_redirect(const char *dos_path, char *host, size_t n);   /* dos.c: .PCX from databases/<db>/pictures */
int db_own_sounds_dir(char *out, size_t n);   /* sound.c: database.cfg "sounds = own": its sounds folder */
int cm2_backgrounds_available(void);    /* are CM2's STADPIC1-5 anywhere (a CM2 database's pictures, the game folder)? */
extern int g_cm2_backgrounds;                 /* cm9798.cfg "backgrounds = cm2" (F1, Graphics): CM2's pictures */
