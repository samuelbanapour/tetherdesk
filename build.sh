#!/usr/bin/env bash
# Builds TetherDesk.
#   ./build.sh          native host + native viewer + web viewer (if emcc exists)
#   ./build.sh native   native only
#   ./build.sh web      web viewer only (needs Emscripten)
#   ./build.sh test     native build + run tests
# Output: build/tetherdesk-host, build/tetherdesk, build/web/ (served by the host)
set -euo pipefail
cd "$(dirname "$0")"
what="${1:-all}"
jobs="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

native() {
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j "$jobs"
}

web() {
  if ! command -v emcmake >/dev/null; then
    echo "emcmake not found - install Emscripten (e.g. 'brew install emscripten') to build the web viewer" >&2
    return 1
  fi
  emcmake cmake -S . -B build-web -DCMAKE_BUILD_TYPE=Release
  cmake --build build-web -j "$jobs"
  mkdir -p build/web
  cp build-web/index.html build-web/index.js build-web/index.wasm build/web/
  echo "web viewer staged in build/web/"
}

case "$what" in
  native) native ;;
  web) web ;;
  test) native && (cd build && ctest --output-on-failure) ;;
  all) native; web || echo "(skipped web viewer)" ;;
  *) echo "usage: $0 [all|native|web|test]" >&2; exit 2 ;;
esac
