#!/usr/bin/env bash
# Dynamic-behavior parity tests (complements run-smokes.sh + the static capture
# certification, which can't see animation rates or input response). Each check
# drives the port headless with --debug-state and asserts a TEMPORAL/INTERACTIVE
# behavior against its RE-derived expectation -- the class of bug that slipped
# past the still-frame side-by-sides (menu pulse 64x too slow; held-Down not
# soft-dropping). Usage: run-dynamic-tests.sh [path-to-exe]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
exe="${1:-}"
if [ -z "$exe" ]; then
  for cand in \
    "$here/build/Release/saba-reborn.exe" "$here/build/saba-reborn" \
    "$here/build/Release/acid_tetris_port.exe" "$here/build/acid_tetris_port"; do
    [ -x "$cand" ] && exe="$cand" && break
  done
fi
[ -x "$exe" ] || { echo "FATAL: exe not found (pass path as arg)"; exit 2; }

export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy
echo "== dynamic tests: $exe =="
fail=0
pass() { echo "  PASS: $1"; }
bad()  { echo "  FAIL: $1"; fail=1; }

# From a pos=x,y series: total descents (y increments) and consecutive-descents
# (a descent whose previous frame ALSO descended). The original soft-drop is one row
# every TWO frames (RE 0x8000/frame vs the 0x10000 threshold), so descents are never
# back-to-back -> consecutive==0. A 1-row-per-frame bug descends every frame ->
# consecutive is large. This separates "soft-drop present" from "soft-drop too fast".
descent_metric() { awk -F, '{y=$2; up=(NR>1 && y>py); if(up&&pup)c++; if(up)d++; pup=up; py=y} END{print d+0, c+0}'; }

# --- Test 1: selected-row pulse, decoded exactly from RE 0x3fd8: reveal = sine/0x800 +
# 0x30, phase +0x20/frame => a 64-frame (~0.91s) breath spanning 0x20..0x40 (32..64),
# symmetric around the settled 0x30=48. Peak 0x40=64 renders a 16px row that exactly
# fills 0x60cc's 16-row window -> no clip, and 16 < the 17px pitch -> no neighbour
# overlap. (Earlier passes: 32f/0x10..0x50 = 2x fast+tall = clip/overlap; then a
# grow-up-only 0x30..0x48 that still overshot the pitch on middle rows.) Assert exact
# amplitude min==32 / max==64 and the ~64f period via rising edges through the midpoint.
# The pulse only runs once the menu is up (after the ~240f splash), so sample the tail.
echo "[1] selected-row pulse rate + amplitude"
# The two boot splash pages now hold ~360 frames each (~6s/page at 60Hz), so the menu
# appears after ~720+ frames; run long and sample the menu tail.
series="$("$exe" --no-fade --debug-state --smoke-frames=1200 2>&1 | grep -oE 'pulse=[0-9]+' | grep -oE '[0-9]+$' | tail -360)"
minv="$(echo "$series" | sort -n | head -1)"
maxv="$(echo "$series" | sort -n | tail -1)"
edges="$(echo "$series" | awk '{v=$1} NR>1 && pv<48 && v>=48 {e++} {pv=v} END{print e+0}')"
# 360 menu frames / 64f ~= 5.6 cycles -> 5-6 rising edges through the midpoint (48).
if [ "$minv" = "32" ] && [ "$maxv" = "64" ] && [ "$edges" -ge 4 ] && [ "$edges" -le 7 ]; then
  pass "pulse breathes 32..64 over ~64f ($edges cycles in 360f; peak 64=16px fits the 16px window, no clip/overlap)"
else
  bad "pulse off (min=$minv want 32, max=$maxv want 64, edges=$edges want 4-7 for ~64f)"
fi

