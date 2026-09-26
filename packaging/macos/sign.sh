#!/usr/bin/env bash
# Signs a .app with TetherDesk's code-signing certificate.
#
#   packaging/macos/sign.sh path/to/TetherDesk.app
#
# The certificate is read from $TD_SIGN_P12 (a .p12 file) and
# $TD_SIGN_PASSWORD, or by default from
# ~/Library/Application Support/TetherDesk-signing/. Without a certificate the
# app is ad-hoc signed instead.
#
# Why this matters: macOS remembers Screen Recording / Accessibility approval
# per *signing identity*. With a stable certificate, updates keep those
# permissions; ad-hoc signatures change on every build, so every update would
# have to be approved again.
#
# A throwaway keychain is used so the login keychain is never touched.
set -euo pipefail
app="${1:?usage: $0 path/to/App.app}"
dir="$HOME/Library/Application Support/TetherDesk-signing"
p12="${TD_SIGN_P12:-$dir/tetherdesk-signing.p12}"
pass="${TD_SIGN_PASSWORD:-$(cat "$dir/p12.password" 2>/dev/null || true)}"

if [ ! -f "$p12" ] || [ -z "$pass" ]; then
  echo "sign.sh: no certificate found - ad-hoc signing (permissions won't survive updates)" >&2
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

identity="$(security find-certificate -c "TetherDesk Code Signing" -Z "$kc" | awk '/SHA-1 hash/{print $3; exit}')"
codesign --force --deep --timestamp=none --keychain "$kc" --sign "$identity" "$app"
codesign --verify --deep --strict "$app"
echo "signed $app with TetherDesk Code Signing ($identity)"
