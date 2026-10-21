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

# Every run uses a suite-private SETUP.DAT seeded with the defaults, so the checks
# never write the player's file beside the game data and never depend on its bindings
# or volumes (a player binding Left to Z silently broke check [3]). $exe becomes a
# wrapper that adds --setup-dat.
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
setup="$tmp/SETUP.DAT"
command -v cygpath >/dev/null 2>&1 && setup="$(cygpath -m "$setup")"
printf '#!/usr/bin/env bash\nexec "%s" --setup-dat="%s" "$@"\n' "$exe" "$setup" > "$tmp/port"
chmod +x "$tmp/port"
exe="$tmp/port"
"$exe" --reset-setup --smoke-frames=1 > "$tmp/seed.txt" 2>&1
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
# pos= is the piece's reference x (table 0x184E3 offsets are added to it), so it goes negative
# at the left wall for every piece but the I (3-wide pieces stop at x=-1, the O at x=-2). The
# regex accepts a sign; the old [0-9]+ form silently dropped those lines and shifted the window.
echo "[3] hold-Left DAS auto-repeat"
minx="$("$exe" --live-demo --start-level=0 --no-fade --debug-state --smoke-frames=220 --scripted-input='120:left' 2>&1 \
        | grep -oE 'pos=-?[0-9]+,-?[0-9]+' | sed -n '121,200p' | awk -F'[=,]' '{print $2}' | sort -n | head -1)"
if [ -n "$minx" ] && [ "$minx" -le 1 ]; then
  pass "Left held repeats to the wall (min x=$minx)"
else
  bad "Left held did not auto-repeat (min x=${minx:-none}; expected <=1)"
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
# Two 375-tick splash pages (0x2998) end at tick 750 whatever the frame rate: frame 749 on 1-tick
# frames, frame 375 (ticks 750-751) on 2-tick frames.
m2t="${m2##*:}"
if [ "$m1" = "749:750" ] && [ "${m2%%:*}" = "375" ] && { [ "$m2t" = "750" ] || [ "$m2t" = "751" ]; } && [ "$to3" = "121" ]; then
  pass "menu after the splashes at frame:tick $m1 (1-tick frames) and $m2 (2-tick frames); top-out timer 121 at frame 40 of 3-tick frames (counted in the logic step)"
else
  bad "per-step counters off (menu frame:tick K=1 ${m1:-none} want 749:750, K=2 ${m2:-none} want 375:750-751; top-out timer at frame 40 of K=3: ${to3:-none}, want 121)"
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
  "$exe" --reset-setup --no-fade --debug-state --smoke-frame-ticks="$1" --smoke-frames=$((860 / $1 + 20)) 2>&1 | grep 'Debug state' \
    | awk '/music_ramp=fadein/ {match($0,/music_ramp_left=[0-9]+/); v[c++]=substr($0,RSTART+16,RLENGTH-16)} END {print v[2]-v[5]}'
}
r1="$(ramp_drop 1)"; r3="$(ramp_drop 3)"
if [ "$r1" = "3" ] && [ "$r3" = "9" ]; then
  pass "fade-in ramp falls $r1 over 3 one-tick frames and $r3 over 3 three-tick frames"
else
  bad "music ramp not on the tick (drop over 3 frames: K=1 ${r1:-none} want 3, K=3 ${r3:-none} want 9)"
fi

# ----------------------------------------------------------------------------------------------
# Gameplay-core checks (October 2026 audit). Each expectation comes from the original ATET.EXE
# (static read of 0x9C8 / 0x12AC / 0x1348 / 0x1D04 / 0x5E0 / 0x32B0, cross-checked in the Unicorn
# oracle and DOSBox-X), never from the port itself. One --debug-state line is printed per
# simulation step, so line N of the filtered output is step N-1.
dbg() { "$exe" --no-fade --debug-state "$@" 2>&1 | grep 'Debug state:'; }
fld() { grep -oE " $1=[^ ]+" | sed "s/^ $1=//"; }
norm() { tr '/' '\n' | sort | paste -sd/ -; }

# --- Test 4: piece ids, authored cells and spawn columns (table 0x184E3, spawn x=4 y=0 rot=0).
# Ids 0..6 = S Z J L T I O; the original's 3-wide pieces spawn on columns 5-7, the I on 4-7 and
# the O on 6-7 (DOSBox-X game-drop g1146/g1203, 178 oracle spawns). Each seed's first piece is the
# listed id (Watcom rand, entry 1 % 7).
echo "[4] piece table: ids S Z J L T I O = 0..6, authored rotation-0 cells, spawn x=4"
geom_ok=1
for spec in "5 0 5:1/6:1/6:0/7:0" "22 1 5:0/6:0/6:1/7:1" "7 2 5:0/6:0/7:0/7:1" "12 3 5:0/6:0/7:0/5:1" \
            "1 4 5:0/6:0/7:0/6:1" "6 5 4:0/5:0/6:0/7:0" "42 6 6:0/7:0/6:1/7:1"; do
  read -r seed want_id want_cells <<< "$spec"
  line="$(dbg --live-demo --piece-seed="$seed" --smoke-frames=1 | head -1)"
  got_id="$(echo "$line" | fld piece | cut -d: -f1)"
  got_cells="$(echo "$line" | fld cells | norm)"
  if [ "$got_id" != "$want_id" ] || [ "$got_cells" != "$(echo "$want_cells" | norm)" ]; then
    geom_ok=0
    echo "    seed $seed: id=${got_id:-?} cells=${got_cells:-none} (want id $want_id cells $want_cells)"
  fi
