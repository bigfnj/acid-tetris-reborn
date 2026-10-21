#!/usr/bin/env bash
# Engineering checks for the port: where SETUP.DAT lives, that the test suites
# never touch the player's settings, and that developer keys stay off unless
# --debug is given. Needs no original executable, only the port and a game data
# file. Works on Linux and on Windows Git Bash. Exits non-zero on any failure.
#
# Usage: run-engineering-tests.sh <path-to-exe> <data-file> [--suite-isolation]
#   <data-file>         ATET.DAT, saba.dat, or an ACiD Tetris / SABA zip
#   --suite-isolation   also run run-smokes.sh and run-dynamic-tests.sh and check
#                       that they neither write nor read the player's SETUP.DAT.
#                       The suites read the data beside the exe, so the player's
#                       SETUP.DAT is the one in the exe's folder.
set -u

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exe="${1:-}"
data="${2:-}"
isolation="${3:-}"
[ -n "$exe" ] && [ -x "$exe" ] || { echo "FATAL: exe not found (arg 1): $exe"; exit 2; }
[ -n "$data" ] && [ -f "$data" ] || { echo "FATAL: data file not found (arg 2): $data"; exit 2; }
exe="$(cd "$(dirname "$exe")" && pwd)/$(basename "$exe")"
data="$(cd "$(dirname "$data")" && pwd)/$(basename "$data")"
exe_dir="$(dirname "$exe")"
exe_name="$(basename "$exe")"

export SDL_VIDEODRIVER=offscreen SDL_AUDIODRIVER=dummy

# sha256 of the 178-byte SETUP.DAT the port writes on a first run. It equals the
# defaults the original game writes (signature, Autodetect, 44100 Hz, stereo,
# 16-bit, keys D0 CB CD 1E 1F, music 100, SFX 90, track 0, default high scores).
defaults_sha=1d2b783a9c3640322d8098e61f37db9158546e3e3560bd0698b906ecc97bdc04

