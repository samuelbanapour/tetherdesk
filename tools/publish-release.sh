#!/usr/bin/env bash
# Publishes the latest successful Azure Pipelines build to the single GitHub
# release "v1.0.0": moves the tag to the built commit and replaces the
# download files in place (stable names, so website links never change).
#
#   tools/publish-release.sh            latest successful run
#   tools/publish-release.sh 1234       a specific run id
#
# Needs: az (logged in to dev.azure.com/SamuelBanapour) and gh (logged in).
set -euo pipefail
cd "$(dirname "$0")/.."
org=https://dev.azure.com/SamuelBanapour
project=sky-mood
pipeline=tetherdesk-release
repo=samuelbanapour/tetherdesk
tag=v1.0.0

run="${1:-$(az pipelines runs list --org "$org" --project "$project" --pipeline-ids \
  "$(az pipelines show --org "$org" --project "$project" --name "$pipeline" --query id -o tsv)" \
  --status completed --result succeeded --top 1 --query '[0].id' -o tsv)}"
[ -n "$run" ] || { echo "no successful $pipeline run found" >&2; exit 1; }
commit="$(az pipelines runs show --org "$org" --project "$project" --id "$run" --query sourceVersion -o tsv)"
echo "publishing Azure run $run (commit ${commit:0:7}) to $repo $tag"

dl="$(mktemp -d)"
trap 'rm -rf "$dl"' EXIT
for a in linux windows macos; do
  az pipelines runs artifact download --org "$org" --project "$project" --run-id "$run" \
    --artifact-name "$a" --path "$dl/$a" >/dev/null
done
files=("$dl"/linux/*.zip "$dl"/windows/*.exe "$dl"/macos/*)
ls -la "${files[@]}"

gh api -X PATCH "repos/$repo/git/refs/tags/$tag" -f sha="$commit" -F force=true >/dev/null 2>&1 \
  || gh api -X POST "repos/$repo/git/refs" -f ref="refs/tags/$tag" -f sha="$commit" >/dev/null
gh release view "$tag" --repo "$repo" >/dev/null 2>&1 \
  || gh release create "$tag" --repo "$repo" --title "TetherDesk" --latest --notes-file .github/release-notes.md
gh release upload "$tag" --repo "$repo" --clobber "${files[@]}"
echo "done: https://github.com/$repo/releases/tag/$tag"