done
[ "$geom_ok" = 1 ] && pass "all seven ids spawn with the original cells" || bad "piece ids/cells differ from table 0x184E3"

# --- Test 5: rotation reads the authored states: A = rot+1, S = rot-1 (wrapping), x/y unchanged,
# no kicks (0xBB6..0xC94). T at x=4,y=0: rot1 = (6,-1)(6,0)(6,1)(5,0), rot3 = (6,-1)(6,0)(6,1)(7,0).
echo "[5] rotation: A=+1, S=-1, authored states, pivot fixed"
rot="$(dbg --live-demo --start-level=0 --piece-seed=1 --smoke-frames=110 \
        --scripted-input='60:a,62:a:up,80:s,82:s:up,100:s,102:s:up')"
r60="$(echo "$rot" | sed -n '61p')"; r80="$(echo "$rot" | sed -n '81p')"; r100="$(echo "$rot" | sed -n '101p')"
got="$(echo "$r60" | fld piece) $(echo "$r60" | fld cells | norm) | $(echo "$r80" | fld piece) $(echo "$r80" | fld cells | norm) | $(echo "$r100" | fld piece) $(echo "$r100" | fld cells | norm)"
want="4:1 $(echo '5:0/6:-1/6:0/6:1' | norm) | 4:0 $(echo '5:0/6:0/7:0/6:1' | norm) | 4:3 $(echo '6:-1/6:0/6:1/7:0' | norm)"
[ "$got" = "$want" ] && pass "T turns about cell (2,0): $got" || bad "rotation states differ (got '$got', want '$want')"

