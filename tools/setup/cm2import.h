/* cm2import.h - Championship Manager 2 -> databases/9596 (see cm2import.c) */
#pragma once
typedef void (*cm2_log_fn)(const char *text);      /* progress text, may hold several lines */
/* 0 = done, -1 = failed (the reason was passed to log) */
/* pictures_from: another CM2 folder (or NULL) - club pictures this one lacks are taken from it.
   addons: 96/97's European Leagues editions (French, Italian, Spanish, German, Dutch, Portuguese,
   Belgian; empty strings skipped) - each one's country, in full, replaces the English edition's
   and its league becomes playable */
int cm2_import(const char *cm2_folder, const char *game_folder, const char *pictures_from,
               const char *const *addons, int naddons, cm2_log_fn log);
/* which season a CM2 folder holds: the year its first season starts, and the day and month a new
   game starts (1 August). 0 if it can't tell. */
int cm2_detect(const char *dir, int *year, int *day, int *month);
/* "Championship Manager 2 (1995/96) found - goes in databases/9596." */
void cm2_describe(const char *dir, char *out, size_t n);
/* which country one of 96/97's European Leagues editions has in full ("France", ...), or NULL */
const char *cm2_edition_country(const char *dir);
