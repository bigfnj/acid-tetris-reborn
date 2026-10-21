#!/usr/bin/env bash
# Portable smoke/regression harness for the ACiD Tetris source port.
# Runs the documented headless scenarios and asserts the certified fingerprints
# (top-out timing, cross-platform-deterministic RNG, SFX pan/volume). Works on
# Linux and on Windows Git Bash. Exits non-zero on any failure.
#
# Usage: run-smokes.sh [path-to-exe]
#   Defaults to the Release build next to this script (Windows or Linux layout).
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exe="${1:-}"
if [ -z "$exe" ]; then
  for cand in \
    "$here/build/Release/saba-reborn.exe" \
    "$here/build-linux/saba-reborn" \
    "$here/build/saba-reborn" \
    "$here/build/Release/acid_tetris_port.exe" \
    "$here/build-linux/acid_tetris_port" \
    "$here/build/acid_tetris_port"; do
    [ -x "$cand" ] && exe="$cand" && break
  done
fi
[ -x "$exe" ] || { echo "FATAL: exe not found (pass path as arg)"; exit 2; }
ss="$here/smoke-scripts"

export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
# Co-locate the Linux SDL3 .so if present.
sdldir="$here/build-linux/_deps/sdl3-build"
[ -d "$sdldir" ] && export LD_LIBRARY_PATH="$sdldir:${LD_LIBRARY_PATH:-}"

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
fails=0
pass(){ echo "  PASS: $1"; }
fail(){ echo "  FAIL: $1"; fails=$((fails+1)); }

run(){ # name -> args...; captures to $tmp/name.txt, returns exit code
  local n="$1"; shift
  "$exe" "$@" > "$tmp/$n.txt" 2>&1
  echo $?
}

echo "== exe: $exe =="

# Every run reads and writes a suite-private SETUP.DAT, never the player's file beside
# the game data: several scenarios reset it (--reset-setup) and one commits a
# 'Player 17500 36' score. $exe becomes a wrapper that adds --setup-dat, and the file
# is seeded with the defaults so every scenario starts from the same settings.
setup="$tmp/SETUP.DAT"
command -v cygpath >/dev/null 2>&1 && setup="$(cygpath -m "$setup")"
printf '#!/usr/bin/env bash\nexec "%s" --setup-dat="%s" "$@"\n' "$exe" "$setup" > "$tmp/port"
chmod +x "$tmp/port"
exe="$tmp/port"
"$exe" --reset-setup --smoke-frames=1 > "$tmp/seed.txt" 2>&1

echo "[1] scenarios exit 0"
declare -A rc
rc[basic3]=$(run basic3 --smoke-frames=3)
# The C key that injects rows is a developer key, active only with --debug.
rc[lineclear]=$(run lineclear --debug --debug-state --live-demo --scripted-input=@"$ss/live-line-clear.txt" --smoke-frames=90)
rc[pauseresume]=$(run pauseresume --live-demo --scripted-input=@"$ss/pause-resume.txt" --smoke-frames=220 --debug-state)
rc[hiscore]=$(run hiscore --reset-setup --topout-demo --scripted-input=@"$ss/high-score-exit.txt" --smoke-frames=1300 --debug-state)
rc[topout1400]=$(run topout1400 --reset-setup --topout-demo --smoke-frames=1400)
for n in basic3 lineclear pauseresume hiscore topout1400; do
  [ "${rc[$n]}" = "0" ] && pass "$n EXIT=0" || fail "$n EXIT=${rc[$n]}"
done
# The scenario must really inject: the C press on frame 48 (after the 32-step opening collapse,
# see smoke-scripts/README.md) starts a new collapse. Without --debug the key does nothing and
# frame 48 shows collapse=0.
tr -d '\r' < "$tmp/lineclear.txt" | grep 'Debug state:' | sed -n '49p' | grep -q 'collapse=1 ' \
  && pass "lineclear: C injected rows on frame 48 (collapse started)" \
  || fail "lineclear: C did not inject rows on frame 48 ($(tr -d '\r' < "$tmp/lineclear.txt" | grep 'Debug state:' | sed -n '49p' | grep -o 'collapse=[0-9]*'))"
