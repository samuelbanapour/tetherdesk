#!/usr/bin/env bash
# Deploys the relay to Fly.io together with the web viewer and the latest
# QuickSupport downloads from the GitHub release.
#   relay/deploy.sh            (needs: flyctl logged in, gh logged in, Emscripten)
set -euo pipefail
cd "$(dirname "$0")/.."
pub=relay/public
mkdir -p "$pub/download"

./build.sh web
command cp -f build/web/index.html build/web/index.js build/web/index.wasm "$pub/"
command cp -f relay/get.html "$pub/get.html"

tag="$(gh release list --limit 1 --json tagName --jq '.[0].tagName')"
rm -f "$pub"/download/*
gh release download "$tag" --dir "$pub/download" --pattern 'TetherDesk-QuickSupport*'
echo "deploying relay with downloads from $tag:"; ls -la "$pub/download"
find . -name '._*' -delete 2>/dev/null || true

fly deploy --config relay/fly.toml --dockerfile relay/Dockerfile --ha=false .
