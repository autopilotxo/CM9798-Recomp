/* game_funcs.h - the game's functions the port's code (rt/) calls or replaces, and the hooks patches.txt
 * calls. These are only names: the code itself is made from each player's CM2E16.EXE (gen/, whose
 * funcs.h declares all of them). This header lets rt/ be built without gen/, e.g. for a release.
 * Adding an override or a patches.txt hook? Declare it here too (the build says so if it's missing). */
#pragma once
#include "rt.h"

/* hooks called from patches.txt (hook_*: void; others return a value) */
uint32_t ecf_entry_source(void);  /* patches.txt, rt/ */
uint32_t ecq_draw_places(void);  /* patches.txt, rt/ */
uint32_t ecq_first_day(void);  /* patches.txt, rt/ */
uint32_t ecq_round_dates(void);  /* patches.txt, rt/ */
uint32_t ecq_two_places(void);  /* patches.txt, rt/ */
uint32_t ecq_third_worst(void);  /* patches.txt, rt/ */
void hook_comps_begin(void);  /* patches.txt, rt/ */
void hook_comps_end(void);  /* patches.txt, rt/ */
void hook_cycle_divide(void);  /* patches.txt, rt/ */
void hook_ecq_entrants(void);  /* patches.txt, rt/ */
void hook_ecq_worst_runner_up(void);  /* patches.txt, rt/ */
void hook_ecf_dates(void);  /* patches.txt, rt/ */
void hook_ecq_dates(void);  /* patches.txt, rt/ */
void hook_ecq_draw_news(void);  /* patches.txt, rt/ */
void hook_ecq_sort_by_name(void);  /* patches.txt, rt/ */
void hook_eqp_daily(void);  /* patches.txt, rt/ */
uint32_t next_host_name(void);  /* patches.txt, rt/ */
uint32_t calendar_old_buffer(void);  /* patches.txt, rt/ */
uint32_t calendar_pack(void);  /* patches.txt, rt/ */
uint32_t competition_menu_tail(void);  /* patches.txt, rt/ */
uint32_t transfers_menu_tail(void);  /* patches.txt, rt/ */
uint32_t player_limit(void);  /* patches.txt, rt/ */
uint32_t first_season_year(void);  /* patches.txt, rt/ */
uint32_t fixtures_old_buffer(void);  /* patches.txt, rt/ */
uint32_t fixtures_pack(void);  /* patches.txt, rt/ */
void hook_change_database(void);  /* patches.txt, rt/ */
void hook_check_leagues(void);  /* patches.txt, rt/ */
void hook_commentary_event(void);  /* patches.txt, rt/ */
void hook_competition_choice(void);  /* patches.txt, rt/ */
void hook_transfers_enter(void);  /* patches.txt, rt/ */
void hook_transfers_choice(void);  /* patches.txt, rt/ */
void hook_select_team_layout_args(void);  /* patches.txt, rt/ */
void hook_select_team_layout_pop(void);  /* patches.txt, rt/ */
void hook_crowd_start(void);  /* patches.txt, rt/ */
void hook_crowd_stop(void);  /* patches.txt, rt/ */
void hook_intro_done(void);  /* patches.txt, rt/ */
void hook_leagues_message(void);  /* patches.txt, rt/ */
void hook_load_calendar(void);  /* patches.txt, rt/ */
void hook_load_club_comps(void);  /* patches.txt, rt/ */
void hook_load_fixtures(void);  /* patches.txt, rt/ */
void hook_load_humans(void);  /* patches.txt, rt/ */
void hook_load_more_tables(void);  /* patches.txt, rt/ */
void hook_load_tmdata_begin(void);  /* patches.txt, rt/ */
void hook_match_speed_kickoff(void);  /* patches.txt, rt/ */
void hook_match_speed_toggled(void);  /* patches.txt, rt/ */
void hook_options_choice(void);  /* patches.txt, rt/ */
void hook_player_position(void);  /* patches.txt, rt/ */
void hook_save_calendar(void);  /* patches.txt, rt/ */
void hook_save_humans(void);  /* patches.txt, rt/ */
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
void f_101b704(void); /* register a name, return its copy */
void f_101b970(void); /* find a name: competition (edx 0), team (2) */
void f_101e3c8(void); /* random number below eax */
void f_102401c(void); /* a match's result into the league tables */
void f_10320d0(void); /* start a competition afresh */
void f_10329c4(void); /* a competition's fixture rounds */
void f_1032be8(void); /* a cup's draw */
void f_1034330(void); /* competition state (2: over) */
void f_10359c8(void); /* a competition's round dates */
void f_10411b8(void); /* add a fixture */
void f_1044658(void); /* league fixture generator: start */
void f_104480c(void); /* league fixture generator: next match */
void f_106a028(void); /* sort the league tables */
void f_106a428(void); /* sort a competition's groups by name */
void f_106a704(void); /* the draw in the news */
void f_106bb7c(void); /* a team qualifies from a group */
void f_106bec4(void); /* a team is out of a group */
void f_107abdc(void); /* a team's rating */
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