# pos y of each step, then: descent steps of the first piece, its landing (last descent) and the
# step its successor appears (y back to 0). Prints: descents min_interval max_interval lock-landing
lifecycle() { awk -F, '{y=$2+0; s=NR-1}
  NR>1 && !done && y==py+1 {d[++n]=s}
  NR>1 && !done && y<py {lock=s; done=1}
  {py=y}
  END{mi=999; ma=0; for(i=2;i<=n;i++){v=d[i]-d[i-1]; if(v<mi)mi=v; if(v>ma)ma=v}
      print n+0, mi, ma, (done&&n? lock-d[n] : -1), (done? lock : -1)}'; }

# --- Test 6/7: gravity. 0x9C8 adds the increment, and on reaching 0x10000 sets the accumulator to
# 0 (not acc-0x10000) and tries ONE row; the piece locks on the first gravity step whose row
# collides, so at level 9 (increment 0x1400) every row takes ceil(65536/5120) = 13 steps and the
# lock comes 13 steps after landing (DOSBox-X L9 13; no 24-step grounded timer, no move resets).
echo "[6] lock on the next failed gravity step (level 9)"
read -r n mi ma lockgap lockstep < <(dbg --live-demo --start-level=9 --piece-seed=42 --smoke-frames=400 | fld pos | lifecycle)
[ "$n" = 18 ] && [ "$lockgap" = 13 ] && pass "O lands after 18 rows and locks 13 steps later (step $lockstep)" \
  || bad "lock timing off (rows=$n, lock-landing=$lockgap; want 18 and 13)"
echo "[7] accumulator reset to 0 on each row (level 9, same run)"
[ "$mi" = 13 ] && [ "$ma" = 13 ] && pass "every row takes exactly 13 steps at level 9" \
  || bad "gravity intervals $mi..$ma (want 13..13: the accumulator must restart from 0)"

# --- Test 8: held-key repeat. Timers are decremented at the top of each step, the key acts when
# held with its timer at 0 and reloads 8 (even when blocked): moves at P, P+8, P+16, ... (0xA97..0xC94).
echo "[8] held Left: moves at P, P+8, P+16 ... to the wall"
moves="$(dbg --live-demo --start-level=0 --piece-seed=42 --smoke-frames=130 --scripted-input='60:left,120:left:up' \
         | fld pos | awk -F, '{x=$1+0; s=NR-1} NR>1 && x<px {printf "%s%d", (m++?" ":""), s} {px=x} END{print ""}')"
lastx="$(dbg --live-demo --start-level=0 --piece-seed=42 --smoke-frames=130 --scripted-input='60:left,120:left:up' \
         | tail -1 | fld pos | cut -d, -f1)"
[ "$moves" = "60 68 76 84 92 100" ] && [ "$lastx" = "-2" ] && pass "repeat every 8 steps, O stops at x=-2 (columns 0-1)" \
  || bad "repeat steps '$moves' last x=$lastx (want '60 68 76 84 92 100' and -2)"

# --- Test 9: soft drop. Down overrides the increment with 0x8000 (2 steps/row); the Down lockout
# 0x2C783 is only armed by a line clear (0xE76), never at a spawn, so with Down held across a plain
# lock the next piece descends 2 steps after its spawn (DOSBox-X game-das g1526 -> g1528), and the
# soft-dropped piece itself locks 2 steps after landing.
echo "[9] Down held across a plain lock: lock 2 steps after landing, next piece descends at spawn+2"
sd="$(dbg --live-demo --start-level=0 --piece-seed=42 --smoke-frames=200 --scripted-input='40:down' | fld pos)"
read -r n mi ma lockgap lockstep < <(echo "$sd" | lifecycle)
next_desc="$(echo "$sd" | awk -F, -v k="$lockstep" '{s=NR-1; y=$2+0} s>k && y==1 {print s; exit}')"
[ "$lockgap" = 2 ] && [ -n "$next_desc" ] && [ $((next_desc - lockstep)) = 2 ] \
  && pass "soft-drop lock at landing+2, next piece first row at spawn+2 (spawn step $lockstep)" \
  || bad "soft drop off (lock-landing=$lockgap, next piece first row at ${next_desc:-none} vs spawn $lockstep; want 2 and 2)"

# --- Test 10: line clear. All bookkeeping happens in the clearing step (lines/score/level/next/
# statistics, 0xE58..0x1011); the next 8n steps only run the collapse (0x1D04 ends a row on its
# 8th call, 0x9C8 returns at 0xA30) and step L+8n+1 is the first that runs gravity. --live-demo
# injects a 4-row clear before step 0 (L = -1): score/lines show from step 0, steps 0..31 are the
# 32 collapse calls (oracle lineclear-tetris3: 32), the state after step 31 is falling again and
# step 32 is the first gravity step (accumulator 0x200 at level 0).
echo "[10] line clear: score at the clear, collapse 8 steps per row"
lc="$(dbg --live-demo --start-level=0 --smoke-frames=40)"
s0="$(echo "$lc" | head -1 | fld score) $(echo "$lc" | head -1 | fld lines)"
p30="$(echo "$lc" | sed -n '31p' | fld phase)"; p31="$(echo "$lc" | sed -n '32p' | fld phase)"
firstg="$(echo "$lc" | fld gravity | awk '$1+0>0 {print NR-1; exit}')"
[ "$s0" = "1200 4" ] && [ "$p30" = collapse ] && [ "$p31" = falling ] && [ "$firstg" = 32 ] \
  && pass "4-row clear: score/lines in the clearing step, 32 collapse steps, first gravity step 32" \
  || bad "score/lines at step 0='$s0' (want '1200 4'), phase at 30/31='$p30/$p31' (want collapse/falling), first gravity step=${firstg:-none} (want 32)"

# --- Test 11: a clear that crosses a 10-line boundary scores with the NEW level (level++ at 0xEA7
# precedes the multiply at 0xFC2): 4 + 4 + 4 lines at start level 0 -> 1200 + 1200 + 2*1200.
# (Uses the developer key B, which injects a four-row clear; it needs --debug.)
echo "[11] level-up clear scored at the new level"
lu="$(dbg --debug --live-demo --start-level=0 --piece-seed=42 --smoke-frames=120 --scripted-input='40:b,41:b:up,80:b,81:b:up' | tail -1)"
got="$(echo "$lu" | fld score) $(echo "$lu" | fld level) $(echo "$lu" | fld lines)"
[ "$got" = "4800 1 12" ] && pass "score/level/lines = $got" || bad "score/level/lines = '$got' (want '4800 1 12')"

# --- Test 12: the running level only changes at New Game (0x5FC) and by +1 on a ten-line crossing
# (0xEA7); the menu's Level row writes only the start level. Pause, set Level 5, resume, then two
# 4-row clears: at 8 lines the level is still 0, and the clear that crosses ten lines makes it 1
# (a level re-derived from the start level would read 5 and then 6; one re-derived only at the
# crossing would read 0 and then 6).
echo "[12] pause-menu Level row does not touch the running level"
keys='40:esc,41:esc:up,100:down,101:down:up,130:down,131:down:up,160:down,161:down:up'
keys="$keys,190:return,191:return:up,200:return,201:return:up,210:return,211:return:up,220:return,221:return:up,230:return,231:return:up"
keys="$keys,260:down,261:down:up,290:down,291:down:up,320:down,321:down:up,350:down,351:down:up,380:return,381:return:up"
keys="$keys,500:b,501:b:up,600:b,601:b:up"
pl="$(dbg --debug --live-demo --start-level=0 --piece-seed=42 --smoke-frames=700 --scripted-input="$keys")"
mid="$(echo "$pl" | sed -n '561p')"; end="$(echo "$pl" | tail -1)"
got="$(echo "$mid" | fld level) $(echo "$mid" | fld lines) / $(echo "$end" | fld level) $(echo "$end" | fld lines)"
[ "$got" = "0 8 / 1 12" ] && pass "after Level 5 in the pause menu: level 0 at 8 lines, 1 at 12 lines" \
  || bad "level/lines = '$got' (want '0 8 / 1 12')"

# --- Test 13: srand at every New Game from the logic tick counter (0x32B0 at 0x6F8 reads
# [0x2D2A3]); --piece-seed=N forces N exactly (0 included). Real menu New Games with the screen
# fades on, so each New Game happens inside a Tick and its seed equals that step's tick=.
echo "[13] New Game seeds from the tick counter; --piece-seed is exact"
ng="$("$exe" --debug-state --reset-setup --scripted-input='1000:enter,1002:enter:up,1300:esc,1301:esc:up,1600:enter,1602:enter:up' \
      --smoke-frames=1900 2>&1 | grep 'Debug state:')"
changes="$(echo "$ng" | awk '{match($0, / seed=[0-9]+/); s=substr($0, RSTART+6, RLENGTH-6); match($0, / tick=[0-9]+/); t=substr($0, RSTART+6, RLENGTH-6)}
  NR>1 && s!=ps {printf "%s%s@%s", (c++?" ":""), s, t} {ps=s} END{print ""}')"
zero="$(dbg --live-demo --piece-seed=0 --smoke-frames=1 | head -1 | fld seed)"
ok=1; n=0; prev=""
for ch in $changes; do n=$((n+1)); s="${ch%@*}"; t="${ch#*@}"; [ "$s" = "$t" ] || ok=0; [ "$s" != "$prev" ] || ok=0; prev="$s"; done
[ "$ok" = 1 ] && [ "$n" = 2 ] && [ "$zero" = 0 ] && pass "two New Games seeded seed@tick = $changes; --piece-seed=0 -> 0" \
  || bad "seeding off (changes '$changes' want two seed==tick, distinct; --piece-seed=0 -> '${zero:-none}')"

# --- Test 14: keys are tracked during screen fades (ISR 0x0800 runs all the time). Down pressed in
# the New Game fade-in and held soft-drops from the first gameplay step: the accumulator reaches
# 0x10000 on the 2nd step (DOSBox-X gravity-input-lock-burst: Down from the fade acts on step 1).
echo "[14] Down pressed during the New Game fade-in soft-drops from the first gameplay step"
fd="$("$exe" --debug-state --reset-setup --piece-seed=42 --scripted-input='1000:enter,1002:enter:up,1150:down' \
      --smoke-frames=1260 2>&1 | grep 'Debug state:' | grep 'scene=Gameplay' | grep 'fade=none')"
# y:steps-so-far on the first two gameplay frames (frame 1 runs 1 step, frame 2 the pacer's 6)
first_y="$(echo "$fd" | awk '{match($0, / pos=-?[0-9]+,-?[0-9]+/); split(substr($0, RSTART+5, RLENGTH-5), p, ",");
  st=1; if (match($0, / steps=[0-9]+/)) st=substr($0, RSTART+7, RLENGTH-7)+0; c+=st;
  printf "%s%s:%s", (n++?" ":""), p[2], c; if (n==2) exit} END{print ""}')"
