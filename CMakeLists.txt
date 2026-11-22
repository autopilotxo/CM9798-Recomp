cmake_minimum_required(VERSION 3.16)
# macOS: run on macOS 11 (Big Sur, the first for Apple Silicon) and later, whatever macOS builds it (Apple's
# tools would otherwise require the building Mac's own version). Set before project(), as CMake needs.
if(CMAKE_HOST_APPLE AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "The oldest macOS the programs run on")
endif()
project(cm9798 C)

# Recompiled Championship Manager 97/98 (CM2E16.EXE) as a native 64-bit program.
#   gen/  - C generated from the original executable by cmgen / make_gen.py (not part of the source)
#   rt/   - runtime: memory, DOS/DPMI/BIOS, VESA video, input, EXE loader, SDL window
# Targets: cm9798 (the launcher players start: makes the game on first run), cm9798setup (the
# installer), cmgen, cm9798rt (rt/ as a library), and with gen/ present cm9798-game (the whole
# game, also built here to check it links). "--target release" makes the folder to give players (README.md).
#
# Options:
#   -DCM_HEADLESS=ON   build without SDL (screenshots + scripted input, for testing)
#   -DCM_CONSOLE=ON    Windows: show a console window instead of writing cm9798.log
#   -DCM_CALLTRACE=ON  keep a ring buffer of recent functions for crash reports (slower)

