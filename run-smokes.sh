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
rc[pauseresume]=$(run pauseresume --live-demo --scripted-input=@"$ss/pause-resume.txt" --smoke-frames=220 --debug-state)
rc[hiscore]=$(run hiscore --reset-setup --topout-demo --scripted-input=@"$ss/high-score-exit.txt" --smoke-frames=300 --debug-state)
rc[topout1400]=$(run topout1400 --reset-setup --topout-demo --smoke-frames=1400)
for n in basic3 lineclear pauseresume hiscore topout1400; do
  [ "${rc[$n]}" = "0" ] && pass "$n EXIT=0" || fail "$n EXIT=${rc[$n]}"
done
# Pause on the Esc RELEASE, Return to Game through the gate and the 48-step
# conceal (the script used to press Esc without a release, which the original
# never acts on, and Esc on the main menu used to quit the port).
{ grep -q 'scene=Main Menu' "$tmp/pauseresume.txt" && tail -n 3 "$tmp/pauseresume.txt" | grep -q 'scene=Gameplay'; } \
  && pass "pause-resume: menu opened and the game resumed" || fail "pause-resume did not pause and resume"

echo "[2] top-out fingerprint"
# hsboot=72 was the port's invented 72-frame state-9 hold; the original runs the
# dispatcher entry (palette fades, off in --topout-demo) and the 48-step table
# reveal, so the check is now the name-entry screen after a reveal.
for pat in "topout=160" "topout=760" "scene=High Score Entry" "flow=reveal"; do
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
