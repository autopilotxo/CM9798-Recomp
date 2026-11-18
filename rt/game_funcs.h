/* game_funcs.h - the game's functions the port's code (rt/) calls or replaces, and the hooks patches.txt
 * calls. These are only names: the code itself is made from each player's CM2E16.EXE (gen/, whose
 * funcs.h declares all of them). This header lets rt/ be built without gen/, e.g. for a release.
 * Adding an override or a patches.txt hook? Declare it here too (the build says so if it's missing). */
#pragma once
#include "rt.h"

/* hooks called from patches.txt (hook_*: void; others return a value) */
uint32_t calendar_old_buffer(void);  /* patches.txt, rt/ */
uint32_t calendar_pack(void);  /* patches.txt, rt/ */
uint32_t competition_menu_tail(void);  /* patches.txt, rt/ */
uint32_t first_season_year(void);  /* patches.txt, rt/ */
uint32_t fixtures_old_buffer(void);  /* patches.txt, rt/ */
uint32_t fixtures_pack(void);  /* patches.txt, rt/ */
void hook_change_database(void);  /* patches.txt, rt/ */
void hook_check_leagues(void);  /* patches.txt, rt/ */
void hook_commentary_event(void);  /* patches.txt, rt/ */
void hook_competition_choice(void);  /* patches.txt, rt/ */
void hook_crowd_start(void);  /* patches.txt, rt/ */
void hook_crowd_stop(void);  /* patches.txt, rt/ */
void hook_intro_done(void);  /* patches.txt, rt/ */
void hook_leagues_message(void);  /* patches.txt, rt/ */
void hook_load_calendar(void);  /* patches.txt, rt/ */
void hook_load_club_comps(void);  /* patches.txt, rt/ */
void hook_load_fixtures(void);  /* patches.txt, rt/ */
void hook_load_more_tables(void);  /* patches.txt, rt/ */
void hook_load_tmdata_begin(void);  /* patches.txt, rt/ */
void hook_match_speed_kickoff(void);  /* patches.txt, rt/ */
void hook_match_speed_toggled(void);  /* patches.txt, rt/ */
void hook_options_choice(void);  /* patches.txt, rt/ */
void hook_player_position(void);  /* patches.txt, rt/ */
void hook_save_calendar(void);  /* patches.txt, rt/ */
void hook_save_club_comps(void);  /* patches.txt, rt/ */
void hook_save_more_tables(void);  /* patches.txt, rt/ */
void hook_save_tmdata_begin(void);  /* patches.txt, rt/ */
void hook_text_glyph(void);  /* patches.txt, rt/ */
void hook_trim_histories(void);  /* patches.txt, rt/ */
uint32_t match_speed_divisor(void);  /* patches.txt, rt/ */
uint32_t max_leagues(void);  /* patches.txt, rt/ */
uint32_t options_menu_tail(void);  /* patches.txt, rt/ */
uint32_t start_day(void);  /* patches.txt, rt/ */
uint32_t start_month(void);  /* patches.txt, rt/ */
uint32_t start_weekday(void);  /* patches.txt, rt/ */
uint32_t start_year_byte(void);  /* patches.txt, rt/ */

/* the game's own functions used here (original address + 0x1000000) */
void f_10106f4(void);
void f_1013714(void);
void f_101a68c(void);
void f_101dfdc(void); /* cm_fread */
void f_101e028(void); /* cm_fwrite */
void f_1036704(void); /* load_games_idx */
void f_1044c94(void);
void f_1045b48(void);
void f_10bdc64(void);

/* overridden in rt/: the generated originals, still callable */
void f_10106f4_orig(void);
void f_1013714_orig(void);
void f_101a68c_orig(void);
void f_1044c94_orig(void);
