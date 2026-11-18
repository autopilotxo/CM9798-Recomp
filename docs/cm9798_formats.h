/*
 * cm9798_formats.h - on-disk layouts for Championship Manager 97/98 (CM2E16.EXE)
 *
 * Recovered from the save/load routines in CM2E16.EXE (addresses below) and
 * checked against real files: every .S16 list file parses with 0 bytes left,
 * and every fixed/random-access file size matches exactly.
 *
 * IMPORTANT FOR THE x64 PORT
 *   These are the ON-DISK layouts. The original game fread()s them straight
 *   into memory, and the leading fields are 32-bit char* pointers that are
 *   meaningless on disk (they are re-malloc'd from the length-prefixed
 *   strings written in front of each record). Never fread() into these
 *   structs on x64: read with an explicit deserialiser into native structs.
 *
 * Endianness: .S16 = little-endian.  .DB1 = BIG-endian integers.
 * Confidence: [code] = taken from a store in the code; [stat] = matched
 *   statistically against PLAYERS.DB1; [?] = unknown or a guess.
 */
#ifndef CM9798_FORMATS_H
#define CM9798_FORMATS_H
#include <stdint.h>
#include <assert.h>

#pragma pack(push, 1)

/* ---------- common ---------- */
/* A string in a save file: uint16 length, then that many bytes, no NUL.
   Written by cm_write_str (0x1E074), read by cm_read_str (0x1E108).      */
typedef uint32_t cm_stale_ptr;   /* 32-bit pointer value saved by the DOS build */

/* ---------- .DB1 master database (players.db1, tmdata.db1, mgdata.db1) ----------
 *  uint16le field_count
 *  field_count x { uint8 name_len; char name[name_len];
 *                  uint8 is_string; uint8 size; uint8 indexed; uint8 reserved; }
 *  uint8 unknown[6]
 *  records: sum(size) bytes each, fields in schema order.
 *           strings are NUL-padded; integers are BIG-endian, width = size.
 *  The schema is self-describing: see tools/db1.py.
 */

/* ---------- PLDATAn.S16 : players  (load 0x3A3F5, save 0x3B024) ----------
 *  uint16 count; count x { str first_name; str second_name; cm_player_disk }
 *  In memory: array at 0x1F3DE0, stride 0x168; count at 0xA3AF32.
 */
typedef struct {
    cm_stale_ptr first_name;      /* 0x000 [code]                                   */
    cm_stale_ptr second_name;     /* 0x004 [code]                                   */
    uint16_t     unk_008;         /* 0x008 [?] set during the name/nation lookup    */
    uint8_t      caps;            /* 0x00A [code+stat r=1.00]                       */
    uint8_t      unk_00b;         /* 0x00B                                          */
    uint8_t      goals;           /* 0x00C [code+stat r=1.00]                       */
    uint8_t      unk_00d[7];      /* 0x00D  0x0F is initialised from goals          */
    uint8_t      dob_day;         /* 0x014 [code, verified: Owen 14/12/79]          */
    uint8_t      dob_month;       /* 0x015                                          */
    uint8_t      dob_year;        /* 0x016 two digits; from DB age as 97-age if no DOB */
    uint8_t      pos_a;           /* 0x017 2-bit ratings: GK b0-1, Sweeper b2-3, Defence b4-5, Anchor b6-7   [code+stat] */
    uint8_t      pos_b;           /* 0x018 Midfield b0-1, Support b2-3, Attack b4-5, Right b6-7                 */
    uint8_t      pos_c;           /* 0x019 Left b0-1, Central b2-3, b4-7 unknown                                */
    int16_t      ability;         /* 0x01A [code+stat] current ability (0-200)      */
    int16_t      potential;       /* 0x01C [code]                                   */
    int16_t      reputation;      /* 0x01E [code+stat]                              */
    uint8_t      unk_020[0x2C];   /* 0x020 0x4C/0x4F hold joined / contract dates (format not decoded yet) */
    uint8_t      unk_04c[0x9E];   /* 0x04C                                          */
    /* 0x0EA: attributes 1-20 [code+stat] */
    uint8_t      aggression, big_occasion, character;
    uint8_t      attr_ed;         /* 0x0ED generated at new game (DB "Marking" is never read) */
    uint8_t      consistency, creativity, determination, dirtiness,
                 dribbling, flair, heading, influence, injury_prone,
                 adaptability, off_the_ball, pace, passing, positioning,
                 set_pieces, shooting, stamina, strength, tackling,
                 technique;       /* ends 0x101 */
    uint8_t      unk_102[0x66];   /* 0x102 contains u16 arrays from about 0x138 (history/form?) */
} cm_player_disk;
static_assert(sizeof(cm_player_disk) == 0x168, "player size");

