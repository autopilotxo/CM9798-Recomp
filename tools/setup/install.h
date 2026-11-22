/* install.h - sets up a folder for the CM 97/98 port from a player's own game (see install.c) */
#pragma once
#include <stddef.h>

typedef void (*install_log_fn)(const char *text);   /* progress text, may hold several lines */

/* CM2's league editions: six 1995/96 ones (the European Leagues CD) and two 1996/97 releases, and
   Liga Portuguesa a second time, for the 1995/96 database */
#define INSTALL_NEDITIONS 9

struct install_opts {
    char src[1100];        /* the player's CM 97/98 folder (read only) */
    char dst[1100];        /* the folder to set up */
    char cm2[1100];        /* Championship Manager 2 folders to import (either edition in either), or "" */
    char cm96[1100];
    char euro[INSTALL_NEDITIONS][1100];   /* CM2's league editions, in install_edition()'s order ("" skipped): each
                                             goes into the import of its season; any other edition in a slot goes
                                             into its own season's */
    char port_dir[1100];   /* where cm9798(.exe) and its libraries are, to copy along, or "" */
};

/* is this a CM 97/98 v2.93 folder? 1 = yes; msg says what was found either way */
int install_check_source(const char *dir, char *msg, size_t n);
/* is this a Championship Manager 2 folder? 1 = yes */
int install_check_cm2(const char *dir, char *msg, size_t n);
/* is this one of CM2's league editions? 1 = yes; msg names it and its season */
int install_check_euro(const char *dir, char *msg, size_t n);
struct install_edition {
    const char *name;        /* "the French edition", "Italian Leagues", "Liga Portuguesa" */
    const char *adjective;   /* "French" (its leagues) */
    const char *country;     /* "France", as cm2_edition_country() says */
    int season;              /* the database it adds to: 1995 (Championship Manager 2) or 1996 (the 96/97 Update) */
    int release;             /* the release it takes (install_euro_release): its own, but Liga Portuguesa (1996/97)
                                also goes into the 1995/96 database */
};
const struct install_edition *install_edition(int k);
/* which edition a folder holds, by its files (an index for install_edition), or -1 */
int install_euro_release(const char *dir);
/* does this folder already have the port set up (databases/, saves/)? */
int install_is_port_folder(const char *dir);
/* already set up with the 97/98 database? Then the 97/98 folder can be left out, to only import CM2 data */
int install_has_base(const char *dir);
/* the same folder (or one inside the other)? */
int install_same_folder(const char *a, const char *b);
/* do it all; 0 = done, -1 = failed (the reason was logged) */
int install_run(const struct install_opts *o, install_log_fn log);
