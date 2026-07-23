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
rc[hiscore]=$(run hiscore --reset-setup --topout-demo --scripted-input=@"$ss/high-score-exit.txt" --smoke-frames=240 --debug-state)
rc[topout1400]=$(run topout1400 --reset-setup --topout-demo --smoke-frames=1400)
for n in basic3 lineclear pauseresume hiscore topout1400; do
  [ "${rc[$n]}" = "0" ] && pass "$n EXIT=0" || fail "$n EXIT=${rc[$n]}"
done

echo "[2] top-out fingerprint"
for pat in "topout=160" "topout=760" "hsboot=72"; do
  grep -q "$pat" "$tmp/hiscore.txt" && pass "$pat present" || fail "$pat missing"
done

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

echo "[5] startup music loads"
grep -q "Music: playing track 0" "$tmp/topout1400.txt" && pass "track 0 'Continuum' loads" || fail "startup music missing"

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
