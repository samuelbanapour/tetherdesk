#!/usr/bin/env bash
# Signs a .app with TetherDesk's code-signing certificate.
#
#   packaging/macos/sign.sh path/to/TetherDesk.app
#
# Signs with a certificate only when $TD_SIGN_P12 (a .p12) and
# $TD_SIGN_PASSWORD are set - meant for an Apple "Developer ID Application"
# certificate. Otherwise the app is ad-hoc signed.
#
# Why not a free self-signed certificate: macOS keeps Screen Recording /
# Accessibility approval per signing identity, which would make approval
# survive updates - but current macOS (verified on 27.0) refuses to match
# permissions to an identity whose certificate isn't trusted
# ("Failed to match existing code requirement"), so a self-signed build can
# never be granted access at all. Ad-hoc builds work; each update just needs
# approving once. A Developer ID certificate fixes both this and Gatekeeper.
#
# A throwaway keychain is used so the login keychain is never touched.
set -euo pipefail
app="${1:?usage: $0 path/to/App.app}"
p12="${TD_SIGN_P12:-}"
pass="${TD_SIGN_PASSWORD:-}"
cn="${TD_SIGN_NAME:-Developer ID Application}"

if [ -z "$p12" ] || [ ! -f "$p12" ] || [ -z "$pass" ]; then
  echo "sign.sh: ad-hoc signing (set TD_SIGN_P12/TD_SIGN_PASSWORD for a Developer ID certificate)" >&2
  codesign --force --deep --sign - "$app"
  exit 0
fi

kc="$(mktemp -d)/tetherdesk-signing.keychain-db"
kcpass="$(openssl rand -hex 16)"
cleanup() { security delete-keychain "$kc" >/dev/null 2>&1 || true; }
trap cleanup EXIT

security create-keychain -p "$kcpass" "$kc"
security set-keychain-settings -lut 900 "$kc"
security unlock-keychain -p "$kcpass" "$kc"
security import "$p12" -k "$kc" -P "$pass" -T /usr/bin/codesign >/dev/null
security set-key-partition-list -S apple-tool:,apple:,codesign: -s -k "$kcpass" "$kc" >/dev/null
# codesign looks identities up through the keychain search list.
orig="$(security list-keychains -d user | tr -d '"' | xargs)"
security list-keychains -d user -s "$kc" $orig
trap 'security list-keychains -d user -s $orig; cleanup' EXIT

identity="$(security find-certificate -c "$cn" -Z "$kc" | awk '/SHA-1 hash/{print $3; exit}')"
codesign --force --deep --options runtime --timestamp --keychain "$kc" --sign "$identity" "$app"
codesign --verify --deep --strict "$app"
echo "signed $app with $cn ($identity)"
