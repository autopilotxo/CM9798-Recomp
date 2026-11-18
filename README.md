# CM 97/98 native port (static recompilation)

A native Windows, Linux and macOS build of the DOS football management game
*Championship Manager 97/98* (CM2E16.EXE, version 2.93), made by statically
recompiling the original executable into C and running it on a small SDL2
runtime.

This is an unofficial fan project. It is not affiliated with or endorsed by
Sports Interactive, Eidos, Square Enix or any current owner of the
Championship Manager name. All trademarks belong to their owners.

## You need your own copy of the game

This repository and its releases contain **no game code and no game data**. The game is
made on your own machine from your own copy of `CM2E16.EXE` v2.93 (size 1,407,137 bytes,
CRC32 0x359E908E; any other version is refused), and the setup tool copies the database,
pictures and sounds from your own installation.

## Playing (release downloads)

1. Download the release for your system and unzip it.
2. Run `cm9798setup` and point it at your CM 97/98 folder. It can also import pictures and
   sounds from Championship Manager 2 and CM 96/97.
3. Start `cm9798` in the folder it set up. **The first time**, it makes the game from your
   `CM2E16.EXE` with the compiler that comes with it; this takes a minute or two and only
   happens once (and again after you install a newer release).

macOS also needs SDL2 from Homebrew (`brew install sdl2 sdl2_ttf`); Linux needs your
distribution's SDL2 and SDL2_ttf packages.

## Building from source
The build makes a **release**: what players download. It holds the launcher (`cm9798`), the
setup tool (`cm9798setup`), the port's own code compiled as a library, and a C compiler
([Zig](https://ziglang.org), MIT licence) as a single archive. It has **no game code** in it:
the first time a player starts `cm9798`, it makes the game on their machine from their own
`CM2E16.EXE`.

To build one you need your own copy of Championship Manager 97/98 **v2.93** (`CM2E16.EXE`,
1,407,137 bytes), because the port's code is compiled against the list of the game's functions
made from it (`gen/`, step 2). None of it goes into the release.

---

### 1. Install the tools (one-off)

#### Windows

1. Install **MSYS2** from <https://www.msys2.org> and open the **MSYS2 UCRT64** terminal.
2. Install CMake, Ninja and SDL2:

   ```
   pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-SDL2_ttf
   ```

3. Download Zig for Windows (x86_64) from <https://ziglang.org/download>. Keep the `.zip` (it goes
   into the release) and also unpack it, e.g. to `C:\Zig`, to build with.
4. Get the SDL project's own `SDL2.dll` and `SDL2_ttf.dll` and put them in the source folder (after
   step 2). They need nothing but Windows, so the release ships just these two DLLs:
   - `SDL2.dll`: `SDL2-devel-2.x.x-mingw.zip` → `x86_64-w64-mingw32/bin/`, from
     <https://github.com/libsdl-org/SDL/releases> (latest **2.x**)
   - `SDL2_ttf.dll`: `SDL2_ttf-devel-2.x.x-mingw.zip` (or the x64 one in `-VC.zip`), from
     <https://github.com/libsdl-org/SDL_ttf/releases> (latest **2.x**)

   The build checks `SDL2_ttf.dll` and stops if it's MSYS2's (which needs twenty-odd more DLLs).

In the UCRT64 terminal, Windows paths are written with forward slashes: `C:\Zig` is `/c/Zig`.

#### Linux

- **Arch / CachyOS / SteamOS:** `sudo pacman -S --needed base-devel cmake sdl2 sdl2_ttf`
- **Ubuntu / Debian / Mint:** `sudo apt install build-essential cmake pkg-config libsdl2-dev libsdl2-ttf-dev`
- **Fedora:** `sudo dnf install gcc cmake SDL2-devel SDL2_ttf-devel`

Then download Zig for Linux (x86_64) from <https://ziglang.org/download>: keep the `.tar.xz` and
also unpack it. (Or leave Zig out: see "Without Zig" below.)

#### macOS

```
xcode-select --install
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
brew install cmake sdl2 sdl2_ttf
```

(Run the two "Next steps" lines Homebrew prints after installing, before `brew install`.)
Then download Zig for your kind of Mac (aarch64 for Apple Silicon, x86_64 for Intel) from
<https://ziglang.org/download> and keep the `.tar.xz`. (Or leave Zig out: see "Without Zig".)

---

### 2. Get the source and make `gen/`

```
git clone https://github.com/autopilotxo/CM9798-Recomp.git
cd CM9798-Recomp
```

`gen/` is made from your `CM2E16.EXE` by `cmgen`, which is built first. Use the configure
command for your system from step 3, then (replacing `/path/to/CM2E16.EXE` with yours):

```
cmake --build build --target cmgen
./build/cmgen /path/to/CM2E16.EXE . gen
```

(`./build/cmgen.exe` on Windows.) It checks the EXE is v2.93 and takes about a second. Run it
again after changing `patches.txt`, `relocations.txt` or `overrides.txt`. `gen/` is never
committed (`.gitignore`).

