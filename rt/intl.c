/* intl.c - Euro 96 in a career that starts in 1995, done the way Championship Manager 2 did it,
 * and the four-year international cycle for years before 1994.
 *
 * 97/98 only ever began in July 1997, with the qualifiers for Euro 2000 still to be drawn (1 February
 * 1998). A career that begins on 16 July 1995 (the CM2 database) is in the second year of the Euro 96
 * cycle: 97/98 plays the last three qualifying rounds into empty groups, and the finals get no
 * qualifiers. CM2 (CM2.EXE, the same engine two years earlier) has the code for this, and the
 * functions in 97/98 are the same ones with this part taken out. On the first day of a career,
 * CM2's European Championship qualifying (0x804b8, the daily routine; 97/98: 0x98140):
 *   - sets the hosts: England for the European Championship (97/98 starts with Holland, for 2000);
 *   - draws the groups, as on 1 February two years before: the European nations except the host,
 *     the weakest (with a little luck, 0x2d504) left out until there are 48, in 6 pots and 8 groups
 *     of 6 (97/98: 54 places in 9 groups), the top two of each group shown as qualifying places;
 *   - plays the first seven of the ten rounds at once, from the teams' ratings (0x80f48), and leaves
 *     the last three for 6 September, 11 October and 15 November 1995 (CM2's dates and calendar:
 *     see cm2_round_dates; 97/98 has the 10th of September, and its calendar moves them to Saturdays);
 *   - doesn't sort the groups by name or put the draw in the news (it's already history).
 * When the groups are over, the winners and the six best runners-up qualify and the two worst
 * runners-up meet in the Euro Championship Qualifying Playoff (CM2's "EQP", which 97/98 dropped:
 * its Euro 2000 rules are nine winners and six runners-up, the three worst out): one match, on
 * 17 January 1996, the winner to the finals in England. The finals then take their sixteen as usual,
 * on CM2's dates (from 8 June 1996).
 *
 * Only the Euro 96 cycle of such a career (the season it began, the second of a Euro cycle) works
 * this way; the qualifiers for Euro 2000 and later are 97/98's own. patches.txt says where each of
 * these is called from. Addresses are the original game's (A() adds 16 MB); the competitions and the
 * league tables have moved (relocations.txt), see COMP() and TABLE().
 */
#include "rt_internal.h"
#include "game_funcs.h"
#include <string.h>

#define A(x) ((uint32_t)(x) + 0x01000000u)
#define DAY    A(0xA4496C)
#define MONTH  A(0xA4496E)
#define YEAR   A(0xA4496F)            /* years since 1900 */
#define ECF_NO A(0xA44945)            /* competition numbers: European Championship Finals, */
#define ECQ_NO A(0xA44946)            /*   ... Qualifying Series */
#define COMP_COUNT A(0xA3AF2C)        /* word */

/* a competition's record (0x7D4 bytes; 0x1172d08 before relocations.txt) and its fields */
#define COMP(i)     (0x04070000u + (uint32_t)(i) * 0x7D4u)
#define C_GROUPS    0x4F              /* groups (league tables) */
#define C_TABLE     0x51              /* its first league table */
#define C_TEAMS     0x58              /* entrants, words */
#define C_NTEAMS    0x148             /*   how many */
#define C_DRAWN     0x20B             /* the draw, words: the groups' teams in turn; a cup's first teams */
#define C_SECOND    0x3EB             /*   a cup's second teams */
#define C_HOST      0x674             /* word */
#define C_ROUNDS    0x676             /* 7 bytes a round: kind, day, month, year, ... */
/* a league table (0x67B bytes; 0x1152698 before), its teams and their places */
#define TABLE(t)    (0x04000002u + (uint32_t)(t) * 0x67Bu)
#define T_TEAMS     2
#define T_PLACES    4                 /* the top places shown as qualifying */
#define T_ENTRY(p)  (8 + 0x42u * (uint32_t)(p))   /* team (word), then +2 its status */
#define ST_QUALIFIED 2
#define ST_PLAYOFF   3
/* the match being played (0x887 bytes): competition, round, home and away team, their goals at +8, +9 */
#define MATCH      A(0xA42B1C)
#define MATCH_SIZE 0x887u

