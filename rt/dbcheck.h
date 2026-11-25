/* dbcheck.h - Test Database (see dbcheck.c) */
#pragma once
/* check the database in dir (PLAYERS.DB1, TMDATA.DB1, MGDATA.DB1); ref_dir: the 97/98 database to compare
   with (or NULL); leagues: database.cfg's "leagues =" (NULL or "": all). Returns the number of problems;
   the report's lines in *out (kept until the next call). */
int dbcheck_run(const char *dir, const char *ref_dir, const char *leagues, char ***out, int *nout);