# Runtime libraries that must travel with a relocated copy of the exe.
runtime_files=()
case "$exe_name" in
  *.exe)
    for f in "$exe_dir"/*.dll; do [ -f "$f" ] && runtime_files+=("$f"); done ;;
  *)
    for d in "$exe_dir" "$exe_dir/_deps/sdl3-build"; do
      for f in "$d"/libSDL3.so*; do [ -e "$f" ] && runtime_files+=("$f"); done
      [ "${#runtime_files[@]}" -gt 0 ] && break
    done ;;
esac

# Paths handed to the exe as arguments must be native on Windows.
native() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

tmp="$(mktemp -d)"
pref_file=""
pref_existed=0
player_file=""
player_existed=0
cleanup() {
  if [ -n "$pref_file" ]; then
    if [ "$pref_existed" = 1 ]; then cp "$tmp/pref-backup" "$pref_file"; else
      rm -f "$pref_file"; rmdir "$(dirname "$pref_file")" 2> /dev/null; fi
  fi
  if [ -n "$player_file" ]; then
    if [ "$player_existed" = 1 ]; then cp "$tmp/player-backup" "$player_file"; else rm -f "$player_file"; fi
  fi
  rm -rf "$tmp"
}
trap cleanup EXIT

fails=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; fails=$((fails + 1)); }
note() { echo "  NOTE: $1"; }

relocate() { mkdir -p "$1" && cp "$exe" "$1/" && for f in "${runtime_files[@]}"; do cp -P "$f" "$1/"; done; }
states() { tr -d '\r' < "$1" | grep 'Debug state:'; }
first_scene() { states "$1" | head -1 | sed -n 's/^.*Debug state: scene=\(.*\) phase=.*$/\1/p'; }
scene_at() { states "$1" | sed -n "$(($2 + 1))p" | sed -n 's/^.*Debug state: scene=\(.*\) phase=.*$/\1/p'; }
size_of() { wc -c < "$1" | tr -d ' '; }
sha_of() { sha256sum "$1" | cut -d' ' -f1; }
first_diff_frame() { # two state files (one line per frame) -> 0-based frame of the first difference, or "none"
  if cmp -s "$1" "$2"; then echo none; return; fi
  local h l
  h="$(diff "$1" "$2" | head -1)"
  case "$h" in
    *a*) echo "${h%%a*}" ;;                                 # the second run has extra frames after line N
    *) l="${h%%[cd]*}"; l="${l%%,*}"; echo $((l - 1)) ;;    # first changed or missing line
  esac
}

cwd="$tmp/unrelated-cwd"
mkdir -p "$cwd"
echo "== engineering checks: $exe (data: $(basename "$data")) =="

# ---------------------------------------------------------------------------
echo "[1] SETUP.DAT is resolved at run time beside the data the port loaded"
A="$tmp/portable"
relocate "$A"; cp "$data" "$A/"
(cd "$cwd" && "$A/$exe_name" --smoke-frames=5 --debug-state) > "$tmp/1a.txt" 2>&1
rc=$?
[ "$rc" = 0 ] && pass "first launch from an unrelated folder exits 0" || fail "first launch exit $rc"
if [ -f "$A/SETUP.DAT" ] && [ "$(size_of "$A/SETUP.DAT")" = 178 ] && [ "$(sha_of "$A/SETUP.DAT")" = "$defaults_sha" ]; then
  pass "first launch wrote the 178-byte default SETUP.DAT beside the exe and data"
else
  fail "no default SETUP.DAT beside the exe and data (folder holds: $(ls "$A" | tr '\n' ' '))"
fi
[ ! -e "$cwd/SETUP.DAT" ] && pass "nothing written to the working directory" || fail "SETUP.DAT written to the working directory"
(cd "$cwd" && "$A/$exe_name" --smoke-frames=5 --debug-state) > "$tmp/1b.txt" 2>&1
s="$(first_scene "$tmp/1b.txt")"
[ "$s" = "DDD Splash" ] && pass "second launch read it back (boots to the DDD splash)" \
  || fail "second launch booted to '$s', expected 'DDD Splash' (settings did not persist)"
leak="$(LC_ALL=C grep -a -o -E '([A-Za-z]:|/)[^[:cntrl:]]*[/\\]SETUP\.DAT' "$exe" | head -1)"
[ -z "$leak" ] && pass "no absolute SETUP.DAT path compiled into the executable" \
  || fail "the executable embeds a build-machine path: $leak"

# ---------------------------------------------------------------------------
echo "[2] data only in the working directory: SETUP.DAT goes with the data"
A2="$tmp/exe-only"
B2="$tmp/data-in-cwd"
relocate "$A2"; mkdir -p "$B2"; cp "$data" "$B2/"
(cd "$B2" && "$A2/$exe_name" --smoke-frames=5 --debug-state) > "$tmp/2a.txt" 2>&1
rc=$?
if [ "$rc" = 0 ] && [ -f "$B2/SETUP.DAT" ] && [ "$(sha_of "$B2/SETUP.DAT")" = "$defaults_sha" ] && [ ! -e "$A2/SETUP.DAT" ]; then
  pass "SETUP.DAT created beside the data in the working directory, none beside the exe"
else
  fail "exit $rc; beside data: $([ -f "$B2/SETUP.DAT" ] && echo yes || echo no); beside exe: $([ -e "$A2/SETUP.DAT" ] && echo yes || echo no)"
fi
(cd "$B2" && "$A2/$exe_name" --smoke-frames=5 --debug-state) > "$tmp/2b.txt" 2>&1
s="$(first_scene "$tmp/2b.txt")"
[ "$s" = "DDD Splash" ] && pass "second launch read it back" || fail "second launch booted to '$s'"

# ---------------------------------------------------------------------------
echo "[3] unwritable SETUP.DAT location: logged per-user fallback"
A3="$tmp/unwritable"
relocate "$A3"; cp "$data" "$A3/"
mkdir -p "$A3/SETUP.DAT"   # a folder in the way: the file can be neither read nor written
xdg=()
case "$exe_name" in
  *.exe)
    if [ -n "${APPDATA:-}" ] && command -v cygpath > /dev/null 2>&1; then
      pref_file="$(cygpath -u "$APPDATA")/saba-reborn/SETUP.DAT"
    fi ;;
  *)
    xdg=(env "XDG_DATA_HOME=$tmp/xdg")
    pref_file="$tmp/xdg/saba-reborn/SETUP.DAT" ;;
esac
if [ -z "$pref_file" ]; then
  note "SKIPPED: cannot locate the per-user folder on this host (APPDATA/cygpath missing); fallback NOT checked"
  fails=$((fails + 1))
else
  if [ -f "$pref_file" ]; then pref_existed=1; cp "$pref_file" "$tmp/pref-backup"; rm -f "$pref_file"; fi
  (cd "$cwd" && "${xdg[@]}" "$A3/$exe_name" --smoke-frames=5 --debug-state) > "$tmp/3a.txt" 2>&1
  rc=$?
  [ "$rc" = 0 ] && pass "launch exits 0" || fail "launch exit $rc"
  grep -q 'is not writable; using the per-user copy' "$tmp/3a.txt" \
    && pass "fallback is logged: $(tr -d '\r' < "$tmp/3a.txt" | grep -m1 -o 'SETUP.DAT: .*')" \
    || fail "no fallback log line"
  [ -d "$A3/SETUP.DAT" ] && pass "the unwritable location was left alone" || fail "the unwritable location was replaced"
  [ -f "$pref_file" ] && [ "$(size_of "$pref_file")" = 178 ] && pass "per-user SETUP.DAT written" \
    || fail "per-user SETUP.DAT missing: $pref_file"
  (cd "$cwd" && "${xdg[@]}" "$A3/$exe_name" --smoke-frames=5 --debug-state) > "$tmp/3b.txt" 2>&1
  s="$(first_scene "$tmp/3b.txt")"
  [ "$s" = "DDD Splash" ] && pass "second launch read the per-user copy back" || fail "second launch booted to '$s'"
fi

# ---------------------------------------------------------------------------
echo "[4] non-ASCII folder names load the data and persist settings"
for name in "$(printf 'J\303\274rgen_dir')" "$(printf '\320\230\320\263\321\200\320\260_dir')"; do
  D="$tmp/$name"
  relocate "$D"; cp "$data" "$D/"
  (cd "$cwd" && "$D/$exe_name" --smoke-frames=3) > "$tmp/4.txt" 2>&1
  rc=$?
  if [ "$rc" = 0 ] && [ -f "$D/SETUP.DAT" ]; then
    pass "$name: exit 0, SETUP.DAT written"
  else
    fail "$name: exit $rc, SETUP.DAT $([ -f "$D/SETUP.DAT" ] && echo present || echo missing): $(tr -d '\r' < "$tmp/4.txt" | grep -m1 -i 'could not\|failed')"
  fi
done

# ---------------------------------------------------------------------------
# The remaining checks use a private settings file so they never touch a real one.
S="$tmp/settings/SETUP.DAT"
mkdir -p "$tmp/settings"
port() { "$exe" --setup-dat="$(native "$S")" "$@"; }
port --reset-setup --smoke-frames=1 > "$tmp/seed.txt" 2>&1
if [ -f "$S" ] && [ "$(sha_of "$S")" = "$defaults_sha" ]; then
  pass "--setup-dat writes the requested file (defaults)"
else
  fail "--setup-dat did not create $S"
fi

echo "[5] developer keys do nothing without --debug"
# Game state per frame plus every audio event and probe line, in order.
events() { tr -d '\r' < "$1" | grep -E 'Debug state:|SFX mix:|Music:|Debug: played'; }
keys='100:t,101:t:up,110:1,111:1:up,112:2,113:2:up,115:3,116:3:up,120:tab,121:tab:up,130:h,131:h:up,'
keys+='140:o,141:o:up,150:c,151:c:up,160:b,161:b:up,170:g,171:g:up,180:space,181:space:up'
port --live-demo --debug-state --smoke-frames=200 > "$tmp/5-ctl.txt" 2>&1
port --live-demo --debug-state --smoke-frames=200 --scripted-input="$keys" > "$tmp/5-keys.txt" 2>&1
port --live-demo --debug-state --smoke-frames=200 --scripted-input="$keys" --debug > "$tmp/5-dbg.txt" 2>&1
states "$tmp/5-ctl.txt" > "$tmp/5-ctl.s"; states "$tmp/5-keys.txt" > "$tmp/5-keys.s"; states "$tmp/5-dbg.txt" > "$tmp/5-dbg.s"
events "$tmp/5-ctl.txt" > "$tmp/5-ctl.e"; events "$tmp/5-keys.txt" > "$tmp/5-keys.e"
n="$(wc -l < "$tmp/5-ctl.s" | tr -d ' ')"
d="$(first_diff_frame "$tmp/5-ctl.s" "$tmp/5-keys.s")"
if [ "$n" -ge 200 ] && [ "$d" = none ] && cmp -s "$tmp/5-ctl.e" "$tmp/5-keys.e"; then
  pass "gameplay: T 1 2 3 Tab H O C B G Space change nothing ($n identical states, same audio events)"
else
  fail "gameplay: state differs from the control run at frame $d ($n control states); audio events $(cmp -s "$tmp/5-ctl.e" "$tmp/5-keys.e" && echo same || echo differ)"
fi
d="$(first_diff_frame "$tmp/5-ctl.s" "$tmp/5-dbg.s")"
[ "$d" = 100 ] && pass "with --debug the keys still act (first change at frame 100, the T press)" \
  || fail "with --debug the first change is at frame $d (expected 100)"

fkeys='800:g,801:g:up,820:1,821:1:up,840:tab,841:tab:up,860:t,861:t:up'
port --debug-state --smoke-frames=900 > "$tmp/5f-ctl.txt" 2>&1
port --debug-state --smoke-frames=900 --scripted-input="$fkeys" > "$tmp/5f-keys.txt" 2>&1
port --debug-state --smoke-frames=900 --scripted-input="$fkeys" --debug > "$tmp/5f-dbg.txt" 2>&1
states "$tmp/5f-ctl.txt" > "$tmp/5f-ctl.s"; states "$tmp/5f-keys.txt" > "$tmp/5f-keys.s"
s="$(scene_at "$tmp/5f-ctl.txt" 800)"
d="$(first_diff_frame "$tmp/5f-ctl.s" "$tmp/5f-keys.s")"
if [ "$s" = "Main Menu" ] && [ "$d" = none ]; then
  pass "main menu: G 1 Tab T change nothing"
else
  fail "main menu: control scene at frame 800 '$s'; first difference at frame $d"
fi
s="$(scene_at "$tmp/5f-dbg.txt" 800)"
[ "$s" = "Gameplay" ] && pass "with --debug, G on the main menu still jumps to gameplay" \
  || fail "with --debug, G on the main menu left the scene at '$s'"

# ---------------------------------------------------------------------------
echo "[6] a binding on a former developer key gets its press"
S2="$tmp/settings-g/SETUP.DAT"
mkdir -p "$tmp/settings-g"
cp "$S" "$S2"
printf '\042' | dd of="$S2" bs=1 seek=24 conv=notrunc 2> /dev/null   # Rotate Left (offset 0x18) = G (0x22)
"$exe" --setup-dat="$(native "$S2")" --live-demo --debug-state --smoke-frames=130 --scripted-input='120:g,122:g:up' > "$tmp/6.txt" 2>&1
rot_g="$(states "$tmp/6.txt" | sed -n '121p' | grep -o 'piece=[0-9]*:[0-9]*' | cut -d: -f2)"
rot_c="$(sed -n '121p' "$tmp/5-ctl.s" | grep -o 'piece=[0-9]*:[0-9]*' | cut -d: -f2)"
if [ -n "$rot_g" ] && [ -n "$rot_c" ] && [ "$rot_g" != "$rot_c" ]; then
  pass "G bound to Rotate Left rotates the piece on the press frame (rotation $rot_c -> $rot_g)"
else
  fail "G bound to Rotate Left: rotation at the press frame '$rot_g', control '$rot_c'"
fi

# ---------------------------------------------------------------------------
echo "[7] the probe sound plays only with --debug"
# In play (--live-demo): frontend screens consume every key-down (menus act on releases).
port --live-demo --smoke-frames=120 --scripted-input='60:space,62:space:up' > "$tmp/7a.txt" 2>&1
port --live-demo --smoke-frames=120 --scripted-input='60:space,62:space:up' --debug > "$tmp/7b.txt" 2>&1
grep -q 'Debug: played the probe WAV' "$tmp/7a.txt" && fail "Space played the probe WAV without --debug" \
  || pass "Space in play plays nothing without --debug"
grep -q 'Debug: played the probe WAV' "$tmp/7b.txt" && pass "Space plays the probe WAV with --debug" \
  || fail "Space did not play the probe WAV with --debug"

# ---------------------------------------------------------------------------
echo "[9] a SETUP.DAT of another size is read like the original (signature check only), never wiped"
for size in 180 13; do
  F="$tmp/size-$size/SETUP.DAT"
  mkdir -p "$tmp/size-$size"
  if [ "$size" = 180 ]; then
    cp "$S" "$F"
    printf '\067' | dd of="$F" bs=1 seek=26 conv=notrunc 2> /dev/null   # music volume (0x1A) = 55
    printf '\000\000' >> "$F"                                           # two bytes the loader never reads
  else
    head -c 13 "$S" > "$F"   # signature, device and mixing rate; the original reads the rest as zero
  fi
  h0="$(sha_of "$F")"
  "$exe" --setup-dat="$(native "$F")" --smoke-frames=3 --debug-state > "$tmp/9-$size.txt" 2>&1
  s="$(first_scene "$tmp/9-$size.txt")"
  h1="$(sha_of "$F")"
  used="$(grep -c 'SETUP.DAT: .*(from --setup-dat)' "$tmp/9-$size.txt")"
  if [ "$used" = 1 ] && [ "$s" = "DDD Splash" ] && [ "$h0" = "$h1" ]; then
    pass "$size-byte file accepted (boots to the DDD splash) and left byte-identical"
  else
    fail "$size-byte file: used=$used booted to '$s'; file $([ "$h0" = "$h1" ] && echo unchanged || echo "rewritten to $(size_of "$F") bytes")"
  fi
done

# ---------------------------------------------------------------------------
if [ "$isolation" = "--suite-isolation" ]; then
  echo "[8] the shipped suites never write or read the player's SETUP.DAT"
  player_file="$exe_dir/SETUP.DAT"
  if [ -e "$player_file" ]; then player_existed=1; cp "$player_file" "$tmp/player-backup"; fi
  cp "$S" "$player_file"
  printf '\062' | dd of="$player_file" bs=1 seek=30 conv=notrunc 2> /dev/null   # SFX volume (0x1E) = 50
  h0="$(sha_of "$player_file")"
  bash "$here/run-smokes.sh" "$exe" > "$tmp/8-smokes.txt" 2>&1
  r1=$?
  bash "$here/run-dynamic-tests.sh" "$exe" > "$tmp/8-dynamic.txt" 2>&1
  r2=$?
  h1="$(sha_of "$player_file")"
  [ "$h0" = "$h1" ] && pass "player's SETUP.DAT unchanged by both suites" || fail "a suite rewrote the player's SETUP.DAT"
  # A suite that read the player's file without first resetting it would play SFX at
  # 50 % and fail run-smokes [4]; a reset is a write, which the hash check above catches.
  [ "$r1" = 0 ] && pass "run-smokes.sh passed against a non-default player file" \
    || fail "run-smokes.sh failed (exit $r1): $(grep -m3 'FAIL' "$tmp/8-smokes.txt" | tr '\n' ' ')"
  [ "$r2" = 0 ] && pass "run-dynamic-tests.sh passed against a non-default player file" \
    || fail "run-dynamic-tests.sh failed (exit $r2): $(grep -m3 'FAIL' "$tmp/8-dynamic.txt" | tr '\n' ' ')"
else
  note "[8] suite isolation not run (pass --suite-isolation)"
fi

echo
if [ "$fails" -eq 0 ]; then echo "ALL ENGINEERING CHECKS PASSED"; else echo "ENGINEERING CHECK FAILURES: $fails"; fi
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