# --- Test 2: holding Down soft-drops at ONE ROW PER TWO FRAMES (not per frame).
# RE gameplay-input-timing-pass: Down held overrides the per-frame gravity increment to
# 0x8000 == half the 0x10000 descent threshold, so the piece steps down every other
# frame. The prior port moved a whole row EVERY frame on top of gravity (~2x too fast --
# the "way too fast" report). Assert soft-drop is present (descents>>gravity) AND that
# descents are never back-to-back (consecutive<=2), which is the fingerprint of the
# correct half-threshold cadence and fails on the 1-row-per-frame regression.
echo "[2] hold-Down soft-drop cadence"
read -r held hcons < <("$exe" --live-demo --start-level=0 --no-fade --debug-state --smoke-frames=220 --scripted-input='120:down' 2>&1 \
        | grep -oE 'pos=[0-9]+,[0-9]+' | sed -n '121,205p' | descent_metric)
read -r ctl  ccons < <("$exe" --live-demo --start-level=0 --no-fade --debug-state --smoke-frames=220 2>&1 \
        | grep -oE 'pos=[0-9]+,[0-9]+' | sed -n '121,205p' | descent_metric)
if [ "$held" -ge 15 ] && [ "$held" -gt $((ctl * 4)) ] && [ "$hcons" -le 2 ]; then
  pass "Down held soft-drops 1 row / 2 frames (descents held=$held vs gravity=$ctl, consecutive=$hcons)"
else
  bad "soft-drop cadence off (held=$held gravity=$ctl consecutive=$hcons; want held>=15, >>gravity, consecutive<=2)"
fi

# --- Test 3: holding Left DAS-repeats to the wall (not a single step).
echo "[3] hold-Left DAS auto-repeat"
minx="$("$exe" --live-demo --start-level=0 --no-fade --debug-state --smoke-frames=220 --scripted-input='120:left' 2>&1 \
        | grep -oE 'pos=[0-9]+,[0-9]+' | sed -n '121,200p' | awk -F'[=,]' '{print $2}' | sort -n | head -1)"
if [ -n "$minx" ] && [ "$minx" -le 1 ]; then
  pass "Left held repeats to the wall (min col=$minx)"
else
  bad "Left held did not auto-repeat (min col=${minx:-none}; expected <=1)"
fi

# --- Test 4: clear faces live 160 steps; a clear within 160 ticks of the previous one gets
# the fast-follow face (original 0x0F54..0x0F9F, life 0xA0 -> face up for 159 steps).
# --live-demo clears four rows at tick 0; a single (C) at step 60 must show face 1, not 2.
echo "[4] clear-face lifetime + fast-follow face"
alerts="$("$exe" --live-demo --debug-state --smoke-frames=400 --scripted-input='60:c,62:c:up' 2>&1 \
          | grep -o 'Debug state: .*' | grep -oE ' alert=-?[0-9]+' | sed 's/ alert=//')"
a60="$(echo "$alerts" | sed -n '61p')"
run1="$(echo "$alerts" | sed -n '61,$p' | awk '$1==1{n++; next} {exit} END{print n+0}')"
if [ "$a60" = "1" ] && [ "$run1" = "159" ]; then
  pass "single 60 ticks after a clear shows face 1 for 159 steps"
else
  bad "fast-follow/lifetime off (face at step 60 = '$a60' want 1, steps with face 1 = $run1 want 159)"
fi

# --- Test 5: sleepy face exactly 1800 ticks after the last clear, up for 319 steps
# (original 0x10A6: tick - last clear == 0x708, life 0x140). --live-demo clears at tick 0.
echo "[5] sleepy face timing"
alerts="$("$exe" --live-demo --debug-state --smoke-frames=2200 2>&1 \
          | grep -o 'Debug state: .*' | grep -oE ' alert=-?[0-9]+' | sed 's/ alert=//')"
first11="$(echo "$alerts" | awk '$1==11{print NR-1; exit}')"
n11="$(echo "$alerts" | awk '$1==11{n++} END{print n+0}')"
if [ "$first11" = "1799" ] && [ "$n11" = "319" ]; then
  pass "sleepy face from step 1799 (tick 1800) for 319 steps"
else
  bad "sleepy face off (first at step '${first11:-none}' want 1799, $n11 steps want 319)"
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL DYNAMIC TESTS PASSED"; else echo "DYNAMIC TESTS FAILED"; fi
exit "$fail"
