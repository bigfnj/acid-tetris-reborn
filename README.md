# ACiD Tetris Reborn

A faithful, modern source port of the 1997 MS-DOS game **ACiD Tetris** (Jason
Pimble & Scott Emerle / Dungeon Dwellers Design), written in C++20 on SDL3 with
libmikmod for the original tracker music. It runs natively on Windows and Linux.

The port was rebuilt from a reverse-engineering study of the original executable
rather than a line-for-line decompile. The behavioural notes, specs, and RE
findings behind it live in the companion repo, **acid-tetris-grimoire**.

## Bring your own game data

This repository contains **no** game assets — no `ATET.DAT`, no extracted
graphics/audio/music, and no original program code. ACiD Tetris is freeware, but
its own notice only permits distributing the intact original archive, so this
port takes the ScummVM approach: you provide your own copy of the game, and the
port decodes everything it needs from it **at runtime, in memory** (see
`src/atet_data.cpp`). Nothing from the original data file is written out or
modified.

**All you need is the game's data file, `ATET.DAT`.** Drop it next to the
`acid_tetris_port` executable and run — that single file is enough; the port
reads every graphic, sound, and music track out of it. If you have the game as an
archive instead of a loose file, you can just place the whole zip next to the
executable and the port will pull `ATET.DAT` out of it for you.

At startup the port looks, in order, for:

1. a loose `ATET.DAT` next to the executable, then
2. an ACiD Tetris archive next to the executable — `AcidTetris.zip`, `ATETRIS.ZIP`
   (it extracts `ATET.DAT` from inside), then
3. the current directory, then a build-tree fallback.

ACiD Tetris was freeware. A preserved, verified-genuine copy is on the Internet
Archive: <https://archive.org/details/swizzle_demu_Acid>. Download it, open the
archive, and copy `ATET.DAT` out next to the executable (or just place the whole
zip beside the executable and let the port pull `ATET.DAT` from it). It also
turns up on other DOS/abandonware archives.

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. SDL3 and libmikmod are fetched
automatically at configure time if not already installed.

**Windows** (Visual Studio 2022+):

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build copies `SDL3.dll` next to the executable. The result is
`build/Release/acid_tetris_port.exe` — a windowed (no-console) game with an app
icon.

**Linux** (GCC/Clang + Ninja):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# run with the fetched SDL3 on the library path:
LD_LIBRARY_PATH=$PWD/build/_deps/sdl3-build ./build/acid_tetris_port
```

## Controls

Arrow keys move/rotate; the menus are keyboard-driven (arrows to move between
rows, Enter to select). Holding Left/Right auto-repeats; holding Down soft-drops.
Keys are rebindable in Options → Keyboard Setup. Sound options persist to a
`SETUP.DAT` next to the game.

## Fidelity

The port aims to match the original's behaviour, not just its look: the gameplay
RNG and piece sequence are bit-identical to the decompiled generator; the tracker
music, SFX mixer, gravity/soft-drop cadence, line-clear and top-out particle
systems, menu animation, and screen fades are modelled on the recovered original
routines; and the whole simulation runs at the original's 70.086 Hz (the VGA
mode-13h vertical-retrace rate the game locked to) via a fixed-rate loop, so it
plays at the correct speed regardless of your monitor.

Two lightweight test harnesses document and guard that behaviour: `run-smokes.sh`
(logic/RNG/audio-routing) and `run-dynamic-tests.sh` (animation rates and input
response). Both need a copy of the game data to run.

## Credits

Original ACiD Tetris © 1997 Jason Pimble (code) and Scott Emerle (graphics), with
music by Liam Hesse, Bobby Tamburrino, Jon Dal Kristbjornsson, and Chris & Scott
Emerle, on Jean-Paul Mikkers' MikMod engine. This port is an independent,
non-commercial fan work. See [`NOTICE`](NOTICE) for the full credits, the
original freeware terms, and trademark information, and [`LICENSE`](LICENSE) for
the port's MIT license (which covers the port's code only, not the game).