y1="${first_y%% *}"; y2="${first_y##* }"
if [ -n "$first_y" ] && [ "$y1" != "$y2" ] && [ "${y1%%:*}" = "$(( ${y1##*:} / 2 ))" ] && [ "${y2%%:*}" = "$(( ${y2##*:} / 2 ))" ] \
   && [ "${y2##*:}" -ge 3 ]; then
  pass "y:steps on the first two gameplay frames = $first_y (soft drop from step 1: one row per two steps)"
else
  bad "y:steps on the first two gameplay frames = '$first_y' (want y = steps/2 on both: Down held through the fade must count)"
fi

# --- Test 15: input is sampled once per step from the held table (0x9C8 reads HELD at its top): a
# press and release that both land before one step never move the piece (oracle tap2ms-miss); a
# press held into the step moves it at once.
echo "[15] a tap released before the step is not seen; a held press acts in its first step"
tp="$(dbg --live-demo --start-level=0 --piece-seed=42 --smoke-frames=70 --scripted-input='50:left,50:left:up,60:left,62:left:up' | fld pos | cut -d, -f1)"
x50="$(echo "$tp" | sed -n '51p')"; x60="$(echo "$tp" | sed -n '61p')"
[ "$x50" = 4 ] && [ "$x60" = 3 ] && pass "same-step tap ignored (x=$x50), held press moved at once (x=$x60)" \
  || bad "x after the same-step tap=$x50 (want 4), after the held press=$x60 (want 3)"

# --- Test 16: the Down lockout of a clear (0x348 = 840 at 0xE76) is counted down only on live steps
# (0xAF3; a collapse step returns at 0xA30 before it) and cleared only by a LIVE step with Down up
# (0xCD8), so a release and re-press inside the collapse changes nothing. --live-demo's injected
# 4-row clear (L = -1) is followed by 32 collapse steps (0..31); with Down down at 0, up at 10 and
# down again at 20, soft drop (increment 0x8000) starts on live step 840 = step 31 + 840 = 871
# (DOSBox-X gravity-input-lock-clearhold: soft drop resumed on normal step 840 after the clear).
echo "[16] Down lockout after a clear: frozen in the collapse, soft drop from live step 840"
lk="$(dbg --live-demo --start-level=0 --piece-seed=42 --smoke-frames=900 --scripted-input='0:down,10:down:up,20:down' | fld gravity)"
# first step whose increment was the soft-drop 0x8000 (level 0 gravity adds 0x200; a row resets to 0)
sd16="$(echo "$lk" | awk '{g=$1+0; s=NR-1} NR>1 && (g==pg+32768 || (g==0 && pg>=32768 && pg!=65024)) {print s; exit} {pg=g}')"
[ "$sd16" = 871 ] && pass "soft drop resumes at step 871 (live step 840; collapse steps and the in-collapse release do not count)" \
  || bad "first soft-drop step=${sd16:-none} (want 871: lockout 840 counted on live steps only, not cleared inside the collapse)"

