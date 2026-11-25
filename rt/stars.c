/* stars.c - a star rating for each player, shown on his player screen (in the blue position bar),
 * like the modern games' star ratings. 1-10, as stars: 8 = four stars, 7 = three and a half.
 *
 * The rating copies how the match engine values a player (port-analysis notes, Nov 2026): every
 * skill it works out in a match is about half the player's overall ability and half the stats
 * involved. So the rating is half his ability (0-200 -> 0-10) and half his stats (1-20 -> 0-10),
 * the stats weighted by how much the engine used each one for players of his position in test
 * matches. A player natural in several positions gets the best of them. Rounded to the nearest
 * whole number (7.4 -> 7, 7.5 -> 8).
 *
 * "Your club" mode (the default; F1, Gameplay: Star ratings), like the newer Football Manager games:
 * the stars say how good the player would be for the human manager's club. The reference is the
 * club's first team, the average rating of its best eleven: a player as good as that gets 3.5 stars,
 * and every point of rating above or below it is 1.5 stars more or less (half stars, 0.5 to 5). So a
 * Premier League regular is five stars to a Division Two club, and a Division Two regular half a star
 * to Manchester United. Without a club (no manager, unemployed, a squad under five) or with more than
 * one human manager (hot-seat) it's the overall rating above.
 */
#include "rt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define PLAYERS 0x06000000u          /* the player table (0x11f3de0 in the game; relocations.txt moves it) */
#define PLAYER_SIZE 0x168u
/* stats, in the order they're stored from +0xEA */
enum { AGG, BIG, CHA, HID, CON, CRE, DET, DIR, DRI, FLA, HEA, INF, INJ, ADA, OTB, PAC, PAS, POS, SET, SHO, STA, STR, TAC, TEC };
/* weights per position (percent): GK, SW, D, DM, M, AM, F. For goalkeepers Tackling is Shot
   Stopping, Pace is Rushing Out and Heading is Aerial Ability. */
static const struct { int stat, w[7]; } weights[] = {
    /*            GK  SW   D  DM   M  AM   F */
    { TEC, {      11, 16, 14, 15, 15, 16, 16 } },
    { PAS, {      16, 13, 12, 11, 11, 10, 10 } },
    { TAC, {      29, 12, 11, 11, 11, 10, 10 } },
    { POS, {      11, 10, 10, 10,  9,  9,  9 } },
    { DRI, {       9, 10, 10,  9,  9,  8,  8 } },
    { HEA, {      10,  7,  8,  8,  7,  6,  8 } },
    { CRE, {       0,  8,  8,  8, 10, 10,  8 } },
    { PAC, {       5,  7,  7,  7,  7,  8,  8 } },
    { SET, {       0,  5,  5,  5,  6,  6,  4 } },
    { STR, {       2,  4,  4,  4,  4,  5,  5 } },
    { ADA, {       4,  4,  4,  4,  4,  3,  3 } },
    { OTB, {       0,  3,  2,  3,  3,  5,  5 } },
    { FLA, {       0,  0,  0,  0,  0,  2,  3 } },
    { DET, {       2,  2,  2,  1,  1,  1,  2 } },
    { AGG, {       1,  1,  1,  1,  1,  1,  1 } },
};

static double rate_as(uint32_t p, int pos) {
    double sum = 0, wsum = 0;
    for (unsigned i = 0; i < sizeof weights / sizeof weights[0]; i++) {
        int w = weights[i].w[pos]; if (!w) continue;
        sum += w * (double)RD8(p + 0xEA + weights[i].stat); wsum += w;
    }
    double stats = wsum > 0 ? sum / wsum / 2.0 : 0;               /* 1-20 -> 0.5-10 */
    double ability = (int16_t)RD16(p + 0x1A) / 20.0;              /* 0-200 -> 0-10 */
    return (stats + ability) / 2;
}

