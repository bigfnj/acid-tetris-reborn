# ACiD Tetris Reborn

A modern source port of the 1997 MS-DOS game **ACiD Tetris** (Jason Pimble & Scott
Emerle / Dungeon Dwellers Design), written in C++20 on SDL3, with libmikmod playing
the game's own tracker music. It builds and runs natively on Windows and Linux.
Music and sound effects share one software mixer, modelled on the original's, which
plays through SDL3 on every platform; the Linux build has not been run since that
change.

**Status: technical preview.** The port plays complete games with the original's
graphics and audio, all decoded from the game's own data file. Matching the
original's behaviour exactly is still in progress: a 2026 audit against the original
executable found differences in timing, input handling, gameplay rules, visuals and
audio, and they are being fixed. The game logic runs on the original's 60.0011 Hz
timer rather than the display refresh, and a slow frame catches up by running up to
six logic steps, as the original's frame pacer does.

The port was rebuilt from a reverse-engineering study of the original executable
rather than a line-for-line decompile. The behavioural notes, specs, and RE
findings behind it live in the companion repo, **acid-tetris-grimoire**.

## Game data

This source repository contains **no** game data: no `ATET.DAT` / `saba.dat` and no
extracted audio or music. It contains no original program code either. Two small
exceptions: the program icon (`app_icon.ico`) is a converted copy of one of the
game's mood-face graphics, and a few short texts and tables (menu strings, the
credits pages and the default high-score table) are transcribed from the original
program.

ACiD Tetris is freeware, but its own notice only permits distributing the intact
original archive, so this port takes the ScummVM approach: you provide your own copy
of the game, and the port decodes everything it needs from it **at runtime, in
memory** (see `src/atet_data.cpp`). Nothing from the original data file is written
out or modified.

**All you need is the game's data file.** Drop it next to the `saba-reborn`
executable and run. That single file is enough: the port reads all of its graphics and
audio out of it. If you have the game as a zip instead of a loose file,
place the whole zip beside the executable and the port pulls the data out of it.

The port accepts **either release of the game**:

- ACiD Tetris (1997): data file `ATET.DAT`; archives `AcidTetris.zip` /
  `ATETRIS.ZIP`.
- SABA, "Super ACiD Block Attack" (2002): data file `saba.dat`; archive
  `saba.zip`. This is the authors' own trademark-safe re-release. Its data is
  **byte-identical to ACiD Tetris except the title-screen logo**, so the port plays
  the same game from either. (The port shows ACiD Tetris's credits pages with both;
  SABA's own executable has different credits.)

At startup the port looks in the executable's folder first, then in the working
directory. In each folder it tries the loose data files (`ATET.DAT`, `saba.dat`,
`SABA.DAT`) before the archives (`AcidTetris.zip`, `ATETRIS.ZIP`, `acidtetris.zip`,
`saba.zip`, `SABA.ZIP`), and extracts the data file from an archive in memory. The
names are matched exactly as written here, which matters on Linux.

Where to get it (both are freeware):

- SABA, from the original authors, Dungeon Dwellers Design:
  <http://www.dddgames.com/saba/>. This is the cleanest source (author-sanctioned,
  no trademark baggage). Drop `saba.dat` (or `saba.zip`) beside the executable.
- ACiD Tetris: a preserved, verified-genuine copy is on the Internet Archive:
  <https://archive.org/details/swizzle_demu_Acid>. Copy `ATET.DAT` out of it (or
  place the archive beside the executable).

No release download has been published yet. The release workflow builds downloads
without game data unless the maintainer turns on bundling SABA's intact archive (see
`NOTICE`).

## Settings and high scores

Options, key bindings and high scores live in `SETUP.DAT`, in the original game's
178-byte format, in the folder of the data file the port loaded (normally next to
the executable). The original keeps `setup.dat` beside `atet.dat` the same way, so a
`SETUP.DAT` from the DOS game can be dropped in. When that folder is not writable
(for example under Program Files), the port keeps a per-user copy instead and logs
the path it chose: `%APPDATA%\saba-reborn\SETUP.DAT` on Windows,
`~/.local/share/saba-reborn/SETUP.DAT` on Linux (under `$XDG_DATA_HOME` when that is
set). `--setup-dat=PATH` uses a file of your choice.

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. CMake uses an installed SDL 3.4 or newer
when it finds one and otherwise downloads SDL 3.4.4; libmikmod 3.3.13 is always
downloaded at configure time and linked statically.