# --- Frontend flow (dispatcher 0x3830, menus 0x4288/0x4984, credits 0x4f38). Boot facts:
# splash pages are 375 ticks (WARNING ends at 750), one tick of load allowance, the
# 0x6544 32-tick music wait, then the dispatcher entry at 783: 63 ticks of palette-out
# (0x64f0), 63 of title fade-in (0x63b8, 846..908), the 48-tick reveal (0x3df4, 909..956);
# the menu is interactive from tick 957. tick=T in --debug-state is the state after the
# tick a key at script step T-1 is read in.
# Runs that boot need SETUP.DAT: a missing file starts in Sound Setup (as the original
# does) and Initialize writes the defaults, so one warm-up run makes the rest deterministic.
"$exe" --reset-setup --smoke-frames=1 >/dev/null 2>&1
# first_tick <file> <flow>: the tick of the first debug line in that flow.
first_tick() { grep -m1 " flow=$2 " "$1" | grep -oE 'tick=[0-9]+' | grep -oE '[0-9]+'; }
count_flow() { grep -c " flow=$2 " "$1"; }
dyn_tmp="$(mktemp -d)"

echo "[4] boot timeline (375-tick splash pages, black gap, 63-tick fades, 48-tick reveal)"
"$exe" --reset-setup --debug-state --smoke-frames=1000 > "$dyn_tmp/boot.txt" 2>&1
b_gap="$(first_tick "$dyn_tmp/boot.txt" boot-gap)"; b_out="$(first_tick "$dyn_tmp/boot.txt" pal-out)"
b_in="$(first_tick "$dyn_tmp/boot.txt" title-in)"; b_rev="$(first_tick "$dyn_tmp/boot.txt" reveal)"
b_scr="$(first_tick "$dyn_tmp/boot.txt" screen)"
if [ "$b_gap" = "750" ] && [ "$b_out" = "783" ] && [ "$b_in" = "846" ] && [ "$b_rev" = "909" ] && [ "$b_scr" = "957" ]; then
  pass "boot: gap 750, palette-out 783, title fade-in 846, reveal 909, menu 957"
else
  bad "boot timeline off (gap=$b_gap want 750, pal-out=$b_out want 783, title-in=$b_in want 846, reveal=$b_rev want 909, menu=$b_scr want 957)"
fi

echo "[5] Esc on the main menu without a game never quits"
# A tap and a 60-tick hold; the original's 0x4288 only reads a held Esc when a game is
# parked ([0x2c72b] == 1), and only Exit Game quits.
"$exe" --reset-setup --debug-state --smoke-frames=1200 --scripted-input=1000:esc,1005:esc:up,1040:esc,1100:esc:up \
  > "$dyn_tmp/esc.txt" 2>&1
e_last="$(grep '^Debug state' "$dyn_tmp/esc.txt" | tail -1)"
if echo "$e_last" | grep -q "scene=Main Menu .* flow=screen .* tick=1200 " && ! grep -q "flow=quit-out" "$dyn_tmp/esc.txt"; then
  pass "Esc tap and hold on the main menu: still on the menu at tick 1200, no quit fade"
else
  bad "Esc on the main menu left the menu or quit (last: $(echo "$e_last" | grep -oE 'scene=[A-Za-z ]+ phase|flow=[a-z-]+|tick=[0-9]+' | tr '\n' ' '))"
fi

echo "[6] Exit Game: pulse gate, 48-tick conceal, 63-tick title fade-out, then quit"
"$exe" --reset-setup --debug-state --smoke-frames=1400 \
  --scripted-input=1000:up,1001:up:up,1030:up,1031:up:up,1060:enter,1061:enter:up > "$dyn_tmp/exit.txt" 2>&1
x_conceal="$(count_flow "$dyn_tmp/exit.txt" conceal)"; x_out="$(count_flow "$dyn_tmp/exit.txt" quit-out)"
x_c0="$(first_tick "$dyn_tmp/exit.txt" conceal)"
x_last="$(grep '^Debug state' "$dyn_tmp/exit.txt" | tail -1 | grep -oE 'tick=[0-9]+' | grep -oE '[0-9]+')"
if [ "$x_conceal" = "48" ] && [ "$x_out" = "63" ] && [ "${x_c0:-0}" -gt 1062 ] && [ "${x_last:-0}" = "$((x_c0 + 48 + 63 - 1))" ]; then
  pass "Exit Game: gate until $((x_c0 - 1)), conceal 48, title fade-out 63, quit after tick $x_last"
else
  bad "Exit Game flow off (conceal=$x_conceal want 48, quit-out=$x_out want 63, conceal start=$x_c0 want >1062 (gate), last tick=$x_last want conceal+110)"
fi

echo "[7] Options volume: one unit per tick while Left is held"
"$exe" --reset-setup --debug-state --smoke-frames=1240 \
  --scripted-input=1000:down,1001:down:up,1030:enter,1031:enter:up,1200:left,1220:left:up > "$dyn_tmp/opt.txt" 2>&1