# Pause on the Esc RELEASE, Return to Game through the gate and the 48-step
# conceal (the script used to press Esc without a release, which the original
# never acts on, and Esc on the main menu used to quit the port).
{ grep -q 'scene=Main Menu' "$tmp/pauseresume.txt" && tail -n 3 "$tmp/pauseresume.txt" | grep -q 'scene=Gameplay'; } \
  && pass "pause-resume: menu opened and the game resumed" || fail "pause-resume did not pause and resume"

echo "[2] top-out: GAME OVER after 160 scanlines, kept until an Esc RELEASE, then the entry screen"
# This used to assert topout=160, topout=760 and hsboot=72: the port's own timers, which
# removed GAME OVER after 600 frames and handed off by itself, and an invented 72-frame
# state-9 hold. The original has no such timer: GAME OVER (0x22FC) appears in the tick of
# the 160th dissolve scanline and stays until Esc is released (session loop 0x4A3);
# Return/Space/Backspace and the Esc press do nothing. The release enters state 9 through
# the dispatcher (palette fades off in --topout-demo) and the 48-step table reveal.
# Step N = debug line N+1.
grep "Debug state:" "$tmp/hiscore.txt" > "$tmp/hiscore.dbg"
st(){ sed -n "$(( $1 + 1 ))p" "$tmp/hiscore.dbg"; }
{ st 158 | grep -q "gameover=0" && st 159 | grep -q "topout=160 .*gameover=1"; } \
  && pass "GAME OVER first shown at step 159 (topout=160), Return/Space/Backspace ignored" \
  || fail "GAME OVER timing/skip ($(st 158 | grep -o 'topout=[0-9]*') / $(st 159 | grep -o 'gameover=[01]'))"
st 999 | grep -q "scene=Top Out .*gameover=1" && pass "GAME OVER still up at step 999 (no timed handoff)" \
  || fail "GAME OVER left without input ($(st 999 | grep -o 'scene=[A-Za-z ]* phase'))"
st 1005 | grep -q "scene=Top Out" && pass "Esc held (1000..1005) changes nothing" || fail "Esc acted on its press"
{ st 1005 | grep -q "scene=Top Out" && st 1006 | grep -q "scene=High Score"; } \
  && pass "Esc release at 1006 leaves GAME OVER for the high-score path" \
  || fail "no GAME OVER -> high-score transition on the Esc release ($(st 1006 | grep -o 'scene=[A-Za-z ]* phase'))"
for pat in "scene=High Score Entry" "flow=reveal"; do
  grep -q "$pat" "$tmp/hiscore.txt" && pass "$pat present" || fail "$pat missing"
done

echo "[3] deterministic RNG (cross-platform golden)"
# The numeric id alone cannot see which SHAPE the id is bound to (audit PC-16: the old I,O,T,S,Z,J,L
# table dealt the same ids as the original but drew other shapes), so the spawn cells are checked
# too: original ids 0..6 = S Z J L T I O (table 0x184E3), spawn x=4 -> id 6 (O) on columns 6-7,
# id 2 (J, ###/..#) on columns 5-7.
chk_seed(){ # seed expected_id expected_cells(sorted)
  local line got cells
  line=$("$exe" --live-demo --piece-seed="$1" --debug-state --smoke-frames=40 2>&1 | grep -m1 'Debug state:')
  got=$(echo "$line" | grep -o ' piece=[0-9]*' | sed 's/ piece=//')
  cells=$(echo "$line" | grep -oE ' cells=[^ ]+' | sed 's/ cells=//' | tr '/' '\n' | sort | paste -sd/ -)
  [ "$got" = "$2" ] && [ "$cells" = "$3" ] && pass "seed $1 -> piece $2 cells $3" \
    || fail "seed $1 -> piece $got cells ${cells:-none} (expected $2 cells $3)"
}
chk_seed 42 6 "6:0/6:1/7:0/7:1"
chk_seed 7 2 "5:0/6:0/7:0/7:1"