option(CM_HEADLESS  "Build the headless test backend instead of SDL" OFF)
option(CM_CALLTRACE "Record recent guest functions for crash diagnostics" OFF)
option(CM_CONSOLE   "Windows: keep a console window for output (debugging)" OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  message(FATAL_ERROR "Use GCC or Clang (e.g. MSYS2 MinGW-w64). MSVC is not supported yet.")
endif()

# Zig as the compiler (release builds, README.md): no C safety checks (Zig adds them unless told
# otherwise), and an optimisation level when linking too, for its own helper code
if(CMAKE_C_COMPILER MATCHES "zig")
  add_compile_options(-fno-sanitize=undefined)
  add_link_options(-O2 -fno-sanitize=undefined)
  # Without a -target (as on Windows) Zig builds for this PC's own processor (AVX-512 and the like), and
  # the programs then quit silently on players' older ones: build for any processor of the kind instead
  add_compile_options(-mcpu=baseline)
  add_link_options(-mcpu=baseline)
  # Linux: CMake can't ask Zig for the system's library folder name, so find_package wouldn't look in
  # /usr/lib/x86_64-linux-gnu (Debian, Ubuntu) for SDL2's files
  if(UNIX AND NOT APPLE AND NOT CMAKE_LIBRARY_ARCHITECTURE)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
      set(CMAKE_LIBRARY_ARCHITECTURE aarch64-linux-gnu)
    else()
      set(CMAKE_LIBRARY_ARCHITECTURE x86_64-linux-gnu)
    endif()
  endif()
  # ...and Zig (building for an older glibc) leaves out the system's own header folders: Debian and Ubuntu
  # keep part of SDL2's headers in /usr/include/<that name>/SDL2. Searched last, after Zig's own C headers,
  # so only what Zig hasn't got (SDL2/_real_SDL_config.h) comes from there.
  if(UNIX AND NOT APPLE AND IS_DIRECTORY "/usr/include/${CMAKE_LIBRARY_ARCHITECTURE}/SDL2")
    add_compile_options("SHELL:-idirafter /usr/include/${CMAKE_LIBRARY_ARCHITECTURE}")
  endif()
endif()

file(GLOB GEN_SOURCES ${CMAKE_SOURCE_DIR}/gen/*.c)
set(RT_SOURCES
  rt/rt.c rt/dos.c rt/video.c rt/input.c rt/le_load.c rt/main.c rt/overrides.c rt/textlayer.c rt/backup.c rt/autosave.c rt/database.c rt/stars.c rt/sound.c rt/intl.c rt/editor.c rt/editor_none.c)

if(CM_HEADLESS)
  list(APPEND RT_SOURCES rt/headless.c)
else()
  list(APPEND RT_SOURCES rt/sdl.c rt/sharptext.c)
  # macOS: the SDL project's SDL2.framework and SDL2_ttf.framework in Frameworks/ (from their .dmg
  # downloads) are used instead of Homebrew's, and go into the release, so players need no Homebrew.
  # The programs find them in a Frameworks folder next to themselves (the game: in the folder above).
  set(CM_MAC_FW "${CMAKE_SOURCE_DIR}/Frameworks")
  if(APPLE AND EXISTS "${CM_MAC_FW}/SDL2.framework" AND EXISTS "${CM_MAC_FW}/SDL2_ttf.framework")
    set(CM_MAC_FRAMEWORKS ON)
    message(STATUS "SDL: the frameworks in ${CM_MAC_FW}")
    foreach(fw SDL2 SDL2_ttf)
      add_library(${fw}::${fw} INTERFACE IMPORTED)
      set_target_properties(${fw}::${fw} PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CM_MAC_FW}/${fw}.framework/Headers"
        INTERFACE_COMPILE_OPTIONS "SHELL:-F \"${CM_MAC_FW}\""
        INTERFACE_LINK_OPTIONS "SHELL:-F \"${CM_MAC_FW}\";SHELL:-framework ${fw};SHELL:-Wl,-rpath,@executable_path/Frameworks;SHELL:-Wl,-rpath,@executable_path/../Frameworks")
    endforeach()
  else()
  find_package(SDL2 REQUIRED CONFIG)
  find_package(SDL2_ttf CONFIG QUIET)
  endif()
  if(NOT TARGET SDL2_ttf::SDL2_ttf)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
      pkg_check_modules(SDL2TTF IMPORTED_TARGET SDL2_ttf)
    endif()
  endif()
endif()

# cmgen: CM2E16.EXE -> gen/ (the same as make_gen.py, without Python or binutils)
#   cmgen <CM2E16.EXE> <folder with functions.json etc.> <gen folder>
#   cmgen <CM2ED.EXE> editor <gen folder>   (the Database Editor, rt/editor.c: optional)
set(CMGEN_SOURCES tools/cmgen/cmgen.c tools/cmgen/x86dis.c tools/cmgen/lift.c tools/cmgen/util.c)
add_executable(cmgen ${CMGEN_SOURCES})
set_property(TARGET cmgen PROPERTY C_STANDARD 11)
target_compile_definitions(cmgen PRIVATE CMGEN_MAIN)

# the port's own code (everything but the game's): a library the launcher links the game against. It only
# needs the names of the game's functions it uses (rt/game_funcs.h), not gen/, so a release builds without it.
add_library(cm9798rt STATIC ${RT_SOURCES})
target_include_directories(cm9798rt PUBLIC rt)
set_property(TARGET cm9798rt PROPERTY C_STANDARD 11)
target_compile_options(cm9798rt PRIVATE -fno-strict-aliasing -fwrapv)
if(CM_CALLTRACE)
  target_compile_definitions(cm9798rt PUBLIC CALLTRACE)
endif()
if(NOT CM_HEADLESS)
  # main() lives in rt/main.c; SDL_MAIN_HANDLED + SDL_SetMainReady() mean SDL2main isn't needed
  target_compile_definitions(cm9798rt PRIVATE SDL_MAIN_HANDLED)
  target_link_libraries(cm9798rt PUBLIC SDL2::SDL2)
  # sharp text (optional): the game's fonts redrawn at full resolution with SDL2_ttf
  if(TARGET SDL2_ttf::SDL2_ttf)
    target_link_libraries(cm9798rt PUBLIC SDL2_ttf::SDL2_ttf)
    target_compile_definitions(cm9798rt PRIVATE CM_TTF)
  elseif(SDL2TTF_FOUND)
    target_link_libraries(cm9798rt PUBLIC PkgConfig::SDL2TTF)
    target_compile_definitions(cm9798rt PRIVATE CM_TTF)
  else()
    message(WARNING "SDL2_ttf not found: building without sharp text (original fonts only)")
  endif()
endif()

if(NOT GEN_SOURCES)
  message(STATUS "gen/ isn't there: building the launcher, setup, cmgen and the port's library (all a release needs). "
                 "To build the whole game here as well, make gen/ from your CM2E16.EXE with cmgen and configure again.")
else()
# the whole game, built here too, to check it compiles and links (players get it from the launcher)
set(GAME_EXTRA)
if(WIN32)
  # program icon shown in Explorer, shortcuts and the taskbar
  enable_language(RC)
  set(GAME_EXTRA icon/cm9798.rc)
  set_source_files_properties(icon/cm9798.rc PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/icon)
  # rebuild the .exe's icon when the icon image changes, not only when the .rc file does
  set_source_files_properties(icon/cm9798.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/icon/cm9798.ico)
endif()
add_executable(cm9798-game ${GEN_SOURCES} ${GAME_EXTRA})
target_include_directories(cm9798-game PRIVATE rt gen)
set_property(TARGET cm9798-game PROPERTY C_STANDARD 11)
target_compile_options(cm9798-game PRIVATE -fno-strict-aliasing -fwrapv)
# generated code is huge and warning-noisy; keep warnings for the runtime only
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-O1")
target_link_libraries(cm9798-game PRIVATE cm9798rt)
if(NOT WIN32)
  target_link_libraries(cm9798-game PRIVATE m)
endif()
if(WIN32)
  # normal Windows program (no console window; output goes to cm9798.log in the game folder).
  # -DCM_CONSOLE=ON brings the console back for debugging.
  if(CM_CONSOLE)
    set(CM_SUBSYSTEM -Wl,--subsystem,console)
  else()
    set(CM_SUBSYSTEM -Wl,--subsystem,windows)
  endif()
  target_link_options(cm9798-game PRIVATE ${CM_SUBSYSTEM} -Wl,--stack,67108864)
endif()
if(MINGW)
  target_link_options(cm9798-game PRIVATE -static-libgcc)   # static runtime = fewer DLLs to copy
endif()

# macOS (Apple Silicon or Intel): SDL2 and SDL2_ttf from Homebrew ("brew install cmake sdl2 sdl2_ttf").
# The game runs on the main thread and recurses deeply; give it the same 64MB stack as on Windows.
if(APPLE)
  target_link_options(cm9798-game PRIVATE -Wl,-stack_size,0x4000000)
  if(CM_MAC_FRAMEWORKS)
    target_link_options(cm9798-game PRIVATE "SHELL:-Wl,-rpath,\"${CM_MAC_FW}\"")   # (built here, not shipped)
  endif()
endif()
endif()   # gen/

# cm9798: the program players start (tools/launcher/launcher.c). The first time it runs it makes the
# game (cm9798-game) from the player's own CM2E16.EXE with the compiler in its build folder, then starts it.
if(NOT CM_HEADLESS)
  set(LAUNCHER_SOURCES tools/launcher/launcher.c ${CMGEN_SOURCES})
  if(WIN32)
    enable_language(RC)
    list(APPEND LAUNCHER_SOURCES icon/cm9798.rc)
    set_source_files_properties(icon/cm9798.rc PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/icon)
  endif()
  add_executable(cm9798 ${LAUNCHER_SOURCES})
  set_property(TARGET cm9798 PROPERTY C_STANDARD 11)
  target_link_libraries(cm9798 PRIVATE SDL2::SDL2)
  if(WIN32)
    # a window program, not a console one (-Wl,--subsystem rather than -mwindows: Zig and Clang take it too)
    target_link_options(cm9798 PRIVATE -Wl,--subsystem,windows)
  endif()
  if(MINGW)
    target_link_options(cm9798 PRIVATE -static-libgcc)
  endif()
endif()

# Linux: "cmake --install build-linux" puts the program, its icons and a launcher entry
# under ~/.local when configured with -DCMAKE_INSTALL_PREFIX=$HOME/.local
if(TARGET cm9798-game AND UNIX)
  install(TARGETS cm9798-game RUNTIME DESTINATION bin)   # the launcher starts it (no build kit next to it)
endif()
if(APPLE AND TARGET cm9798)
  install(TARGETS cm9798 RUNTIME DESTINATION bin)
elseif(UNIX AND TARGET cm9798)
  install(TARGETS cm9798 RUNTIME DESTINATION bin)
  foreach(sz 16 24 32 48 64 128 256 512)
    install(FILES icon/icon-${sz}.png DESTINATION share/icons/hicolor/${sz}x${sz}/apps RENAME cm9798.png)
  endforeach()
  install(FILES icon/cm9798.desktop DESTINATION share/applications)
endif()

# cm9798setup: the installer. Sets up a folder for the port from the player's own CM 97/98 (game
# files, databases/9798 with its pictures, saved games), copies the port in, and can import
# Championship Manager 2 and 96/97 (with its European Leagues editions). A window (SDL);
# "cm9798setup --cli <cm9798> <target> [<cm2> [<cm96/97> [<edition>...]]]" does the same without one.
if(NOT CM_HEADLESS)
  set(SETUP_SOURCES tools/setup/setup.c tools/setup/install.c tools/setup/cm2import.c tools/setup/dlls.c)
  if(WIN32)
    list(APPEND SETUP_SOURCES icon/cm9798setup.rc)
    set_source_files_properties(icon/cm9798setup.rc PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/icon
                                OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/icon/cm9798setup.ico)
  endif()
  add_executable(cm9798setup ${SETUP_SOURCES})
  set_property(TARGET cm9798setup PROPERTY C_STANDARD 11)
  target_link_libraries(cm9798setup PRIVATE SDL2::SDL2)
  if(WIN32)
    target_link_libraries(cm9798setup PRIVATE shell32 ole32)
    target_link_options(cm9798setup PRIVATE -Wl,--subsystem,windows)
  endif()
  if(MINGW)
    target_link_options(cm9798setup PRIVATE -static-libgcc)
  endif()
  if(UNIX)
    install(TARGETS cm9798setup RUNTIME DESTINATION bin)
    if(NOT APPLE)
      foreach(sz 16 24 32 48 64 128 256 512)
        install(FILES icon/setupicon-${sz}.png DESTINATION share/icons/hicolor/${sz}x${sz}/apps RENAME cm9798setup.png)
      endforeach()
      install(FILES icon/cm9798setup.desktop DESTINATION share/applications)
    endif()
  endif()
endif()

# Windows: after building, copy the DLLs the programs need (SDL2.dll, SDL2_ttf.dll and anything
# they need in turn - found on the PATH, e.g. in MSYS2) into the build folder, so the folder runs
# on any PC and can be zipped up as it is.
if(WIN32)
  add_executable(copydlls tools/setup/copydlls.c tools/setup/dlls.c)
  set_property(TARGET copydlls PROPERTY C_STANDARD 11)
  if(MINGW)
    target_link_options(copydlls PRIVATE -static-libgcc)
  endif()
  foreach(t cm9798 cm9798-game)
    if(TARGET ${t})
      add_dependencies(${t} copydlls)
      add_custom_command(TARGET ${t} POST_BUILD COMMAND copydlls $<TARGET_FILE:${t}> VERBATIM)
    endif()
  endforeach()
  if(TARGET cm9798setup)
    add_dependencies(cm9798setup copydlls)
    add_custom_command(TARGET cm9798setup POST_BUILD COMMAND copydlls $<TARGET_FILE:cm9798setup> VERBATIM)
  endif()
endif()

# release: the folder to zip up for players - the launcher, the installer and the build kit (the port's
# own code as a library, plus the compiler). No game code: that is made on each player's machine.
#   cmake --build <dir> --target release      (configure with -DCM_ZIG=<an unpacked Zig release> to
#                                              bundle the compiler; see README.md)
if(TARGET cm9798rt AND TARGET cm9798 AND TARGET cm9798setup)
  set(CM_ZIG "" CACHE PATH "An unpacked Zig release (ziglang.org/download) to bundle as the release's build/zig")
  set(PKG_TTF OFF)
  if(TARGET SDL2_ttf::SDL2_ttf OR SDL2TTF_FOUND)
    set(PKG_TTF ON)
  endif()
  set(PKG_ARGS -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/release -DZIG=${CM_ZIG}
      -DLAUNCHER=$<TARGET_FILE:cm9798> -DSETUP=$<TARGET_FILE:cm9798setup> -DRTLIB=$<TARGET_FILE:cm9798rt>
      -DCALLTRACE=${CM_CALLTRACE} -DTTF=${PKG_TTF} -DBINDIR=$<TARGET_FILE_DIR:cm9798>)
  if(WIN32)
    add_library(cm9798icon OBJECT icon/cm9798.rc)
    set_source_files_properties(icon/cm9798.rc PROPERTIES INCLUDE_DIRECTORIES ${CMAKE_SOURCE_DIR}/icon)
    find_library(CM_SDL2_IMPLIB NAMES libSDL2.dll.a SDL2.lib)
    find_library(CM_SDL2TTF_IMPLIB NAMES libSDL2_ttf.dll.a SDL2_ttf.lib)
    list(APPEND PKG_ARGS -DPLATFORM=windows "-DICON=$<TARGET_OBJECTS:cm9798icon>"
         -DSDLLIB=${CM_SDL2_IMPLIB} -DTTFLIB=${CM_SDL2TTF_IMPLIB})
    set(PKG_DEPS cm9798icon)
  elseif(APPLE)
    list(APPEND PKG_ARGS -DPLATFORM=mac -DMACFW=${CM_MAC_FRAMEWORKS})
  else()
    list(APPEND PKG_ARGS -DPLATFORM=linux)
  endif()
  if(TARGET cm9798-game)
    list(APPEND PKG_DEPS cm9798-game)    # (with gen/: check the whole game links; on Windows, copy its DLLs)
  endif()
  add_custom_target(release COMMAND ${CMAKE_COMMAND} ${PKG_ARGS} -P ${CMAKE_SOURCE_DIR}/tools/package.cmake
                    DEPENDS cm9798 cm9798setup cm9798rt ${PKG_DEPS} VERBATIM)
else()
  # say why there's nothing to package, rather than "unknown target"
  set(PKG_WHY "")
  if(CM_HEADLESS)
    string(APPEND PKG_WHY " CM_HEADLESS is on.")
  endif()
  add_custom_target(release COMMAND ${CMAKE_COMMAND} -E echo "release: can't make a release:${PKG_WHY}"
                    COMMAND ${CMAKE_COMMAND} -E false VERBATIM)
endif()