/* ---------- TMDATAn.S16 : clubs  (load 0x391B0, save 0x39954) ----------
 *  uint16 count; count x { 7 x str; cm_club_disk }; uint8 tail1[0x11F8]; uint8 tail2[0x19]
 *  count at 0xA3AF6A. tail1 is read to 0x1E8EC0, tail2 to 0xA433A8.
 */
typedef struct { cm_stale_ptr names[7]; uint8_t body[0x5BA - 28]; } cm_club_disk;
static_assert(sizeof(cm_club_disk) == 0x5BA, "club size");

/* ---------- CPDATAn.S16 : competitions  (load 0x37A4B, save 0x3818C) ----------
 *  uint16 count; count x { 5 x str; cm_comp_disk }; uint8 tail[0x20670] (-> 0x152698)
 *  count at 0xA3AF2C. Strings, e.g.: "WCF","WCF","W/Cup","World Cup","bra_94.pcx"
 */
typedef struct { cm_stale_ptr names[5]; uint8_t body[0x7D4 - 20]; } cm_comp_disk;
static_assert(sizeof(cm_comp_disk) == 0x7D4, "competition size");

/* ---------- MGDATAn.S16 : managers/staff  (load 0x3B552, save 0x3BB34) ----------
 *  uint16 count; count x { str first; str second; cm_staff_disk };
 *  uint8 b (-> 0xA4495A); uint8 tail[0x11FB0] (-> 0x18BBC8). count at 0xA3AF30.
 */
typedef struct { cm_stale_ptr first_name, second_name; uint8_t body[0x27 - 8]; } cm_staff_disk;
static_assert(sizeof(cm_staff_disk) == 0x27, "staff size");

/* ---------- NMDATAn.S16 : name table  (load 0x36804, save 0x36958) ----------
 *  uint32 count (0xA06068); count x { str name; cm_name_disk (-> 0x8F06D8) };
 *  uint8 tail[0x2BF20] (-> 0x8C18D8)
 */
typedef struct { cm_stale_ptr name; uint8_t body[0x0D - 4]; } cm_name_disk;
static_assert(sizeof(cm_name_disk) == 0x0D, "name size");

/* ---------- fixed-layout files: concatenated raw blocks ---------- */
/* AWDATAn : 0x61CB (-> 0xA3BCD4), 0x685 (-> 0xA3B62C), 0x900 (-> 0x10E070)            = 29008 bytes */
/* FXDATAn : 0x340 (-> 0x1F3AA0), u16 (0xA3AF36), u16 (0xA3AF2E), 0x7B69C (-> 0x97F6A4),
             0x5140 (-> 0x562C60)                                                      = 527136 bytes */
/* TRDATAn : 0x14820 (-> 0xF9850), u16 (0xA3AF26), 0x32D7A (-> 0xA08168), u16 (0xA3AF24) = 292254 bytes */
/* GNDATAn : 20 small globals, 471 bytes (see load routine 0x36A20 for the order)                     */
/* GAMESS16.IDX : 0x230 bytes (-> 0x8BC770), save-slot index                                         */

/* ---------- random-access history files (opened rb+, one record per seek) ---------- */
#define CM_MCHFAX_REC 0x887   /* MCHFAXn : 7000 records,  buffer 0xA42B1C */
#define CM_CLHIST_REC 0x13DD  /* CLHISTn : 2300 records,  buffer 0xA433C8 */
#define CM_PLHIST_REC 0x2D0   /* PLHISTn : 10000 records, buffer 0x19DB78 */
#define CM_MGHIST_REC 0xA9EC  /* MGHISTn : 400 records,   buffer 0x9FAD40 */

#pragma pack(pop)
#endif