/* the game's own strings */
#define S_ENGLAND  A(0x0F02A0)
#define S_FRANCE   A(0x0E7B80)
#define S_HOLLAND  A(0x0E7B88)
#define S_EMPTY    A(0x0F0454)
#define ECQ_DATES_9798 A(0x0F544C)

/* ours, in free conventional memory (beside overrides.c's Options menu text at 0x28000) */
#define OURS        0x00028200u
#define S_EQP       (OURS + 0x00)
#define S_EQP_SHORT (OURS + 0x08)
#define S_EQP_LONG  (OURS + 0x28)
#define ECQ_DATES   (OURS + 0x58)     /* CM2's qualifying dates (0xbe307), 6 bytes a round */
#define EQP_DATES   (OURS + 0xA0)     /* CM2's play-off date (0xbe300) */
#define PAIR_A      (OURS + 0xB0)     /* the fixture generator's answers */
#define PAIR_B      (OURS + 0xB4)
static void put_ours(void) {
    static const uint8_t ecq_dates[] = {        /* 6/9, 11/10, 15/11, 13/12, 24/4, 1/6, 8/6, 6/9, 11/10, 15/11 */
        0,6,9,0,0,1, 0,11,10,0,0,1, 0,15,11,0,0,1, 0,13,12,0,0,1, 0,24,4,0,0,1, 0,1,6,0,0,1,
        0,8,6,0,0,1, 0,6,9,0,0,1, 0,11,10,0,0,1, 0,15,11,0,0,1, 0xff };
    static const uint8_t eqp_dates[] = { 0x0a, 17, 1, 4, 0, 3, 0xff };   /* 17 January */
    strcpy((char *)&M[S_EQP], "EQP");
    strcpy((char *)&M[S_EQP_SHORT], "E/Champ Qlfying Playoff");
    strcpy((char *)&M[S_EQP_LONG], "Euro Championship Qualifying Playoff");
    memcpy(&M[ECQ_DATES], ecq_dates, sizeof ecq_dates);
    memcpy(&M[EQP_DATES], eqp_dates, sizeof eqp_dates);
}

/* call one of the game's functions: arguments in eax, edx, ebx, ecx and then on the stack (st[0]
   pushed first); ret is the return site of a real call to it. Every register is kept. */
static uint32_t call_game(void (*fn)(void), uint32_t ret, uint32_t a, uint32_t d, uint32_t b, uint32_t c,
                          int nst, const uint32_t *st) {
    uint32_t r[8] = { eax, ebx, ecx, edx, esi, edi, ebp, esp }; uint32_t saved_ret = g_ret;
    for (int i = 0; i < nst; i++) PUSH32(st[i]);
    eax = a; edx = d; ebx = b; ecx = c;
    PUSH32(ret);
    fn();
    uint32_t v = eax;
    eax = r[0]; ebx = r[1]; ecx = r[2]; edx = r[3]; esi = r[4]; edi = r[5]; ebp = r[6]; esp = r[7];
    g_ret = saved_ret;
    return v;
}
#define CALL(fn, ret, a, d, b, c) call_game(fn, ret, (uint32_t)(a), (uint32_t)(d), (uint32_t)(b), (uint32_t)(c), 0, NULL)
static int find_name(uint32_t s, int kind) { return (int16_t)CALL(f_101b970, 0x1096ECFu, s, kind, 0, 0); } /* 0 competition, 2 team */
static void comp_reset(int c)          { CALL(f_10320d0, 0x10981A2u, c, 0, 0, 0); }
static void comp_dates(int c, uint32_t d) { CALL(f_10359c8, 0x1098561u, c, d, 0, 0); }
static int  comp_state(int c)          { return (int8_t)CALL(f_1034330, 0x109753Bu, c, 0, 0, 0); }   /* 2: over */
static void comp_fixture_rounds(int c, int last) { CALL(f_10329c4, 0x10983BAu, c, 0, last, 0); }
static void comp_cup_draw(int c)       { CALL(f_1032be8, 0x1095B19u, c, 1, 0, 0); }
static void qualify(int c, int g, int p)   { CALL(f_106bb7c, 0x1098928u, c, g, p, 0xF); }
static void eliminate(int c, int g, int p) { CALL(f_106bec4, 0x1098964u, c, g, p, 0xF); }
static int  game_rand(int n)           { return (int32_t)CALL(f_101e3c8, 0x1098F12u, n, 0, 0, 0); }
static int  rating(int team)           { return (int16_t)CALL(f_107abdc, 0x109900Bu, team, 0, 0, 0); }

