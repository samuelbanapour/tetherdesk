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
# Universal binary: runs natively on Apple Silicon and Intel Macs.
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DTD_STATIC_SDL=ON -DFETCHCONTENT_BASE_DIR="$deps" \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build-release -j "$jobs"
app=build-release/TetherDesk.app

if [ -n "${TD_WEB_DIR:-}" ]; then  # prebuilt web viewer (CI builds it once, elsewhere)
  mkdir -p "$app/Contents/Resources/web"
  cp "$TD_WEB_DIR"/index.html "$TD_WEB_DIR"/index.js "$TD_WEB_DIR"/index.wasm "$app/Contents/Resources/web/"
elif command -v emcmake >/dev/null; then
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
# Stable signing identity so macOS keeps Screen Recording / Accessibility
# approval across updates (falls back to ad-hoc without the certificate).
bash "$(dirname "$0")/sign.sh" "$app"

stage="$(mktemp -d)"
ditto "$app" "$stage/TetherDesk.app"
ln -s /Applications "$stage/Applications"
mkdir -p dist
out="dist/TetherDesk-$version-macOS.dmg"
rm -f "$out"
hdiutil create -volname "TetherDesk" -srcfolder "$stage" -ov -format UDZO "$out" >/dev/null
rm -rf "$stage"
echo "built $out"

# QuickSupport: the dedicated share-only app, zipped (nothing to install).
qs_app=build-release/TetherDeskQuickSupport.app
find "$qs_app" -name '._*' -delete
bash "$(dirname "$0")/sign.sh" "$qs_app"
qs_stage="$(mktemp -d)"
ditto "$qs_app" "$qs_stage/TetherDesk QuickSupport.app"
qs="dist/TetherDesk-QuickSupport-macOS.zip"
rm -f "$qs"
(cd "$qs_stage" && ditto -c -k --keepParent "TetherDesk QuickSupport.app" "$OLDPWD/$qs")
rm -rf "$qs_stage"
echo "built $qs"

# TetherDesk Remote: the app for reaching your own computers (no support
# features). A .dmg like TetherDesk: it belongs in Applications, since
# "Always on" starts it from there.
rm_app=build-release/TetherDeskRemote.app
if [ -d "$app/Contents/Resources/web" ]; then
  mkdir -p "$rm_app/Contents/Resources/web"
  cp "$app"/Contents/Resources/web/index.* "$rm_app/Contents/Resources/web/"
fi
find "$rm_app" -name '._*' -delete
bash "$(dirname "$0")/sign.sh" "$rm_app"
rm_stage="$(mktemp -d)"
ditto "$rm_app" "$rm_stage/TetherDesk Remote.app"
ln -s /Applications "$rm_stage/Applications"
rm_out="dist/TetherDesk-Remote-macOS.dmg"
rm -f "$rm_out"
hdiutil create -volname "TetherDesk Remote" -srcfolder "$rm_stage" -ov -format UDZO "$rm_out" >/dev/null
rm -rf "$rm_stage"
echo "built $rm_out"
