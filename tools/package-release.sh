#!/usr/bin/env bash
# Package a finished, tested build into a release archive and check what went in.
# Used by .github/workflows/release.yml; runs locally too (Git Bash or Linux).
#
# Usage: tools/package-release.sh windows|linux <version> <build-dir> [<saba.zip>]
#   Writes saba-reborn-<version>-<platform>-x64.zip (Windows) or .tar.gz (Linux) into
#   the current directory. The optional fourth argument bundles SABA's intact archive;
#   give it only when permission to redistribute it is on record (see NOTICE). Without
#   it the download ships no game data and players bring their own.
set -eu

platform="${1:?platform}"
version="${2:?version}"
build="${3:?build dir}"
saba="${4:-}"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
name="saba-reborn-${version}-${platform}-x64"

# Dependency sources: FetchContent puts them in <build>/_deps/<name>-src unless the
# configure step pointed FETCHCONTENT_SOURCE_DIR_<NAME> elsewhere.
dep_src() {
  local dir
  dir="$(sed -n "s/^FETCHCONTENT_SOURCE_DIR_$1:PATH=//p" "$build/CMakeCache.txt" 2> /dev/null | head -1)"
  if [ -n "$dir" ]; then echo "$dir"; else echo "$build/_deps/$2-src"; fi
}
mikmod_src="$(dep_src LIBMIKMOD libmikmod)"
sdl_src="$(dep_src SDL3 sdl3)"

rm -rf "$name"
mkdir -p "$name/LICENSES"
case "$platform" in
  windows)
    cp "$build/Release/saba-reborn.exe" "$build/Release/SDL3.dll" "$name/" ;;
  linux)
    cp "$build/saba-reborn" "$name/"
    cp -P "$build"/_deps/sdl3-build/libSDL3.so* "$name/"
    chmod +x "$name/saba-reborn" ;;
  *)
    echo "unknown platform: $platform" >&2; exit 2 ;;
esac
cp "$root/README.md" "$root/NOTICE" "$root/LICENSE" "$name/"
# Third-party licence texts that must travel with the binaries: libmikmod is linked
# statically (GNU LGPL v2 or later), miniz is compiled in (MIT), SDL3 sits beside the
# executable (zlib; required for source copies, shipped here as a courtesy).
cp "$mikmod_src/libmikmod/COPYING.LIB" "$name/LICENSES/libmikmod-COPYING.LIB"
cp "$root/third_party/miniz/LICENSE" "$name/LICENSES/miniz-LICENSE"
cp "$sdl_src/LICENSE.txt" "$name/LICENSES/SDL3-LICENSE.txt"
if [ -n "$saba" ]; then
  cp "$saba" "$name/saba.zip"
fi

case "$platform" in
  windows)
    archive="$name.zip"
    rm -f "$archive"
    ps="$(command -v pwsh || command -v powershell)"
    "$ps" -NoProfile -Command "Compress-Archive -Path '$name' -DestinationPath '$archive'"
    py="$(command -v python || command -v python3)"
    listing="$("$py" -c 'import sys, zipfile; print("\n".join(zipfile.ZipFile(sys.argv[1]).namelist()))' "$archive" | tr '\\' '/')" ;;
  linux)
    archive="$name.tar.gz"
    tar czf "$archive" "$name"
    listing="$(tar tzf "$archive")" ;;
esac

# Check the archive itself, not the staging folder.
missing=0
for f in README.md NOTICE LICENSE LICENSES/libmikmod-COPYING.LIB LICENSES/miniz-LICENSE LICENSES/SDL3-LICENSE.txt; do
  echo "$listing" | grep -q -x -F "$name/$f" || { echo "package check: $f missing from $archive" >&2; missing=1; }
done
if [ -n "$saba" ]; then
  echo "$listing" | grep -q -x -F "$name/saba.zip" || { echo "package check: saba.zip missing" >&2; missing=1; }
elif echo "$listing" | grep -q -i -E '\.(zip|dat)$'; then
  echo "package check: game data in $archive although bundling was not requested" >&2
  missing=1
fi
[ "$missing" = 0 ] || exit 1
sha256sum "$archive"
echo "packaged $archive ($(echo "$listing" | grep -c -v '/$') files)"