static int season_year(void) {          /* the year the season began (seasons start on 16 July) */
    int y = RD8(YEAR), m = RD8(MONTH), d = RD8(DAY);
    return (m < 7 || (m == 7 && d < 16)) ? y - 1 : y;
}
static int cycle(int season) { return ((season - 94) % 4 + 4) % 4; }   /* 1: Euro qualifiers' 2nd year */
/* the Euro 96 way: this season is the first of the career, and the second of a Euro cycle */
static int e96_mode(void) { return cycle(season_year()) == 1 && season_year() == (int)first_season_year(); }
static int e96_first_day(void) { return e96_mode() && RD8(DAY) == 16 && RD8(MONTH) == 7; }
static int e96_eqp(void) { put_ours(); return find_name(S_EQP, 0); }

/* ---- CM2's calendar ----
 * A competition's round dates come from a list of days and months, moved by a few days to keep the
 * weekday of the year the game was made for (0x35240): 97/98's lists are for 1997, and in 1995 they
 * move three days (Wednesdays become Saturdays). CM2's lists are for 1995, and CM2 moves them by its
 * own table (0x2fa7c): by the weekday of 1 August, 1 day more from March to July in a year that isn't
 * a leap year. For the Euro 96 competitions the dates are worked out again the way CM2 did. */
