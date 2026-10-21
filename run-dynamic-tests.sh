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

# --- Game clock and frame pacing (checks [4]-[8]) ----------------------------------------------
# The original's logic runs on PIT channel 0 at 1193181.818/19886 = 60.00109716 Hz. Each frame runs
# n logic steps then flips; its pacer flips first, then waits for the next tick edge and returns the
# ticks since its previous return capped at 6 (n for the next frame). Splash pages and the gameplay
# palette fades are tick loops that never call the pacer, so the first pacer return after them is 6;
# the pacer is rebased only when gameplay resumes from the frontend (Return to Game, and every New
# Game except the first after boot). Expectations are the original's, measured on the Unicorn oracle
# (runs tb-o1 / tbv-o1: first drop 122 ticks after the first gameplay step of the first game, 127 for
# a New Game from the in-game menu, one step and no burst on Return to Game, one 6-step frame in the
# title fade-in after a pause; starvation runs: steps == min(ticks per frame, 6) on all 418 returns).
# The debug-state line carries tick= (the frame's PIT tick), steps= (logic steps run for the frame)
# and loop=tick|paced. --smoke-frame-ticks=K makes every smoke frame take K ticks (a slow machine).
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
field() { sed -n "s/.* $1=\([0-9]*\).*/\1/p"; }

echo "[4] frame pacer unit tests (fake clock: tick rate, flip-then-wait, cap 6, rebase)"
if "$exe" --selftest-pacer --smoke-frames=1 > "$tmp/selftest.txt" 2>&1 && grep -q "pacer-selftest: ALL PASS" "$tmp/selftest.txt"; then
  pass "pacer self-test ($(grep -c 'pacer-selftest: PASS' "$tmp/selftest.txt") checks, incl. 216003 ticks per hour)"
else
  bad "pacer self-test: $(grep -m3 'FAIL' "$tmp/selftest.txt" | tr '\n' ' ')"
fi

# Game time per tick must not depend on the frame rate: on a machine where every frame takes K ticks
# the pacer runs K steps per frame (up to 6), so the first gravity drop of the --live-demo board
# lands on the same tick (within the K-tick frame granularity) for K = 1, 2, 3.
echo "[5] catch-up: game time follows the tick, not the frame rate"
drop_at() { # K -> frame:tick of the first frame whose piece row is > 0
  "$exe" --live-demo --start-level=0 --debug-state --smoke-frame-ticks="$1" --smoke-frames=$((200 / $1 + 20)) 2>&1 \
    | grep 'Debug state' | awk '{match($0,/pos=[0-9]+,[0-9]+/); split(substr($0,RSTART+4,RLENGTH-4),p,",");
        t=NR-1; if (match($0,/tick=[0-9]+/)) t=substr($0,RSTART+5,RLENGTH-5); if (p[2] > 0) {print NR-1 ":" t; exit}}'
}
d1="$(drop_at 1)"; d2="$(drop_at 2)"; d3="$(drop_at 3)"
f1="${d1%%:*}"; t1="${d1##*:}"; f2="${d2%%:*}"; t2="${d2##*:}"; f3="${d3%%:*}"; t3="${d3##*:}"
k10="$("$exe" --live-demo --debug-state --smoke-frame-ticks=10 --smoke-frames=20 2>&1 | grep 'Debug state' | sed -n '2,20p' \
       | awk '{s=1; if (match($0,/steps=[0-9]+/)) s=substr($0,RSTART+6,RLENGTH-6); print s}' | sort -u | tr '\n' ' ')"
if [ -n "$d1" ] && [ -n "$d2" ] && [ -n "$d3" ] && [ $((t2 - t1)) -ge 0 ] && [ $((t2 - t1)) -le 1 ] \
   && [ $((t3 - t1)) -ge 0 ] && [ $((t3 - t1)) -le 2 ] && [ $((2 * f2 - t2)) -ge -2 ] && [ $((2 * f2 - t2)) -le 2 ] \
   && [ $((3 * f3 - t3)) -ge -3 ] && [ $((3 * f3 - t3)) -le 3 ] && [ "$k10" = "6 " ]; then
  pass "first drop at frame:tick $d1 / $d2 / $d3 for 1 / 2 / 3-tick frames (same tick); 10-tick frames run 6 steps each"