echo "[4] SFX mixer (pan + half-volume + engine curve)"
"$exe" --live-demo --start-level=9 --piece-seed=7 --smoke-frames=900 > "$tmp/sfx.txt" 2>&1
# The first lock is the spawn J (seed 7 deals id 2) dropped in place on columns 5-7. The original
# pans the lock sound from the piece's reference x, the origin of its authored 4x4 box: the spawn
# x = 4 of 0x1348, so (4 - 5) * 21 + 0x80 = 107. The leftmost-column formula the port shipped
# (audit finding SX-04) would print (5 - 5) * 21 + 0x80 = 128 for this piece.
first_lock="$(grep -m1 -o 'SFX mix: slot=0 vol%=[0-9]* engine=[0-9]*/64 pan=[0-9]* class=[0-9]*' "$tmp/sfx.txt")"
[ "$first_lock" = "SFX mix: slot=0 vol%=45 engine=28/64 pan=107 class=0" ] \
  && pass "first piece-lock pan=107 (reference x 4) half-vol(45)->engine 28" || fail "piece-lock pan/half-volume mismatch (${first_lock:-none})"
grep -q "SFX mix: slot=10 vol%=90 engine=57/64" "$tmp/sfx.txt" \
  && pass "line-clear engine curve 90->57" || fail "engine curve mismatch"

echo "[5] track 0 music loads"
# Read from the hiscore run: the --topout-demo run only ever started music through the
# port's invented 780-frame top-out handoff, which no longer exists (GAME OVER now waits
# for an Esc release, FX-01). The hiscore run starts track 0 on that release.
grep -q "Music: playing track 0" "$tmp/hiscore.txt" && pass "track 0 'Continuum' loads" || fail "track 0 music missing"

echo "[6] gameplay<->frontend transitions (dispatcher 0x3830)"
# The old check expected ~120-frame overlay fades fitted to a capture. The original
# runs four 63-tick palette fades: at boot 0x64f0 (invisible, over black) and the
# title fade-in 0x63b8; on New Game the title fade-out 0x62e0 and the gameplay
# fade-in 0x6498. The menu is interactive from tick 957 (step 956); an Enter
# released at step 960 starts New Game through the gate and the 48-step conceal.
"$exe" --reset-setup --debug-state --scripted-input=958:enter,960:enter:up --smoke-frames=1250 > "$tmp/fade.txt" 2>&1
po=$(grep -c 'fade=pal-out:' "$tmp/fade.txt"); ti=$(grep -c 'fade=title-in:' "$tmp/fade.txt")
to=$(grep -c 'fade=title-out:' "$tmp/fade.txt"); pi=$(grep -c 'fade=pal-in:' "$tmp/fade.txt")
co=$(grep -c 'flow=conceal' "$tmp/fade.txt")
{ [ "$po" -eq 63 ] && [ "$ti" -eq 63 ] && [ "$to" -eq 63 ] && [ "$pi" -eq 63 ] && [ "$co" -eq 48 ]; } \
  && pass "boot pal-out=$po title-in=$ti, New Game conceal=$co title-out=$to pal-in=$pi (63-tick fades)" \
  || fail "transition timing off (pal-out=$po title-in=$ti conceal=$co title-out=$to pal-in=$pi)"
grep -q 'scene=Gameplay' "$tmp/fade.txt" && pass "New Game reaches gameplay" || fail "New Game never reached gameplay"
# Shortcuts stay fade-free for determinism.
"$exe" --live-demo --debug-state --smoke-frames=5 2>&1 | grep -q 'fade=none' \
  && pass "shortcut (--live-demo) is fade-free" || fail "shortcut should not fade"

echo
if [ "$fails" -eq 0 ]; then echo "ALL SMOKES PASSED"; else echo "SMOKE FAILURES: $fails"; fi
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
