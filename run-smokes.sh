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

echo "[1] scenarios exit 0"
declare -A rc
rc[basic3]=$(run basic3 --smoke-frames=3)
rc[lineclear]=$(run lineclear --live-demo --scripted-input=@"$ss/live-line-clear.txt" --smoke-frames=90)
rc[pauseresume]=$(run pauseresume --live-demo --scripted-input=@"$ss/pause-resume.txt" --smoke-frames=100)
rc[hiscore]=$(run hiscore --reset-setup --topout-demo --scripted-input=@"$ss/high-score-exit.txt" --smoke-frames=1240 --debug-state)
rc[topout1400]=$(run topout1400 --reset-setup --topout-demo --smoke-frames=1400)
for n in basic3 lineclear pauseresume hiscore topout1400; do
  [ "${rc[$n]}" = "0" ] && pass "$n EXIT=0" || fail "$n EXIT=${rc[$n]}"
done

echo "[2] top-out: GAME OVER after 160 scanlines, kept until an Esc RELEASE"
# This used to assert topout=160, topout=760 and hsboot=72: the port's own timers, which
# removed GAME OVER after 600 frames and handed off by itself. The original has no such
# timer: GAME OVER (0x22FC) appears in the tick of the 160th dissolve scanline and stays
# until Esc is released (session loop 0x4A3); Return/Space/Backspace and the Esc press do
# nothing (October 2026 audit FX-01, FX-02, FX-25). Step N = debug line N+1.
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
grep -q "hsboot=72" "$tmp/hiscore.txt" && pass "hsboot=72 present" || fail "hsboot=72 missing"

echo "[3] deterministic RNG (cross-platform golden)"
chk_seed(){ # seed expected
  local got
  got=$("$exe" --live-demo --piece-seed="$1" --debug-state --smoke-frames=40 2>&1 | grep -m1 -o 'piece=[0-9]*' | sed 's/piece=//')
  [ "$got" = "$2" ] && pass "seed $1 -> piece $2" || fail "seed $1 -> piece $got (expected $2)"
}
chk_seed 42 6
chk_seed 7 2

echo "[4] SFX mixer (pan + half-volume + engine curve)"
"$exe" --live-demo --start-level=9 --piece-seed=7 --smoke-frames=900 > "$tmp/sfx.txt" 2>&1
grep -q "SFX mix: slot=0 vol%=45 engine=28/64 pan=107 class=0" "$tmp/sfx.txt" \
  && pass "piece-lock pan=107 half-vol(45)->engine 28" || fail "piece-lock pan/half-volume mismatch ($(grep -m1 -o 'SFX mix: slot=0.*' "$tmp/sfx.txt"))"
grep -q "SFX mix: slot=10 vol%=90 engine=57/64" "$tmp/sfx.txt" \
  && pass "line-clear engine curve 90->57" || fail "engine curve mismatch"

echo "[5] track 0 music loads"
# Read from the hiscore run: the --topout-demo run only ever started music through the
# port's invented 780-frame top-out handoff, which no longer exists (GAME OVER now waits
# for an Esc release, FX-01). The hiscore run starts track 0 on that release.
grep -q "Music: playing track 0" "$tmp/hiscore.txt" && pass "track 0 'Continuum' loads" || fail "track 0 music missing"

echo "[6] gameplay<->frontend screen fade"
# Real New Game (no shortcut, fades on): Enter on the default menu row after the
# boot splashes (~720+ frames now). The menu should fade out ~120 ticks, switch at
# black, fade in ~120 (screen fades are ~2.0s = 120 frames at 60Hz).
"$exe" --reset-setup --debug-state --scripted-input=820:enter,822:enter:up --smoke-frames=1120 > "$tmp/fade.txt" 2>&1
fo=$(grep -c 'fade=out:' "$tmp/fade.txt"); fi=$(grep -c 'fade=in:' "$tmp/fade.txt")
{ [ "$fo" -ge 112 ] && [ "$fo" -le 126 ] && [ "$fi" -ge 112 ] && [ "$fi" -le 126 ]; } \
  && pass "New Game fade out=$fo in=$fi (~120t each)" || fail "fade timing off (out=$fo in=$fi)"
# Shortcuts stay fade-free for determinism.
"$exe" --live-demo --debug-state --smoke-frames=5 2>&1 | grep -q 'fade=none' \
  && pass "shortcut (--live-demo) is fade-free" || fail "shortcut should not fade"

echo
if [ "$fails" -eq 0 ]; then echo "ALL SMOKES PASSED"; else echo "SMOKE FAILURES: $fails"; fi
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