o_series="$(grep '^Debug state' "$dyn_tmp/opt.txt" | sed -n '1200,1222p' | grep -oE 'music_vol=[0-9]+' | grep -oE '[0-9]+' | tr '\n' ' ')"
# 96 = floor(31 * 100 / 32): the level the 0x20-tick fade-in leaves (audio MU-08) until the first edit.
o_want="96 99 98 97 96 95 94 93 92 91 90 89 88 87 86 85 84 83 82 81 80 80 80 "
if grep -q "scene=Options" "$dyn_tmp/opt.txt" && [ "$o_series" = "$o_want" ]; then
  pass "Options: Left held 20 ticks lowers the music volume 100 -> 80, one unit per tick"
else
  bad "Options volume steps off (ticks 1200-1222: $o_series; want $o_want)"
fi

echo "[8] Credits: 397-tick page cycle (48 reveal + 299 hold + 1 + 48 conceal + 1)"
"$exe" --reset-setup --debug-state --smoke-frames=2000 \
  --scripted-input=1000:up,1001:up:up,1030:up,1031:up:up,1060:up,1061:up:up,1090:enter,1091:enter:up > "$dyn_tmp/cred.txt" 2>&1
c_revs="$(grep '^Debug state' "$dyn_tmp/cred.txt" | grep 'scene=Credits' | awk '{f="";t="";for(i=1;i<=NF;i++){if($i~/^flow=/)f=$i;if($i~/^tick=/)t=substr($i,6)}; if(f=="flow=reveal"&&pf!="flow=reveal")print t; pf=f}' | tr '\n' ' ')"
read -r c1 c2 c3 _ <<< "$c_revs"
if [ -n "${c3:-}" ] && [ $((c2 - c1)) = 397 ] && [ $((c3 - c2)) = 397 ]; then
  pass "Credits pages start at $c1, $c2, $c3 (397 ticks apart)"
else
  bad "Credits page cycle off (reveal starts: $c_revs; want 397 apart)"
fi

echo "[9] developer keys need --debug"
"$exe" --reset-setup --debug-state --smoke-frames=1100 --scripted-input=1000:g,1001:g:up > "$dyn_tmp/dbg0.txt" 2>&1
"$exe" --reset-setup --debug-state --debug --smoke-frames=1100 --scripted-input=1000:g,1001:g:up > "$dyn_tmp/dbg1.txt" 2>&1
d0="$(grep '^Debug state' "$dyn_tmp/dbg0.txt" | tail -1 | grep -oE 'scene=[A-Za-z ]+ phase' | sed 's/ phase//')"
d1="$(grep '^Debug state' "$dyn_tmp/dbg1.txt" | tail -1 | grep -oE 'scene=[A-Za-z ]+ phase' | sed 's/ phase//')"
if [ "$d0" = "scene=Main Menu" ] && [ "$d1" = "scene=Gameplay" ]; then
  pass "G on the menu does nothing by default ($d0) and jumps to play only with --debug ($d1)"
else
  bad "developer key G: default run ends in '$d0' (want scene=Main Menu), --debug run in '$d1' (want scene=Gameplay)"
fi
echo "[10] menu move sound: on the key release, never in Sound Setup"
# SFX slot 1 comes with the release that moves the row (0x4288 reads releases); Sound
# Setup runs before sound init and plays nothing. An event is printed before the
# debug line of the tick it was raised in.
"$exe" --reset-setup --debug-state --smoke-frames=1030 --scripted-input=1000:down,1010:down:up > "$dyn_tmp/sfx.txt" 2>&1
"$exe" --reset-setup --debug-state --smoke-frames=330 --scripted-input=300:down,310:down:up setup > "$dyn_tmp/sfx5.txt" 2>&1
s_n="$(grep -c 'Audio event: menu-row-move' "$dyn_tmp/sfx.txt")"
s_tick="$(grep -E '^Debug state|Audio event: menu-row-move' "$dyn_tmp/sfx.txt" | grep -B1 'Audio event: menu-row-move' | head -1 | grep -oE 'tick=[0-9]+' | grep -oE '[0-9]+')"
s5_n="$(grep -c 'Audio event: menu-row-move' "$dyn_tmp/sfx5.txt")"
s5_view="$(grep '^Debug state' "$dyn_tmp/sfx5.txt" | tail -1 | grep -oE 'scene=[A-Za-z ]+ phase' | sed 's/ phase//')"
if [ "$s_n" = "1" ] && [ "${s_tick:-0}" = "1010" ] && [ "$s5_n" = "0" ] && [ "$s5_view" = "scene=Sound Setup" ]; then
  pass "menu move sound raised once, in tick 1011 (Down released at step 1010); none in Sound Setup"
else
  bad "menu move sound off (main menu: $s_n events, after tick ${s_tick:-none} want 1 after 1010; Sound Setup ($s5_view): $s5_n events want 0)"
fi

# flows_after <file> <tick>: the distinct flow phases, in order, of the debug lines after <tick>.
flows_after() {
  grep '^Debug state' "$1" | awk -v t0="$2" '{f="";t=0;for(i=1;i<=NF;i++){if($i~/^flow=/)f=substr($i,6);if($i~/^tick=/)t=substr($i,6)+0}
    if(t>t0&&f!=p){printf "%s ",f;p=f}}'
}

