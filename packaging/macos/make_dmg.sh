#!/usr/bin/env bash
# Builds dist/TetherDesk-<version>-macOS.dmg: a self-contained TetherDesk.app
# (viewer + bundled host + web viewer, SDL linked statically), ad-hoc signed.
set -euo pipefail
cd "$(dirname "$0")/../.."
version="${1:-$(sed -n 's/^project(TetherDesk VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)}"
jobs="$(sysctl -n hw.ncpu)"

# Downloaded dependencies live on the system disk: unpacking archives onto
# exFAT/FAT volumes adds "._*" files that confuse CMake's FetchContent.
deps="${TMPDIR:-/tmp}/tetherdesk-deps"
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTD_STATIC_SDL=ON -DFETCHCONTENT_BASE_DIR="$deps"
cmake --build build-release -j "$jobs"
app=build-release/TetherDesk.app

if command -v emcmake >/dev/null; then
  emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
  cmake --build build-web -j "$jobs"
  mkdir -p "$app/Contents/Resources/web"
  cp build-web/index.html build-web/index.js build-web/index.wasm "$app/Contents/Resources/web/"
else
  echo "warning: Emscripten not found - the app won't include the browser viewer" >&2
fi

# Refuse to ship something that still needs Homebrew at runtime.
if otool -L "$app/Contents/MacOS/TetherDesk" | grep -q /opt/homebrew; then
  echo "error: TetherDesk links a Homebrew library; install SDL2 with static libs" >&2
  exit 1
fi

find "$app" -name '._*' -delete
codesign --force --deep --sign - "$app"

stage="$(mktemp -d)"
ditto "$app" "$stage/TetherDesk.app"
ln -s /Applications "$stage/Applications"
mkdir -p dist
out="dist/TetherDesk-$version-macOS.dmg"
rm -f "$out"
hdiutil create -volname "TetherDesk" -srcfolder "$stage" -ov -format UDZO "$out" >/dev/null
rm -rf "$stage"
echo "built $out"