**Windows** (Visual Studio 2022+):

```
cmake -S . -B build -A x64
cmake --build build --config Release
```

The build copies `SDL3.dll` next to the executable and links the Visual C++ runtime
statically, so neither file needs the Visual C++ redistributable. The result is
`build/Release/saba-reborn.exe`, a windowed (no-console) game with an app icon.

**Linux** (GCC/Clang + Ninja):

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
# run with the fetched SDL3 on the library path:
LD_LIBRARY_PATH=$PWD/build/_deps/sdl3-build ./build/saba-reborn
```

## Controls

| Action (Keyboard Setup name) | Default key |
| --- | --- |
| Soft drop (Down) | Down arrow |
| Move left (Left) | Left arrow |
| Move right (Right) | Right arrow |
| Rotate clockwise (Rotate Left) | A |
| Rotate counter-clockwise (Rotate Right) | S |

Holding Left or Right auto-repeats, and holding Down soft-drops. Up does nothing
during play. Esc opens the menu during a game. In the menus the arrow keys move
between rows and Enter selects. All five game keys can be rebound in Options →
Keyboard Setup.

### Developer options

`--debug` turns on developer shortcuts the original game does not have: scene
jumps (Tab, T, G), preset boards (1-3), injected line clears (C, B), a forced
top-out (O) or high-score entry (H), and a test sound on Space. They are off by
default, so they cannot interrupt a game or take a key you have bound. The test
scripts also use `--smoke-frames`, `--scripted-input`, `--live-demo`,
`--topout-demo`, `--piece-seed`, `--start-level`, `--reset-setup` and `--setup-dat`.

The original game mixed its sound one Sound Blaster buffer ahead of what you heard
(mixing rate / 12 sample frames, 83 ms at 44,100 Hz). The port adds no such delay
by default, because no delay sounded closest to the original in a side-by-side
listening test. `--audio-delay=original` adds the original's buffer at the mixing
rate in use. `--audio-delay-samples=N`, or the environment variable
`ATET_AUDIO_DELAY_SAMPLES=N` when neither flag is given, delays all sound by N
sample frames at the Sound Setup mixing rate, so the same N lasts twice as long at
22,050 Hz as at 44,100 Hz. Values above 2 seconds are capped, with a warning. An
invalid value, or a second delay flag, is an error and the game does not start.
The active delay is logged at startup.

## Tests

`run-smokes.sh`, `run-dynamic-tests.sh`, `run-engineering-tests.sh` and `run-audio-tests.sh` check that the
game boots and keeps a handful of its behaviours stable, and that settings and
developer keys behave as described above. They mostly compare the port with its own
reference numbers, not with the original, so passing them does not prove fidelity.
All four need a copy of the game data, and none of them touches your own
`SETUP.DAT`. The release workflow runs the first three before it packages a build.

## Credits

Original ACiD Tetris © 1997 Jason Pimble (code) and Scott Emerle (graphics), with
music by Liam Hesse, Bobby Tamburrino, Jon Dal Kristbjornsson, and Chris & Scott
Emerle, on Jean-Paul Mikkers' MikMod engine. This port is an independent,
non-commercial fan work by BigFNj (<https://github.com/bigfnj/acid-tetris-reborn>).
See [`NOTICE`](NOTICE) for the full credits, the original freeware terms, and
trademark information, and [`LICENSE`](LICENSE) for the port's license: 0BSD for the
port's own source code (do whatever you like with it, no attribution required). It
covers the port only, not the game. Release binaries also contain third-party code
under its own licences: libmikmod (GNU LGPL, version 2 or later, linked statically),
miniz (MIT) and SDL3 (zlib). Release downloads include those licence texts in
`LICENSES/`.