echo "[11] SETUP.DAT is saved when the frontend is left, not on each edit"
# 0x3604 runs only from the dispatcher's exits (New Game / Return to Game after the title
# fade-out, Exit Game) and when the defaults are created; Options edits change memory only
# (IN-14, HS-26). Lower the music volume, go back with Esc, start a New Game: the one save
# before the end of the run is in the tick the title fade-out ends (the first pal-in tick).
"$exe" --reset-setup --debug-state --smoke-frames=1760 \
  --scripted-input=1000:down,1001:down:up,1030:enter,1031:enter:up,1200:left,1210:left:up,1300:esc,1306:esc:up,1450:up,1451:up:up,1500:enter,1501:enter:up \
  > "$dyn_tmp/save.txt" 2>&1
sv_ticks="$(sed '/^Audit summary/,$d' "$dyn_tmp/save.txt" | grep -oE 'SETUP.DAT saved at tick [0-9]+' | grep -oE '[0-9]+$' | tr '\n' ' ')"
sv_palin="$(first_tick "$dyn_tmp/save.txt" pal-in)"
sv_vol="$(grep '^Debug state' "$dyn_tmp/save.txt" | sed -n '1220p' | grep -oE 'music_vol=[0-9]+')"
if [ -n "$sv_palin" ] && [ "$sv_ticks" = "$sv_palin " ] && [ "$sv_vol" = "music_vol=90" ] && grep -q 'scene=Gameplay' "$dyn_tmp/save.txt"; then
  pass "volume edited to 90 at ticks 1201-1211 without a save; one save at tick $sv_palin, where the New Game title fade-out ends"
else
  bad "SETUP.DAT save timing off (saves at ticks: ${sv_ticks:-none}; want only the first pal-in tick ${sv_palin:-?}; $sv_vol at tick 1220, want music_vol=90)"
fi

echo "[12] Options and Sound Setup ignore Backspace, Enter on a volume row and (Sound Setup) Left/Right"
# Options 0x4584 and Sound Setup 0x5a94 read Esc from the held table and compare the release
# history only with Enter, Up and Down; Options' Enter acts on rows 2 and 3 only; Sound Setup
# has no Left/Right (IN-VO-01, IN-VP-01, IN-04, FE-11, IN-11).
# The volume is read from the trace (settings.music_volume, the value the Options row and
# SETUP.DAT use); the debug line's music_vol is the player's applied volume.
bs_trace="$dyn_tmp/bs.jsonl"; ss_trace="$dyn_tmp/ss.jsonl"
command -v cygpath >/dev/null 2>&1 && bs_trace="$(cygpath -m "$bs_trace")" && ss_trace="$(cygpath -m "$ss_trace")"
"$exe" --reset-setup --debug-state --trace="$bs_trace" --smoke-frames=1400 \
  --scripted-input=1000:down,1001:down:up,1030:enter,1031:enter:up,1200:enter,1201:enter:up,1250:backspace,1251:backspace:up \
  > "$dyn_tmp/bs.txt" 2>&1
bs_last="$(grep '^Debug state' "$dyn_tmp/bs.txt" | tail -1)"
bs_vol="$(tail -1 "$dyn_tmp/bs.jsonl" 2>/dev/null | grep -oE '"music_volume":[0-9]+')"
"$exe" --reset-setup --debug-state --trace="$ss_trace" --smoke-frames=500 \
  --scripted-input=300:left,320:left:up,350:right,370:right:up,400:backspace,401:backspace:up setup > "$dyn_tmp/ss.txt" 2>&1
ss_first="$(grep -m1 '"flow":"screen"' "$dyn_tmp/ss.jsonl" 2>/dev/null | grep -oE '"row_texts":\[[^]]*\]')"
ss_last="$(tail -1 "$dyn_tmp/ss.jsonl" 2>/dev/null | grep -oE '"row_texts":\[[^]]*\]')"
ss_view="$(grep '^Debug state' "$dyn_tmp/ss.txt" | tail -1 | grep -oE 'scene=[A-Za-z ]+ phase' | sed 's/ phase//')"
# music_vol 96 = floor(31 * 100 / 32): the player's level after the boot fade-in (audio MU-08), untouched.
if echo "$bs_last" | grep -q 'scene=Options .* sel=0 .*music_vol=96' && [ "$bs_vol" = '"music_volume":100' ] \
   && [ -n "$ss_first" ] && [ "$ss_first" = "$ss_last" ] && [ "$ss_view" = "scene=Sound Setup" ]; then
  pass "Options: Enter on Music Volume and Backspace change nothing; Sound Setup: Left, Right, Backspace change nothing"
else
  bad "extra menu keys act (Options last: $(echo "$bs_last" | grep -oE 'scene=[A-Za-z ]+ phase|sel=[0-9]+|music_vol=[0-9]+' | tr '\n' ' ')${bs_vol:-no trace volume}; Sound Setup $ss_view, rows first ${ss_first:-none} last ${ss_last:-none})"
fi

echo "[13] Esc held through the resume fade, or tapped during the New Game fade-in, reopens the menu"
# The dispatcher clears the release history (0x3941) BEFORE its exit fades; a release that
# arrives later is read by the first gameplay tick (main 0x4b1), which reopens the menu with
# Return to Game selected (IN-VO-02, IN-06). A 5-tick hold resumes for good.
"$exe" --reset-setup --debug-state --smoke-frames=2000 \
  --scripted-input=958:enter,960:enter:up,1300:esc,1305:esc:up,1550:esc,1700:esc:up > "$dyn_tmp/bounce.txt" 2>&1