else
  bad "catch-up off (first drop frame:tick K=1 ${d1:-none} K=2 ${d2:-none} K=3 ${d3:-none}, want the same tick on 1/K the frames; steps of 10-tick frames: '${k10}', want '6 ')"
fi

# Every per-step counter advances per step, not per presented frame (it used to advance in Render):
# the splash pages last 720 ticks and the top-out timer counts ticks whatever the frame rate.
echo "[6] per-step presentation counters advance in catch-up steps"
menu_at() { "$exe" --reset-setup --no-fade --debug-state --smoke-frame-ticks="$1" --smoke-frames=$((800 / $1)) 2>&1 \
  | grep 'Debug state' | awk '/scene=Main Menu/ {match($0,/tick=[0-9]+/); print NR-1 ":" substr($0,RSTART+5,RLENGTH-5); exit}'; }
m1="$(menu_at 1)"; m2="$(menu_at 2)"
to3="$("$exe" --topout-demo --debug-state --smoke-frame-ticks=3 --smoke-frames=60 2>&1 | grep 'Debug state' | field topout | sed -n '41p')"
if [ "${m1%%:*}" = "720" ] && [ "${m2%%:*}" = "360" ] && [ "$to3" = "120" ]; then
  pass "menu after the splashes at frame:tick $m1 (1-tick frames) and $m2 (2-tick frames); top-out timer 120 at frame 40 of 3-tick frames"
else
  bad "per-step counters off (menu frame:tick K=1 ${m1:-none} want 720:..., K=2 ${m2:-none} want 360:...; top-out timer at frame 40 of K=3: ${to3:-none}, want 120)"
fi

# Render must not change game state: every frame is drawn twice and any step counter that moves
# between the two draws fails the run (exit 3). Covers splash, menu, fades, gameplay, top-out, entry.
echo "[7] Render changes no game state"
"$exe" --reset-setup --check-render-purity --scripted-input=1100:enter,1102:enter:up,1550:esc,1552:esc:up \
  --smoke-frames=1900 > "$tmp/purity1.txt" 2>&1; p1=$?
"$exe" --reset-setup --topout-demo --check-render-purity --smoke-frames=900 > "$tmp/purity2.txt" 2>&1; p2=$?
if [ "$p1" = "0" ] && [ "$p2" = "0" ] && grep -q "Render purity: PASS" "$tmp/purity1.txt" && grep -q "Render purity: PASS" "$tmp/purity2.txt"; then
  pass "$(grep -o 'Render purity: PASS ([0-9]* frames' "$tmp/purity1.txt" | grep -o '[0-9]*') + $(grep -o 'Render purity: PASS ([0-9]* frames' "$tmp/purity2.txt" | grep -o '[0-9]*') frames drawn twice, no state change"
else
  bad "Render changed state (exit $p1/$p2): $(grep -m2 -A2 'Render purity: FAIL' "$tmp/purity1.txt" "$tmp/purity2.txt" | tr '\n' ' ' | cut -c1-400)"
fi

# The pacer's baseline rules, end to end: boot, New Game (first game after boot), Esc, New Game
# (second game), Esc, Up to Return to Game, Enter.
echo "[8] pacer baseline rules vs the original (first game burst, rebase on resume and later games)"
"$exe" --reset-setup --debug-state --smoke-frames=3600 \
  --scripted-input=1100:enter,1102:enter:up,1700:esc,1702:esc:up,2150:enter,2152:enter:up,2700:esc,2702:esc:up,3150:up,3152:up:up,3160:enter,3162:enter:up \
  > "$tmp/rules.txt" 2>&1