static int leap(int y) { y += 1900; return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static int month_days(int m, int y) { static const int n[] = {31,28,31,30,31,30,31,31,30,31,30,31}; return m == 2 && leap(y) ? 29 : n[m - 1]; }
static int weekday(int d, int m, int y) {      /* 1 = Sunday .. 7 = Saturday */
    y += 1900; if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100, h = (d + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7 + 1;
}
static void cm2_round_dates(int c, uint32_t list) {
    static const int8_t move[8] = { 0, 2, 1, 0, 6, 5, 4, 3 };   /* CM2 0x2f42c, by weekday */
    uint32_t q = COMP(c); int season = season_year();
    for (int k = 0; RD8(list + 6 * k) != 0xFF && k < 40; k++) {
        int d = RD8(list + 6 * k + 1), m = RD8(list + 6 * k + 2);
        if (!d || m < 1 || m > 12) continue;
        int y = season + (m < 8), s = move[weekday(1, 8, RD8(YEAR))];
        if (!leap(y) && ((d == 29 && m == 2) || (m >= 3 && m <= 7))) s++;
        for (d += s; d > month_days(m, y); ) { d -= month_days(m, y); if (++m > 12) { m = 1; y++; } }
        WR8(q + C_ROUNDS + 7 * k + 1, (uint8_t)d); WR8(q + C_ROUNDS + 7 * k + 2, (uint8_t)m); WR8(q + C_ROUNDS + 7 * k + 3, (uint8_t)y);
    }
}
/* patches.txt 09855c, 0974d2: after the qualifiers' and the finals' dates are set on the first day */
void hook_ecq_dates(void) { if (e96_first_day()) cm2_round_dates(RD8(ECQ_NO), ECQ_DATES); }
void hook_ecf_dates(void) { if (e96_first_day()) cm2_round_dates(RD8(ECF_NO), A(0x0F538C)); }

/* ---- the four-year cycle (patches.txt 0329bb) ----
 * 0x3296c gives (season - 94) mod 4 with a signed divide: -3 for 1991 instead of 1, -1 for 1993
 * instead of 3. Rounded down instead, so the years before 1994 come round like the ones after. */
void hook_cycle_divide(void) {
    int32_t n = (int32_t)eax, d = (int32_t)ebx, q = n / d, r = n % d;
    if (r < 0) { r += d; q--; }
    eax = (uint32_t)q; edx = (uint32_t)r;
}

/* ---- the first day: draw the qualifying groups (patches.txt 09816c, 09817c, 09818c) ----
 * The draw's tests (the cycle's last year, 1 February) compare with what they read instead. */
uint32_t ecq_first_day(void) { return (uint32_t)e96_first_day(); }

/* how many places the draw has (patches.txt 0981b1, 09828b); in the Euro 96 way it sets the host and
   the groups first (CM2 at 0x80562, 0x803e8), and a later draw has 97/98's nine groups again */
uint32_t ecq_draw_places(void) {
    uint32_t q = COMP(RD8(ECQ_NO));
    if (e96_mode()) {
        if (RD8(q + C_GROUPS) != 8) {
            WR8(q + C_GROUPS, 8);
            int england = find_name(S_ENGLAND, 2);
            if (england >= 0) WR16(COMP(RD8(ECF_NO)) + C_HOST, (uint16_t)england);
        }
        return 48;
    }
    if (RD8(q + C_GROUPS) == 8) WR8(q + C_GROUPS, 9);
    return 0x36;
}
uint32_t ecq_two_places(void) { return (uint32_t)e96_mode(); }   /* patches.txt 0981f2: two shown, not one */

/* the weakest of the entrants, with some luck (CM2 0x2d504) */
static int weakest(uint32_t q) {
    int n = RD8(q + C_NTEAMS), pick = -1, low = 0;
    for (int i = 0; i < n; i++) {
        int luck = game_rand(game_rand(3) ? 3 : 4);
        int s = (int8_t)(rating((int16_t)RD16(q + C_TEAMS + 2 * i)) / 10 + luck);
        if (s < low || i == 0) { pick = i; low = s; }
    }
    return pick;
}
/* patches.txt 098287: the entrants are in; leave the weakest out until there are 48 (CM2 0x806b7) */
void hook_ecq_entrants(void) {
    if (!e96_mode()) return;
    uint32_t q = COMP(RD8(ECQ_NO));
    while (RD8(q + C_NTEAMS) > 48) {
        int n = RD8(q + C_NTEAMS), w = weakest(q);
        if (w < 0) break;
        WR16(q + C_TEAMS + 2 * w, RD16(q + C_TEAMS + 2 * (n - 1)));
        WR8(q + C_NTEAMS, (uint8_t)(n - 1));
    }
}
/* patches.txt 0984dd, 0984ea: not on the first day (CM2 0x808ec) */
void hook_ecq_sort_by_name(void) { if (!e96_first_day()) CALL(f_106a428, 0x10984E2u, eax, edx, ebx, ecx); }
void hook_ecq_draw_news(void)    { if (!e96_first_day()) CALL(f_106a704, 0x10984EFu, eax, edx, ebx, ecx); }

/* ---- the first day: the first seven rounds, played at once (CM2 0x80f48; patches.txt 098554) ----
 * Every group's fixtures come from the game's fixture generator as they will for the rounds still to
 * play; a match of the first seven gets a result from the two teams' ratings (tenths): the home side
 * scores 0 to 2 goals, more as it's better (up to 6), the away side 0 to 1, more as it's better (up
 * to 5). Each result goes into the tables as a real match's would (0x2401c), then they are sorted. */
static void e96_play_first_rounds(void) {
    int c = RD8(ECQ_NO); uint32_t q = COMP(c);
    int first = (int8_t)RD8(q + C_TABLE), groups = RD8(q + C_GROUPS), off = 0;
    static uint8_t keep[MATCH_SIZE];
    memcpy(keep, &M[MATCH], MATCH_SIZE);
    for (int g = 0; g < groups; g++) {
        int n = (int8_t)RD8(TABLE(first + g) + T_TEAMS);
        CALL(f_1044658, 0x10985F7u, n, 0, 0, 0);                      /* the fixture generator */
        for (int round = 0; round < 2 * n - 2; round++) {
            for (int m = 1; m <= n / 2; m++) {
                CALL(f_104480c, 0x109862Bu, PAIR_A, PAIR_B, 0, 0);
                if (round + 1 > 7) continue;
                int home = (int16_t)RD16(q + C_DRAWN + 2 * (off + (int8_t)RD8(PAIR_A)));
                int away = (int16_t)RD16(q + C_DRAWN + 2 * (off + (int8_t)RD8(PAIR_B)));
                memset(&M[MATCH], 0, MATCH_SIZE);
                WR8(MATCH, (uint8_t)c); WR8(MATCH + 1, (uint8_t)round);
                WR16(MATCH + 2, (uint16_t)home); WR16(MATCH + 4, (uint16_t)away);
                int h = (int8_t)(rating(home) / 10), a = (int8_t)(rating(away) / 10);
                int hb = h - a < 0 ? 0 : h - a > 4 ? 4 : h - a, ab = a - h < 0 ? 0 : a - h > 4 ? 4 : a - h;
                WR8(MATCH + 8, (uint8_t)game_rand(hb + 3));
                WR8(MATCH + 9, (uint8_t)game_rand(ab + 2));
                WR8(q + C_ROUNDS + 7 * round, 0);
                CALL(f_102401c, 0x10534D2u, 1, 0, 0, 0);                  /* the result into the tables */
            }
            CALL(f_106a028, 0x1010525u, 0, 0, 0, 0);                      /* sort the tables */
        }
        off += n;
    }
    memcpy(&M[MATCH], keep, MATCH_SIZE);
}
/* the dates for the rounds to come: CM2's on the first day (after playing the first seven) */
uint32_t ecq_round_dates(void) {
    if (!e96_first_day()) return ECQ_DATES_9798;
    put_ours();
    e96_play_first_rounds();
    return ECQ_DATES;
}

/* ---- the groups are over: the two worst runners-up to the play-off (patches.txt 098b33, 098bb4) ----
 * 97/98 picks the three worst runners-up and puts them out; CM2 picks two and gives them the play-off
 * place (0x5d1b0). The rest qualify, in both. */
uint32_t ecq_third_worst(void) { return !e96_mode(); }
void hook_ecq_worst_runner_up(void) {          /* eax: competition, edx: group, ebx: place (1) */
    if (!e96_mode()) { CALL(f_106bec4, 0x1098BB9u, eax, edx, ebx, ecx); return; }
    uint32_t q = COMP((int8_t)eax), e = TABLE((int8_t)RD8(q + C_TABLE) + (int8_t)edx) + T_ENTRY((int8_t)ebx);
    int st = RD8(e + 2);
    if (st != 1 && st != ST_QUALIFIED && st != ST_PLAYOFF) WR8(e + 2, ST_PLAYOFF);
}

/* ---- the play-off (CM2 0x7ff78 makes it, 0x800b0 runs it) ----
 * Made at new game, for a career that starts in the second year of a Euro cycle, after all the
 * others (0x37238 makes them; competitions are numbered in turn, and the clubs keep their European
 * places by number, so it goes on the end rather than after the qualifiers, as in CM2).
 * patches.txt 037257: the making starts (cl: the mode, as the others get it: 2 sets the rules);
 * 037bb8: the competitions are all made (or read from a save, which already has it). */
static int making = 0, making_mode = 0;
void hook_comps_begin(void) { making = 1; making_mode = (int8_t)esi; }
static void create_eqp(int mode) {
    int y = (int)start_year_byte(), m = (int)start_month(), d = (int)start_day();
    if (m < 7 || (m == 7 && d < 16)) y--;
    if (cycle(y) != 1) return;
    put_ours();
    int c = (int16_t)RD16(COMP_COUNT); uint32_t q = COMP(c);
    memset(&M[q], 0, 0x7D4);
    WR32(q + 0x0, CALL(f_101b704, 0x1097FE2u, S_EQP, 0, c, 0));
    WR32(q + 0x4, CALL(f_101b704, 0x1097FE2u, S_EQP, 0, c, 0));
    WR32(q + 0x8, CALL(f_101b704, 0x1097FE2u, S_EQP_SHORT, 0, c, 0));
    WR32(q + 0xC, CALL(f_101b704, 0x1097FE2u, S_EQP_LONG, 0, c, 0));
    WR8(q + 0x10, RD8(S_EMPTY));
    if (mode == 2) {                                      /* CM2's values (97/98's play-off has 0xf at +0x1d) */
        static const uint8_t v[][2] = { {0x1C,3}, {0x1D,0xD}, {0x1E,0x14}, {0x1F,0}, {0x40,5}, {0x41,3},
                                        {0x42,0x10}, {0x43,0x10}, {0x44,0x12} };
        for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) WR8(q + v[i][0], v[i][1]);
        WR16(q + 0x4B, 0x82); WR16(q + 0x4D, 0x9C);
    }
    WR16(COMP_COUNT, (uint16_t)(c + 1));
}
void hook_comps_end(void) { if (making) create_eqp(making_mode); making = 0; }
/* patches.txt 031ef0: each day, after the qualifiers' */
void hook_eqp_daily(void) {
    if (!e96_mode()) return;
    int p = e96_eqp(); if (p < 0) return;
    int c = RD8(ECQ_NO); uint32_t q = COMP(c), pq = COMP(p);
    int first = (int8_t)RD8(q + C_TABLE), groups = RD8(q + C_GROUPS), drawn = 0;
    if (RD8(DAY) == 16 && RD8(MONTH) == 7) { comp_reset(p); comp_dates(p, EQP_DATES); cm2_round_dates(p, EQP_DATES); }
    if (comp_state(c) == 2) {                             /* the groups are over: the two in */
        for (int g = 0; g < groups; g++) {
            uint32_t e = TABLE(first + g) + T_ENTRY(1);
            if (RD8(e + 2) != ST_PLAYOFF) continue;
            int k = RD8(pq + C_NTEAMS);
            WR16(pq + C_TEAMS + 2 * k, RD16(e)); WR8(pq + C_NTEAMS, (uint8_t)(k + 1));
        }
        comp_fixture_rounds(p, 1);
        drawn = 1;
    }
    if (drawn) {                                          /* the match, on its day */
        comp_cup_draw(p);
        uint32_t st[7] = { S_EMPTY, 1, (uint32_t)(int16_t)RD16(pq + C_SECOND), (uint32_t)(int16_t)RD16(pq + C_DRAWN), 0, (uint32_t)p, 0 };
        call_game(f_10411b8, 0x109875Du, RD8(pq + C_ROUNDS + 1), RD8(pq + C_ROUNDS + 2), RD8(pq + C_ROUNDS + 3), 0, 7, st);
    } else if (comp_state(p) == 2) {                      /* played: the winner goes through */
        for (int g = 0; g < groups; g++) {
            uint32_t e = TABLE(first + g) + T_ENTRY(1);
            if (RD16(e) == RD16(pq + C_DRAWN)) qualify(c, g, 1);
            else if (RD8(e + 2) == ST_PLAYOFF) eliminate(c, g, 1);
        }
    }
}
/* patches.txt 09752e: the finals take their teams when the qualifiers are over - in the Euro 96
   way, when the play-off is (CM2 0x7f46a). As the dword it replaces: the number in the top byte. */
uint32_t ecf_entry_source(void) {
    int p = e96_mode() ? e96_eqp() : -1;
    return p >= 0 ? (uint32_t)p << 24 : RD32(A(0xA44943));
}

/* ---- the next hosts (patches.txt 03270a) ----
 * After a final, 0x326c8 takes the next host from a list for the finals from 2002 ((year - 96) / 2 - 1).
 * A final before 1998 read in front of it (no list there, garbage): Euro 96 gives Euro 2000 to
 * Holland (as 97/98 had it), the 1994 World Cup gives 1998 to France, Euro 92 gives 1996 to England. */
uint32_t next_host_name(void) {
    if ((int32_t)eax >= 1) return RD32(0xC4u + esp + eax * 4u);
    int y = RD8(YEAR);
    return y >= 96 ? S_HOLLAND : y >= 94 ? S_FRANCE : S_ENGLAND;
}
