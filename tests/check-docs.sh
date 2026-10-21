#!/usr/bin/env bash
# Documentation accuracy gate (run by the release workflow before packaging).
# Fails on claims that an audit against the original executable showed to be
# false, and checks that the README controls table names the default bindings
# the port actually ships (src/milestone_a_demo.cpp kDefaultKeyboardBindings).
# Usage: bash tests/check-docs.sh
set -u

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
docs=("$root/README.md" "$root/NOTICE" "$root/LICENSE" "$root/.github/workflows/release.yml")
fails=0
pass() { echo "  PASS: $1"; }
fail() { echo "  FAIL: $1"; fails=$((fails + 1)); }

echo "[1] retired claims are gone"
# 70.086           the original's game logic runs on a 60.0011 Hz timer tick; the
#                  70 Hz retrace measurement it was derived from is dead code.
# bit-identical    the generator matches, but every New Game starts from one fixed
#                  seed where the original seeds from its tick counter.
# faithful         not true yet: the audit lists open timing, input, gameplay,
#                  visual and audio differences.
# Arrow keys move/rotate   rotation defaults to A and S; Up does nothing in play.
# already bundles SABA     no release has been published, and bundling is opt-in.
# and release binaries     the 0BSD grant covers the port's own source; the
#                  binaries also contain statically linked LGPL libmikmod.
for phrase in '70.086' 'bit-identical' 'faithful' 'Arrow keys move/rotate' 'already bundles SABA' \
  'and release binaries'; do
  hits="$(grep -n -i -F -- "$phrase" "${docs[@]}" 2> /dev/null)"
  [ -z "$hits" ] && pass "no '$phrase'" || fail "'$phrase' found: $(echo "$hits" | head -2 | tr '\n' ' ')"
done

echo "[2] README controls table matches the shipped default bindings"
keyname() {
  case "$1" in
    D0) echo "Down arrow" ;; CB) echo "Left arrow" ;; CD) echo "Right arrow" ;; C8) echo "Up arrow" ;;
    10) echo Q ;; 11) echo W ;; 12) echo E ;; 13) echo R ;; 14) echo T ;; 15) echo Y ;; 16) echo U ;;
    17) echo I ;; 18) echo O ;; 19) echo P ;; 1E) echo A ;; 1F) echo S ;; 20) echo D ;; 21) echo F ;;
    22) echo G ;; 23) echo H ;; 24) echo J ;; 25) echo K ;; 26) echo L ;; 2C) echo Z ;; 2D) echo X ;;
    2E) echo C ;; 2F) echo V ;; 30) echo B ;; 31) echo N ;; 32) echo M ;; 39) echo Space ;;
    *) echo "scancode 0x$1" ;;
  esac
}
mapfile -t codes < <(sed -n '/kDefaultKeyboardBindings = {/,/};/p' "$root/src/milestone_a_demo.cpp" \
  | grep -o '0x[0-9A-Fa-f][0-9A-Fa-f]' | sed 's/0x//' | tr 'a-f' 'A-F')
labels=("Soft drop (Down)" "Move left (Left)" "Move right (Right)" "Rotate clockwise (Rotate Left)" \
  "Rotate counter-clockwise (Rotate Right)")
if [ "${#codes[@]}" != 5 ]; then
  fail "could not read the five default bindings from src/milestone_a_demo.cpp (got ${#codes[@]})"
else
  for i in 0 1 2 3 4; do
    row="| ${labels[$i]} | $(keyname "${codes[$i]}") |"
    grep -q -F -x -- "$row" "$root/README.md" && pass "README: $row" || fail "README lacks the row: $row"
  done
fi

echo "[3] release downloads carry the third-party licence texts"
for f in LICENSES/libmikmod-COPYING.LIB LICENSES/miniz-LICENSE LICENSES/SDL3-LICENSE.txt; do
  grep -q -F -- "$f" "$root/tools/package-release.sh" && pass "packaging copies $f" || fail "packaging does not copy $f"
done

echo
if [ "$fails" -eq 0 ]; then echo "ALL DOC CHECKS PASSED"; else echo "DOC CHECK FAILURES: $fails"; fi
exit $([ "$fails" -eq 0 ] && echo 0 || echo 1)