read -r games g1s2 g1drop g2max g2drop g3s1 g3max fbursts < <(grep 'Debug state' "$tmp/rules.txt" | awk '
  { n++
    match($0,/scene=.* phase=/); sc[n]=substr($0,RSTART+6,RLENGTH-13)
    match($0,/pos=[0-9]+,[0-9]+/); split(substr($0,RSTART+4,RLENGTH-4),p,","); y[n]=p[2]+0
    match($0,/gravity=[0-9]+/); g[n]=substr($0,RSTART+8,RLENGTH-8)
    match($0,/fade=[a-z]+/); fd[n]=substr($0,RSTART+5,RLENGTH-5)
    tk[n]=n-1; if (match($0,/tick=[0-9]+/)) tk[n]=substr($0,RSTART+5,RLENGTH-5)+0
    st[n]=1; if (match($0,/steps=[0-9]+/)) st[n]=substr($0,RSTART+6,RLENGTH-6)+0
    lp[n]="paced"; if (match($0,/loop=[a-z]+/)) lp[n]=substr($0,RSTART+5,RLENGTH-5) }
  function playing(i) { return sc[i] == "Gameplay" && fd[i] == "none" && lp[i] == "paced" }
  END {
    games = 0; i = 1
    while (i <= n) {
      if (playing(i) && !playing(i - 1)) {
        f = i
        while (f <= n && playing(f) && g[f] == g[f - 1] && y[f] == y[f - 1]) f++
        if (f > n || !playing(f)) { i = f + 1; continue }
        games++; first[games] = f; s1[games] = st[f]; s2[games] = st[f + 1]; mx = 0; d = -1
        for (k = f; k <= n && playing(k); k++) { if (st[k] > mx) mx = st[k]; if (d < 0 && y[k] > y[f]) d = tk[k] - tk[f] }
        mxs[games] = mx; dd[games] = d; i = k; continue
      }
      i++
    }
    fb = 0; for (k = first[1]; k <= n; k++) if (sc[k] != "Gameplay" && st[k] == 6) fb++
    print games, s2[1] + 0, dd[1] + 0, mxs[2] + 0, dd[2] + 0, s1[3] + 0, mxs[3] + 0, fb }')
if [ "$games" = "3" ] && [ "$g1s2" = "6" ] && [ "$g1drop" = "122" ] && [ "$g2max" = "1" ] && [ "$g2drop" = "127" ] \
   && [ "$g3s1" = "1" ] && [ "$g3max" = "1" ] && [ "$fbursts" = "2" ]; then
  pass "first game: 6-step 2nd frame, drop +122 ticks; 2nd game: no burst, +127; Return to Game: 1 step, no burst; 1 six-step title frame per pause"
else
  bad "pacer rules off (games=$games want 3; game 1 2nd-frame steps=$g1s2 want 6, drop +$g1drop want 122; game 2 max steps=$g2max want 1, drop +$g2drop want 127; resume first=$g3s1 max=$g3max want 1/1; title bursts=$fbursts want 2)"
fi

# The music volume ramp is stepped by the original's timer interrupt once per PIT tick (0x6E51 ->
# 0x6620), whatever the frame rate: on 3-tick frames the 32-tick boot fade-in must drop 3 per frame.
echo "[9] music volume ramp steps once per tick"
ramp_drop() { # K -> music_ramp_left at fade-in frames F+2 minus F+5 (F = first fade-in frame)
  "$exe" --reset-setup --no-fade --debug-state --smoke-frame-ticks="$1" --smoke-frames=$((760 / $1 + 20)) 2>&1 | grep 'Debug state' \
    | awk '/music_ramp=fadein/ {match($0,/music_ramp_left=[0-9]+/); v[c++]=substr($0,RSTART+16,RLENGTH-16)} END {print v[2]-v[5]}'
}
r1="$(ramp_drop 1)"; r3="$(ramp_drop 3)"
if [ "$r1" = "3" ] && [ "$r3" = "9" ]; then
  pass "fade-in ramp falls $r1 over 3 one-tick frames and $r3 over 3 three-tick frames"
else
  bad "music ramp not on the tick (drop over 3 frames: K=1 ${r1:-none} want 3, K=3 ${r3:-none} want 9)"
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL DYNAMIC TESTS PASSED"; else echo "DYNAMIC TESTS FAILED"; fi
exit "$fail"