static double player_value(uint32_t player) {                   /* the overall rating, 0-10 unrounded */
    uint32_t p = PLAYERS + player * PLAYER_SIZE;
    uint8_t a = RD8(p + 0x17), b = RD8(p + 0x18);
    int r[7] = { a & 3, (a >> 2) & 3, (a >> 4) & 3, (a >> 6) & 3, b & 3, (b >> 2) & 3, (b >> 4) & 3 };
    int best = 0; for (int i = 0; i < 7; i++) if (r[i] > best) best = r[i];
    double v = 0;
    for (int i = 0; i < 7; i++) if (best == 0 || r[i] == best) { double x = rate_as(p, i); if (x > v) v = x; }
    return v;
}

int g_star_mode = 2;                  /* 0 off, 1 overall, 2 for your club (cm9798.cfg "stars", F1 menu) */
__attribute__((constructor)) static void star_env(void) {
    const char *e = getenv("CM_STARMODE"); if (e) g_star_mode = atoi(e);
}
#define MANAGERS     0x011EFDB0u      /* manager records, 0x27 bytes; the club (i16) at +12 */
#define N_MANAGERS   0x01A3AF30u      /* u16 */
#define N_HUMANS     0x01A4495Au      /* byte: the human managers are the last ones in the list */
#define CLUBS        0x01585260u      /* club records, 0x5BA bytes */
#define CLUB_SQUAD   0xDAu            /* i16 player ids */
#define CLUB_NSQUAD  0x120u           /* byte: how many */
#define N_CLUBS      0x01A3AF6Au      /* u16 */
/* the human manager's club, or -1: also with more than one human manager (a hot-seat game), where
   the overall rating is shown instead, as nobody's club is "yours" */
static int human_club(void) {
    int n = RD16(N_MANAGERS), h = RD8(N_HUMANS);
    if (h != 1 || h > n) return -1;
    int c = (int16_t)RD16(MANAGERS + (uint32_t)(n - 1) * 0x27u + 12);
    return c >= 0 && c < RD16(N_CLUBS) ? c : -1;
}
/* the club's first-team level: the average rating of its best eleven (fewer if it has fewer); -1 if
   the squad is too small to say */
static double club_level(int c) {
    uint32_t b = CLUBS + (uint32_t)c * 0x5BAu;
    int n = RD8(b + CLUB_NSQUAD), np = RD16(0x1A3AF32u);
    if (n > 40) n = 40;
    double v[40]; int k = 0;
    for (int i = 0; i < n; i++) {
        int pl = (int16_t)RD16(b + CLUB_SQUAD + 2u * (uint32_t)i);
        if (pl >= 0 && pl < np) v[k++] = player_value((uint32_t)pl);
    }
    if (k < 5) return -1;
    for (int i = 1; i < k; i++) for (int j = i; j > 0 && v[j] > v[j - 1]; j--) { double t = v[j]; v[j] = v[j - 1]; v[j - 1] = t; }
    int top = k < 11 ? k : 11; double sum = 0;
    for (int i = 0; i < top; i++) sum += v[i];
    return sum / top;
}

/* the player's rating 1-10 (0 = no such player): half stars, 10 = five */
int player_stars(uint32_t player) {
    if (player >= RD16(0x1A3AF32u)) return 0;                     /* number of players */
    double v = player_value(player), x = v;
    if (g_star_mode == 2) {
        int c = human_club(); double ref = c >= 0 ? club_level(c) : -1;
        if (ref >= 0) x = 7.0 + 3.0 * (v - ref);                  /* 3.5 stars at the first team's level */
    }
    int s = (int)floor(x + 0.5);
    return s < 1 ? 1 : s > 10 ? 10 : s;
}

/* the player screen's position bar (patches.txt): which player, and the bar's text */
int g_star_player = -1;
char g_star_text[64];
void hook_player_position(void) {                 /* eax: the position text; esi: the player */
    g_star_player = (int)(int16_t)esi;
    size_t k = 0; for (; k + 1 < sizeof g_star_text && RD8(eax + k); k++) g_star_text[k] = (char)RD8(eax + k);
    g_star_text[k] = 0;
    if (getenv("CM_STARLOG")) fprintf(stderr, "[stars] player %d \"%s\": %d\n", g_star_player, g_star_text, player_stars((uint32_t)g_star_player));
}
