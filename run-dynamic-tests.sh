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
# (Uses the build's debug key B, which injects a four-row clear.)
echo "[11] level-up clear scored at the new level"
lu="$(dbg --live-demo --start-level=0 --piece-seed=42 --smoke-frames=120 --scripted-input='40:b,41:b:up,80:b,81:b:up' | tail -1)"
got="$(echo "$lu" | fld score) $(echo "$lu" | fld level) $(echo "$lu" | fld lines)"
[ "$got" = "4800 1 12" ] && pass "score/level/lines = $got" || bad "score/level/lines = '$got' (want '4800 1 12')"

# --- Test 12: the running level only changes at New Game (0x5FC) and by +1 on a ten-line crossing
# (0xEA7); the menu's Level row writes only the start level. Pause, set Level 5, resume, then two
# 4-row clears: at 8 lines the level is still 0, and the clear that crosses ten lines makes it 1
# (a level re-derived from the start level would read 5 and then 6; one re-derived only at the
# crossing would read 0 and then 6).
echo "[12] pause-menu Level row does not touch the running level"
keys='40:esc,41:esc:up,44:down,45:down:up,48:down,49:down:up,52:down,53:down:up'
keys="$keys,56:return,57:return:up,60:return,61:return:up,64:return,65:return:up,68:return,69:return:up,72:return,73:return:up"
keys="$keys,76:down,77:down:up,80:down,81:down:up,84:down,85:down:up,88:down,89:down:up,92:return,93:return:up"
keys="$keys,100:b,101:b:up,140:b,141:b:up"
pl="$(dbg --live-demo --start-level=0 --piece-seed=42 --smoke-frames=180 --scripted-input="$keys")"
mid="$(echo "$pl" | sed -n '121p')"; end="$(echo "$pl" | tail -1)"
got="$(echo "$mid" | fld level) $(echo "$mid" | fld lines) / $(echo "$end" | fld level) $(echo "$end" | fld lines)"
[ "$got" = "0 8 / 1 12" ] && pass "after Level 5 in the pause menu: level 0 at 8 lines, 1 at 12 lines" \
  || bad "level/lines = '$got' (want '0 8 / 1 12')"

# --- Test 13: srand at every New Game from the logic tick counter (0x32B0 at 0x6F8 reads
# [0x2D2A3]); --piece-seed=N forces N exactly (0 included). Real menu New Games with the screen
# fades on, so each New Game happens inside a Tick and its seed equals that step's tick=.
echo "[13] New Game seeds from the tick counter; --piece-seed is exact"
ng="$("$exe" --debug-state --reset-setup --scripted-input='820:enter,822:enter:up,1100:esc,1101:esc:up,1400:enter,1402:enter:up' \
      --smoke-frames=1600 2>&1 | grep 'Debug state:')"
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
fd="$("$exe" --debug-state --reset-setup --piece-seed=42 --scripted-input='820:enter,822:enter:up,1000:down' \
      --smoke-frames=1080 2>&1 | grep 'Debug state:' | grep 'scene=Gameplay' | grep 'fade=none')"
# g0 = the first gameplay step, the first line whose accumulator is non-zero; y at g0 and g0+1
first_y="$(echo "$fd" | awk '{match($0, / gravity=[0-9]+/); g=substr($0, RSTART+9, RLENGTH-9)+0;
  match($0, / pos=-?[0-9]+,-?[0-9]+/); split(substr($0, RSTART+5, RLENGTH-5), p, ",")}
  !s && g>0 {s=1} s {printf "%s%s", (n++?" ":""), p[2]; if (n==2) exit} END{print ""}')"
[ "$first_y" = "0 1" ] && pass "y at the first two gameplay steps = $first_y (soft drop from step 1)" \
  || bad "y at the first two gameplay steps = '$first_y' (want '0 1': Down held through the fade must count)"

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

echo
if [ "$fail" -eq 0 ]; then echo "ALL DYNAMIC TESTS PASSED"; else echo "DYNAMIC TESTS FAILED"; fi
exit "$fail"
