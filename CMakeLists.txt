cmake_minimum_required(VERSION 3.24)

project(acid_tetris_port LANGUAGES C CXX)  # C for the vendored miniz.c

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

option(ATET_PORT_FETCH_SDL3 "Fetch SDL3 automatically when it is not installed." ON)

find_package(SDL3 CONFIG QUIET)

if(NOT SDL3_FOUND)
  if(NOT ATET_PORT_FETCH_SDL3)
    message(FATAL_ERROR "SDL3 was not found. Re-run with ATET_PORT_FETCH_SDL3=ON or install SDL3.")
  endif()

  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XCURSOR OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XDBE OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XINPUT OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XFIXES OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XRANDR OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XSCRNSAVER OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XSHAPE OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XSYNC OFF CACHE BOOL "" FORCE)
  set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.4
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
  )
  FetchContent_MakeAvailable(SDL3)
endif()

set(ATET_SDL_TARGET SDL3::SDL3)
if(NOT TARGET ${ATET_SDL_TARGET})
  if(TARGET SDL3::SDL3-shared)
    set(ATET_SDL_TARGET SDL3::SDL3-shared)
  elseif(TARGET SDL3::SDL3-static)
    set(ATET_SDL_TARGET SDL3::SDL3-static)
  else()
    message(FATAL_ERROR "SDL3 was located, but no usable SDL3 target was exported.")
  endif()
endif()

# --- libmikmod: tracker music playback ---------------------------------------
# The original ACiD Tetris used MikMod (its "Music System" credit names MikMod's
# author) and embeds MikMod UN05 modules, so libmikmod reproduces the original
# music engine. Build a static core with WinMM output on Windows. The libmikmod
# docs target needs makeinfo, which is not required here; EXCLUDE_FROM_ALL keeps
# it out of the default build so only the linked mikmod-static target is built.
set(ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(ENABLE_STATIC ON  CACHE BOOL "" FORCE)
set(ENABLE_DSOUND OFF CACHE BOOL "" FORCE)
set(ENABLE_WASAPI OFF CACHE BOOL "" FORCE)
set(ENABLE_WINMM  ON  CACHE BOOL "" FORCE)
set(ENABLE_SDL    OFF CACHE BOOL "" FORCE)
set(ENABLE_OPENAL OFF CACHE BOOL "" FORCE)
set(ENABLE_WAV    OFF CACHE BOOL "" FORCE)
set(ENABLE_RAW    OFF CACHE BOOL "" FORCE)
set(ENABLE_AIFF   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
  libmikmod
  GIT_REPOSITORY https://github.com/sezero/mikmod.git
  GIT_TAG        libmikmod-3.3.13
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  libmikmod
  EXCLUDE_FROM_ALL
  SYSTEM)
FetchContent_MakeAvailable(libmikmod)

# WIN32 -> Windows GUI subsystem (no console window on double-click); ignored on
# other platforms. The SDL_main entry shim (SDL3::SDL3main + <SDL3/SDL_main.h>)
# supplies WinMain. run-smokes.sh still captures stdout/stderr via redirection.
add_executable(acid_tetris_port WIN32
  src/main.cpp
  src/atet_data.cpp
  src/audio_probe.cpp
  src/indexed_asset_loader.cpp
  src/milestone_a_demo.cpp
  src/music_player.cpp
  third_party/miniz/miniz.c
)

target_include_directories(acid_tetris_port PRIVATE include)
# libmikmod does not export its (generated) header directory on its target, so
# point at both the source include dir and the build tree where mikmod.h lands.
target_include_directories(acid_tetris_port SYSTEM PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/third_party/miniz"
  "${libmikmod_SOURCE_DIR}/libmikmod/include"
  "${libmikmod_BINARY_DIR}"
  "${libmikmod_BINARY_DIR}/include")
target_link_libraries(acid_tetris_port PRIVATE ${ATET_SDL_TARGET} mikmod-static)
# SDL_main entry shim (required for the WIN32/GUI subsystem; harmless elsewhere).
if(TARGET SDL3::SDL3main)
  target_link_libraries(acid_tetris_port PRIVATE SDL3::SDL3main)
endif()
if(WIN32)
  target_compile_definitions(acid_tetris_port PRIVATE ATET_PORT_HAVE_WINMM)
  target_sources(acid_tetris_port PRIVATE app_icon.rc)  # embeds the taskbar/Explorer icon
endif()

target_compile_definitions(acid_tetris_port PRIVATE
  MIKMOD_STATIC  # required to link libmikmod statically (esp. on MSVC)
  # The game reads NO copyrighted asset files: it decodes all graphics/audio/music
  # in-engine from the player's own ATET.DAT at runtime (see src/atet_data.cpp),
  # which it locates next to the executable as a loose ATET.DAT or inside an intact
  # AcidTetris.zip / ATETRIS.ZIP (see README). This is only the last-resort relative
  # fallback path for that lookup; no reference assets are bundled or required.
  ATET_PORT_ATET_DAT_PATH="ATET.DAT"
  ATET_PORT_SETUP_DAT_PATH="${CMAKE_CURRENT_BINARY_DIR}/SETUP.DAT"
)

# On Windows the port links SDL3 as a shared library, so SDL3.dll must sit next
# to acid_tetris_port.exe or the executable will not launch. Copy every linked
# runtime DLL into the executable's directory after each build. This is a no-op
# on Linux/macOS, where $<TARGET_RUNTIME_DLLS> expands to an empty list, so the
# native desktop build path is unaffected.
if(WIN32)
  add_custom_command(TARGET acid_tetris_port POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_RUNTIME_DLLS:acid_tetris_port>" "$<TARGET_FILE_DIR:acid_tetris_port>"
    COMMAND_EXPAND_LISTS
    VERBATIM)
endif()
