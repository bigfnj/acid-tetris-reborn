#!/usr/bin/env bash
# Audio parity checks (complements run-smokes.sh and run-dynamic-tests.sh): music routed through
# SDL on every platform, effect voice classes, lock-sound pan, music ramps and Music-row changes,
# each asserted against the reverse-engineered original. Needs Python 3 (standard library only).
# Usage: run-audio-tests.sh [path-to-exe] [--only name,...]
set -u

here="$(cd "$(dirname "$0")" && pwd)"
exe="${1:-}"
if [ -z "$exe" ] || [ "${exe#--}" != "$exe" ]; then
  for cand in \
    "$here/build/Release/saba-reborn.exe" "$here/build/saba-reborn" \
    "$here/build/Release/acid_tetris_port.exe" "$here/build/acid_tetris_port"; do
    [ -x "$cand" ] && exe="$cand" && break
  done
else
  shift
fi
[ -x "$exe" ] || { echo "FATAL: exe not found (pass path as arg)"; exit 2; }

py=""
for cand in python3 python; do
  if "$cand" -c 'import sys; sys.exit(0 if sys.version_info[0] == 3 else 1)' >/dev/null 2>&1; then
    py="$cand"
    break
  fi
done
[ -n "$py" ] || { echo "FATAL: Python 3 not found"; exit 2; }

echo "== audio checks: $exe =="
exec "$py" "$here/tests/audio_checks.py" "$exe" "$@"
