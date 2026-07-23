# ACiD Tetris Reborn

A faithful, modern source port of the 1997 MS-DOS game **ACiD Tetris** (Jason
Pimble & Scott Emerle / Dungeon Dwellers Design), written in C++20 on SDL3 with
libmikmod for the original tracker music. It runs natively on Windows and Linux.

The port was rebuilt from a reverse-engineering study of the original executable
rather than a line-for-line decompile. The behavioural notes, specs, and RE
findings behind it live in the companion repo, **acid-tetris-grimoire**.

## Game data

**Using a pre-built release?** It already bundles SABA (see below) — just unzip and
run, nothing else to fetch. The rest of this section is for building from source or
bringing your own copy of the game.

This source repository contains **no** game assets — no `ATET.DAT` / `saba.dat`, no
extracted graphics/audio/music, and no original program code. ACiD Tetris is freeware, but
its own notice only permits distributing the intact original archive, so this
port takes the ScummVM approach: you provide your own copy of the game, and the
port decodes everything it needs from it **at runtime, in memory** (see
`src/atet_data.cpp`). Nothing from the original data file is written out or
modified.

**All you need is the game's data file.** Drop it next to the `saba-reborn`
executable and run — that single file is enough; the port reads every graphic,
sound, and music track out of it. If you have the game as a zip instead of a loose
file, just place the whole zip beside the executable and the port will pull the
data out of it for you.

The port accepts **either release of the game**, because they are the same game:

- **ACiD Tetris** (1997) — data file `ATET.DAT`; archives `AcidTetris.zip` /
  `ATETRIS.ZIP`.
- **SABA — "Super ACiD Block Attack"** (2002) — data file `saba.dat`; archive
  `saba.zip`. This is the authors' own trademark-safe re-release; its data is
  **byte-identical to ACiD Tetris except the title-screen logo**, so it plays
  exactly the same.

At startup the port looks next to the executable (then in the working directory)
for any of those loose data files, then any of those archives, extracting the data
file from within.

Where to get it (both are freeware):

- **SABA** — from the original authors, Dungeon Dwellers Design:
  <http://www.dddgames.com/saba/>. This is the cleanest source (author-sanctioned,
  no trademark baggage). Drop `saba.dat` (or `saba.zip`) beside the executable.
- **ACiD Tetris** — a preserved, verified-genuine copy is on the Internet Archive:
  <https://archive.org/details/swizzle_demu_Acid>. Copy `ATET.DAT` out of it (or
  place the archive beside the executable).

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. SDL3 and libmikmod are fetched
automatically at configure time if not already installed.

**Windows** (Visual Studio 2022+):

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build copies `SDL3.dll` next to the executable. The result is
`build/Release/saba-reborn.exe` — a windowed (no-console) game with an app
icon.

**Linux** (GCC/Clang + Ninja):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# run with the fetched SDL3 on the library path:
LD_LIBRARY_PATH=$PWD/build/_deps/sdl3-build ./build/saba-reborn
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
routines; and the whole simulation is paced by a fixed-rate loop clocked to match
the running original's real-time speed, so it plays at the correct speed regardless
of your monitor.

Two lightweight test harnesses document and guard that behaviour: `run-smokes.sh`
(logic/RNG/audio-routing) and `run-dynamic-tests.sh` (animation rates and input
response). Both need a copy of the game data to run.

## Credits

Original ACiD Tetris © 1997 Jason Pimble (code) and Scott Emerle (graphics), with
music by Liam Hesse, Bobby Tamburrino, Jon Dal Kristbjornsson, and Chris & Scott
Emerle, on Jean-Paul Mikkers' MikMod engine. This port is an independent,
non-commercial fan work by BigFNj (<https://github.com/bigfnj/acid-tetris-reborn>).
See [`NOTICE`](NOTICE) for the full credits, the original freeware terms, and
trademark information, and [`LICENSE`](LICENSE) for the port's license — 0BSD, i.e.
do whatever you like with the port's own code and release binaries, no attribution
required (this covers the port only, not the game).
