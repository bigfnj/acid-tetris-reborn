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
o_want="100 99 98 97 96 95 94 93 92 91 90 89 88 87 86 85 84 83 82 81 80 80 80 "
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
if echo "$bs_last" | grep -q 'scene=Options .* sel=0 .*music_vol=100' && [ "$bs_vol" = '"music_volume":100' ] \
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

echo
if [ "$fail" -eq 0 ]; then echo "ALL DYNAMIC TESTS PASSED"; else echo "DYNAMIC TESTS FAILED"; fi
exit "$fail"