"$exe" --reset-setup --debug-state --smoke-frames=2000 \
  --scripted-input=958:enter,960:enter:up,1300:esc,1305:esc:up,1550:esc,1555:esc:up > "$dyn_tmp/resume.txt" 2>&1
"$exe" --reset-setup --debug-state --smoke-frames=1400 \
  --scripted-input=958:enter,960:enter:up,1090:esc,1093:esc:up > "$dyn_tmp/fadeesc.txt" 2>&1
r_bounce="$(flows_after "$dyn_tmp/bounce.txt" 1550)"; r_resume="$(flows_after "$dyn_tmp/resume.txt" 1550)"
r_fade="$(flows_after "$dyn_tmp/fadeesc.txt" 1000)"
r_blast="$(grep '^Debug state' "$dyn_tmp/bounce.txt" | tail -1)"
r_rlast="$(grep '^Debug state' "$dyn_tmp/resume.txt" | tail -1)"
if [ "$r_bounce" = "screen conceal title-out pal-in pal-out title-in reveal screen " ] \
   && echo "$r_blast" | grep -q 'scene=Main Menu .* sel=7 ' \
   && [ "$r_resume" = "screen conceal title-out pal-in none " ] && echo "$r_rlast" | grep -q 'scene=Gameplay' \
   && echo "$r_fade" | grep -q 'pal-in pal-out title-in reveal screen'; then
  pass "held Esc: resume then back to the menu on Return to Game; 5-tick Esc: stays in play; Esc in the fade-in: menu right after it"
else
  bad "Esc around the exit fades off (held: '$r_bounce' want 'screen conceal title-out pal-in pal-out title-in reveal screen', last sel $(echo "$r_blast" | grep -oE 'sel=[0-9]+'); tap: '$r_resume'; fade-in tap: '$r_fade')"
fi

echo "[14] keypad arrows move the menu cursor"
# The ISR stores keypad 8/2 as 0x48/0x50 (no E0 prefix); 0x4288 accepts them besides the grey
# arrows 0xC8/0xD0 (IN-09, menu part).
"$exe" --reset-setup --debug-state --smoke-frames=1060 --scripted-input=1000:kp2,1001:kp2:up > "$dyn_tmp/kp.txt" 2>&1
k_last="$(grep '^Debug state' "$dyn_tmp/kp.txt" | tail -1)"
if echo "$k_last" | grep -q 'scene=Main Menu .* sel=1 '; then
  pass "keypad 2 released at step 1001 moves the main menu from New Game to Options"
else
  bad "keypad 2 did not move the menu (last: $(echo "$k_last" | grep -oE 'scene=[A-Za-z ]+ phase|sel=[0-9]+' | tr '\n' ' '))"
fi
rm -rf "$dyn_tmp"

# --- Test 4: clear faces live 160 steps; a clear within 160 ticks of the previous one gets
# the fast-follow face (original 0x0F54..0x0F9F, life 0xA0 -> face up for 159 steps).
# --live-demo clears four rows at tick 0; a single (C, a developer key: --debug) at step 60 must
# show face 1, not 2.
echo "[4] clear-face lifetime + fast-follow face"
alerts="$("$exe" --debug --live-demo --debug-state --smoke-frames=400 --scripted-input='60:c,62:c:up' 2>&1 \
          | grep -o 'Debug state: .*' | grep -oE ' alert=-?[0-9]+' | sed 's/ alert=//')"
a60="$(echo "$alerts" | sed -n '61p')"
run1="$(echo "$alerts" | sed -n '61,$p' | awk '$1==1{n++; next} {exit} END{print n+0}')"
if [ "$a60" = "1" ] && [ "$run1" = "159" ]; then
  pass "single 60 ticks after a clear shows face 1 for 159 steps"
else
  bad "fast-follow/lifetime off (face at step 60 = '$a60' want 1, steps with face 1 = $run1 want 159)"
fi

# --- Test 5: sleepy face exactly 1800 ticks after the last clear, up for 319 steps
# (original 0x10A6: tick - last clear == 0x708, life 0x140). --live-demo clears at tick 0 and its
# step s runs at tick s (the main loop's clock), so tick 1800 is step 1800.
echo "[5] sleepy face timing"
alerts="$("$exe" --live-demo --debug-state --smoke-frames=2200 2>&1 \
          | grep -o 'Debug state: .*' | grep -oE ' alert=-?[0-9]+' | sed 's/ alert=//')"
first11="$(echo "$alerts" | awk '$1==11{print NR-1; exit}')"
n11="$(echo "$alerts" | awk '$1==11{n++} END{print n+0}')"
if [ "$first11" = "1800" ] && [ "$n11" = "319" ]; then
  pass "sleepy face from step 1800 (tick 1800) for 319 steps"
else
  bad "sleepy face off (first at step '${first11:-none}' want 1800 = tick 1800, $n11 steps want 319)"
fi

echo
if [ "$fail" -eq 0 ]; then echo "ALL DYNAMIC TESTS PASSED"; else echo "DYNAMIC TESTS FAILED"; fi
exit "$fail"