---

### 3. Build the release

#### Windows (UCRT64)

Here Zig is unpacked in `C:\Zig` and the download is `C:\Zig\zig.zip`:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=/c/Zig/zig.exe;cc" -DCM_ZIG=/c/Zig/zig.zip
cmake --build build --target release
```

#### Linux

```
cmake -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_C_COMPILER=/path/to/zig/zig;cc;-target;x86_64-linux-gnu.2.28" -DCM_ZIG=/path/to/zig-x86_64-linux-VERSION.tar.xz
cmake --build build --target release -j$(nproc)
```

The glibc 2.28 target makes the release run on older distributions as well as new ones.

#### macOS

One release per kind of Mac (Apple Silicon or Intel), each built on that kind:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCM_ZIG=/path/to/zig-aarch64-macos-VERSION.tar.xz
cmake --build build --target release -j$(sysctl -n hw.ncpu)
```

Players need SDL2 from Homebrew (`brew install sdl2 sdl2_ttf`).

#### Without Zig (Linux and macOS only)

Leave out `-DCM_ZIG` (and the `CMAKE_C_COMPILER` option) for a much smaller release. The launcher
then uses the player's own compiler: `cc`/`gcc`/`clang` on Linux, Apple's command line tools on
a Mac (`xcode-select --install`), and says how to install one if there's none. Windows has no
compiler of its own, so a Windows release always includes Zig.

If you made `gen/` after configuring, run the `cmake -B build ...` line again so CMake sees it.

---

### 4. The release

The release is `build/release`: zip it up as it is.

```
cm9798(.exe)             the launcher players start
cm9798setup(.exe)        the setup tool
SDL2.dll, SDL2_ttf.dll   (Windows)
build/                   the kit the game is made with:
  lib/libcm9798rt.a        the port's code, compiled
  include/, data/          rt.h and rt_internal.h; functions.json, labels.csv, patches.txt, ...
  cflags.txt, link.txt     how the game is compiled and linked on this platform
  zig.zip / zig.tar.xz     the compiler, exactly as downloaded
```

Players run `cm9798setup` (pointing it at their CM 97/98 folder), then `cm9798`. On the first
start it unpacks Zig, makes the game from their `CM2E16.EXE` (a minute or two), and deletes the
kit, leaving only `build/cm9798-game`, `build/game.stamp` and `build/build.log`. A newer
release, set up with `cm9798setup`, brings a new kit and the game is made again. Settings
(`cm9798.cfg`), screenshots and saves stay in the game folder.

#### Trying it

Copy `build/release` somewhere else, close the terminal (on Windows its PATH can hide a missing
DLL), run `cm9798setup` into a new folder, and start `cm9798` there: you get exactly what
players get. To try changes again, build the release again and set up a new folder (or the
same one: the changed kit makes the game again).

---

### Troubleshooting

- **The game couldn't be made:** `build/build.log` in the game folder has the compiler's output.
- **Make the game again:** set it up again with `cm9798setup`, or start the first run with
  `CM_KEEP_BUILD=1` (keeps the kit) and then use `cm9798 --rebuild`.
- **Use another compiler for the game:** `CM_CC=gcc cm9798 --rebuild` (with the kit kept).
- **"release: can't make a release: gen/ is missing":** do step 2, then run the configure line again.
- **Windows paths in the UCRT64 terminal:** forward slashes (`/c/Zig/zig.zip`), or quote them.

## Features

- Native 64-bit build, windowed or full screen, scaling, sharp TrueType text
- Up to 9 playable leagues at once
- Optional CM 2 backgrounds and match sounds (from your own CM 2 install)
- Relative ("for your club") star ratings, autosave and save backups
- Separate database folders, each with its own `database.cfg`

## What is in this repository

| Path | Contents |
|---|---|
| `tools/cmgen/`, `make_gen.py`, `gen.py`, `lift.py`, `relocate.py`, `disasm.py`, `tools/gen/` | The recompiler: reads your EXE and writes `gen/` (in C, or in Python) |
| `tools/launcher/` | `cm9798`: makes the game from your EXE on first run, then starts it |
| `functions.json`, `labels.csv` | Function addresses and names (no code) |
| `patches.txt`, `relocations.txt`, `overrides.txt` | Hooks and memory changes applied while generating |
| `rt/` | The runtime: DOS/video/input emulation, SDL front end, new features |
| `tools/setup/` | The installer / importer |
| `tools/package.cmake` | Assembles a release (no game code in it) |
| `icon/` | Program icons |
| `docs/` | Notes on the game's file formats |
| `LICENSES/` | Third-party licences (DejaVu fonts) |

Not included, and never to be committed: `gen/`, any `.EXE`, database files,
pictures, sounds, saves, and Microsoft fonts.

## Licence

The code in this repository is released under the [MIT License](LICENSE). Generated
`gen/` output and anything copied from the game remain the property of their
rights holders.
