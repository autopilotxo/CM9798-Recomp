# CM 97/98 data and save formats (CM2E16)

Recovered from the I/O module at 0x36000–0x3CA00 and checked against the files in this install.

## Verification
| File | Check | Result |
|---|---|---|
| PLDATA1 / TMDATA1 / CPDATA1 / MGDATA1 / NMDATA1 | Parse the whole file | 0 bytes left over in every file |
| AWDATA1 / FXDATA1 / TRDATA1 | Sum of block sizes = file size | 29,008 / 527,136 / 292,254: exact |
| MCHFAX1 / CLHIST1 / PLHIST1 / MGHIST1 | Size divided by record size | 7,000 / 2,300 / 10,000 / 400: whole numbers |
| Player struct | Decoded against PLAYERS.DB1 and real-world facts | Positions r=0.85–0.97, Caps/Goals r=1.00, Owen DOB 14/12/79, Shearer 13/8/70 |

## Two kinds of database
1. **`.DB1` master database** (`players.db1` 19,505 records, `tmdata.db1` 2,085, `mgdata.db1` 944). Self-describing schema, **big-endian** integers, NUL-padded strings. Only read when a new game starts. `db1_read_header` 0x3CB08, `db1_get_int` 0x3CD20, `db1_get_str` 0x3CDD8. The field index passed to `db1_get_int` is the schema index minus one.
2. **`.S16` save slot** (`<name><slot>.S16`). A raw dump of the in-memory tables, little-endian. The extension comes from the build (S16, or S8 for CM2E8).

## .S16 list files
Layout: `count`, then for each record its strings (`u16 len` + bytes) followed by the raw struct, then any trailing blocks.

| File | Count | Strings | Struct | Trailing |
|---|---|---|---|---|
| PLDATA (players) | u16 @0xA3AF32 | 2 | 0x168 | none |
| TMDATA (clubs) | u16 @0xA3AF6A | 7 | 0x5BA | 0x11F8, 0x19 |
| CPDATA (competitions) | u16 @0xA3AF2C | 5 | 0x7D4 | 0x20670 |
| MGDATA (managers/staff) | u16 @0xA3AF30 | 2 | 0x27 | 1, 0x11FB0 |
| NMDATA (name table) | u32 @0xA06068 | 1 | 0x0D | 0x2BF20 |

Each struct starts with one stale 32-bit `char*` per string. The loader mallocs the strings again and overwrites those pointers.

## Player struct (0x168): see `cm9798_formats.h`
- +0x00/+0x04: name pointers
- +0x0A: caps
- +0x0C: goals
- +0x14–0x16: date of birth (d/m/yy)
- +0x17–0x19: ten 2-bit position ratings
- +0x1A/0x1C/0x1E: ability/potential/reputation (i16)
- +0xEA–0x101: 24 attributes (1–20)

Still unknown: the joined and contract date encoding (+0x4C, +0x4F) and +0x102–0x167, which includes u16 arrays (form, history?).

The DB "Marking" column is never read at new game. +0xED is a generated attribute in the 1–20 range.

## Implications for the x64 port
- Write explicit (de)serialisers per record type. Pointers are on disk, so `fread(struct)` will not work on x64.
- To stay compatible with existing saves, keep the exact on-disk byte layouts (the `static_assert`s in the header) and convert to native structs on load.
- The history files are opened `rb+` and accessed by seeking to individual records. Keep them on disk rather than loading them whole, or load them whole if memory isn't a concern (about 51 MB in total).

## Tools
- `tools/db1.py`: schema dump of any .DB1
- `tools/s16.py <folder>`: parses and checks every save file in slot 1
- `tools/export_players.py <folder> [slot]`: players to CSV (sample: `players_slot1.csv`)
- `tools/iotrace.py`: re-derives the read/write table straight from the EXE
