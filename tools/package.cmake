# package.cmake - assembles the release folder (run by "cmake --build <dir> --target release").
#   release/
#     cm9798(.exe)        the launcher players start; makes the game on first run
#     cm9798setup(.exe)   the installer
#     (Windows: SDL2.dll, SDL2_ttf.dll and what they need)
#     build/              the kit the launcher makes the game with:
#       lib/libcm9798rt.a   the port's own code (rt/), compiled
#       include/            rt.h, rt_internal.h
#       data/               functions.json, labels.csv, patches.txt, overrides.txt, relocations.txt
#       cflags.txt, link.txt  how to compile and link the game on this platform
#       zig/                the C compiler (if -DCM_ZIG was given)
# Nothing in it comes from the game: the game's code is made from each player's own CM2E16.EXE.
file(REMOVE_RECURSE ${OUT})
set(K ${OUT}/build)
file(MAKE_DIRECTORY ${K}/lib ${K}/include ${K}/data)
file(COPY ${LAUNCHER} ${SETUP} DESTINATION ${OUT})
if(PLATFORM STREQUAL "windows")
  # SDL2.dll and SDL2_ttf.dll put in the source folder (the SDL project's own Windows builds, which need
  # nothing but Windows): just those two. Otherwise everything the build copied next to the programs
  # (MSYS2's SDL2_ttf.dll needs FreeType, HarfBuzz and a dozen more DLLs).
  if(EXISTS ${SRC}/SDL2.dll)
    set(DLLS ${SRC}/SDL2.dll)
    if(TTF)
      if(NOT EXISTS ${SRC}/SDL2_ttf.dll)
        message(FATAL_ERROR "SDL2.dll is in ${SRC} but SDL2_ttf.dll isn't: put the SDL project's SDL2_ttf.dll there too")
      endif()
      file(STRINGS ${SRC}/SDL2_ttf.dll NEEDS REGEX "lib(freetype|harfbuzz)-[0-9]+\\.dll")
      if(NEEDS)
        message(FATAL_ERROR "${SRC}/SDL2_ttf.dll is MSYS2's: it needs libfreetype-6.dll, libharfbuzz-0.dll and more. "
          "Use the SDL project's own build instead (FreeType and HarfBuzz built in): from "
          "https://github.com/libsdl-org/SDL_ttf/releases, SDL2_ttf-devel-2.x.x-mingw.zip, "
          "x86_64-w64-mingw32/bin/SDL2_ttf.dll")
      endif()
      list(APPEND DLLS ${SRC}/SDL2_ttf.dll)
    endif()
    message(STATUS "release: SDL from ${SRC} (only SDL2.dll and SDL2_ttf.dll)")
  else()
    file(GLOB DLLS ${BINDIR}/*.dll)
  endif()
  if(DLLS)
    file(COPY ${DLLS} DESTINATION ${OUT})
  endif()
endif()
file(COPY ${RTLIB} DESTINATION ${K}/lib)
get_filename_component(RTNAME ${RTLIB} NAME)
if(NOT RTNAME STREQUAL "libcm9798rt.a")
  file(RENAME ${K}/lib/${RTNAME} ${K}/lib/libcm9798rt.a)
endif()
file(COPY ${SRC}/rt/rt.h ${SRC}/rt/rt_internal.h DESTINATION ${K}/include)
foreach(f functions.json labels.csv patches.txt overrides.txt relocations.txt)
  file(COPY ${SRC}/${f} DESTINATION ${K}/data)
endforeach()

# compiling the generated C: the same options CMakeLists.txt uses for gen/
set(CF "# options for compiling the game's generated C (one per line)\n-O2\n-w\n-std=gnu11\n-fno-strict-aliasing\n-fwrapv\n-fno-sanitize=undefined\n")
if(CALLTRACE)
  string(APPEND CF "-DCALLTRACE\n")
endif()
file(WRITE ${K}/cflags.txt "${CF}")

# linking: the port's library, SDL and the platform's options. {kit} is this folder; {find:a|b} the
# first of those libraries in the usual library folders (the player's own SDL2 on Linux and macOS)
set(LK "# extra options for linking the game (one per line). Zig: an -O level, or it links its own\n# helpers (compiler_rt) in debug mode, with safety checks\n-O2\n-fno-sanitize=undefined\n")
if(PLATFORM STREQUAL "windows")
  foreach(o ${ICON})
    file(COPY ${o} DESTINATION ${K}/lib)
    get_filename_component(n ${o} NAME)
    file(RENAME ${K}/lib/${n} ${K}/lib/cm9798_icon.o)
  endforeach()
  if(ICON)
    string(APPEND LK "{kit}/lib/cm9798_icon.o\n")
  endif()
  file(COPY ${SDLLIB} DESTINATION ${K}/lib)
  get_filename_component(n ${SDLLIB} NAME)
  string(APPEND LK "{kit}/lib/${n}\n")
  if(TTF AND TTFLIB)
    file(COPY ${TTFLIB} DESTINATION ${K}/lib)
    get_filename_component(n ${TTFLIB} NAME)
    string(APPEND LK "{kit}/lib/${n}\n")
  endif()
  string(APPEND LK "-luser32\n-Wl,--subsystem,windows\n-Wl,--stack,67108864\n")
elseif(PLATFORM STREQUAL "mac" AND MACFW)
  # the SDL project's frameworks, shipped in Frameworks/ next to cm9798 (setup copies them into the game
  # folder); the game, in build/, finds them in the folder above
  file(COPY ${SRC}/Frameworks/SDL2.framework ${SRC}/Frameworks/SDL2_ttf.framework DESTINATION ${OUT}/Frameworks)
  string(APPEND LK "-F{kit}/../Frameworks\n-framework\nSDL2\n-framework\nSDL2_ttf\n-Wl,-rpath,@executable_path/../Frameworks\n")
  string(APPEND LK "-lm\n-Wl,-stack_size,0x4000000\n")
  message(STATUS "release: SDL2.framework and SDL2_ttf.framework included (no Homebrew needed)")
elseif(PLATFORM STREQUAL "mac")
  string(APPEND LK "{find:libSDL2-2.0.0.dylib|libSDL2.dylib}\n")
  if(TTF)
    string(APPEND LK "{find:libSDL2_ttf-2.0.0.dylib|libSDL2_ttf.dylib}\n")
  endif()
  string(APPEND LK "-lm\n-Wl,-stack_size,0x4000000\n")
else()
  string(APPEND LK "{find:libSDL2-2.0.so.0|libSDL2.so}\n")
  if(TTF)
    string(APPEND LK "{find:libSDL2_ttf-2.0.so.0|libSDL2_ttf.so}\n")
  endif()
  string(APPEND LK "-lm\n")
endif()
file(WRITE ${K}/link.txt "${LK}")

if(ZIG)
  if(NOT EXISTS ${ZIG})
    # (in the MSYS2 terminal an unquoted C:\Zig\zig.zip loses its backslashes: C:Zigzig.zip)
    message(FATAL_ERROR "CM_ZIG=${ZIG} doesn't exist, so the release would have no compiler. Give the "
      "Zig download with forward slashes (-DCM_ZIG=/c/Zig/zig.zip) and configure again.")
  endif()
  if(NOT IS_DIRECTORY ${ZIG} AND ZIG MATCHES "\\.(zip|tar\\.xz|tar\\.gz)$")
    # the archive as downloaded from ziglang.org: one file in the release; the launcher unpacks it
    string(REGEX MATCH "\\.(zip|tar\\.xz|tar\\.gz)$" EXT ${ZIG})
    configure_file(${ZIG} ${K}/zig${EXT} COPYONLY)
    message(STATUS "release: bundled Zig as build/zig${EXT} (unpacked on the player's first run)")
  elseif(IS_DIRECTORY ${ZIG}/lib)
    file(COPY ${ZIG}/ DESTINATION ${K}/zig USE_SOURCE_PERMISSIONS)
    message(STATUS "release: bundled Zig from ${ZIG} (unpacked - give the downloaded .zip/.tar.xz instead for far fewer files)")
  else()
    message(FATAL_ERROR "CM_ZIG=${ZIG} is neither a Zig download (.zip, .tar.xz) nor an unpacked Zig folder")
  endif()
else()
  if(PLATFORM STREQUAL "windows")
    file(WRITE ${K}/zig/PUT-ZIG-HERE.txt "Unpack a Zig release (https://ziglang.org/download) here, so that build/zig/zig.exe is its program.\n")
    message(WARNING "release: no compiler bundled (configure with -DCM_ZIG=<unpacked Zig>): Windows has no compiler of its own, so players can't make the game")
  else()
    message(STATUS "release: no compiler bundled - players' own cc/gcc/clang will be used (Linux; macOS: Apple's command line tools)")
  endif()
endif()
message(STATUS "release folder: ${OUT}")
