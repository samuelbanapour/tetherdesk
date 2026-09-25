#!/usr/bin/env bash
# Installs or updates the TetherDesk relay on an Ubuntu server that already
# runs Caddy (which provides HTTPS). Uses the relay binary + web viewer from
# the latest GitHub release, so the server needs no compilers.
#
#   relay/deploy-ec2.sh ubuntu@54.151.75.113 [domain]
#   SSH_KEY=~/.ssh/other.pem relay/deploy-ec2.sh ...
#
# Idempotent: re-run to upgrade. The relay listens on 127.0.0.1:4020 only,
# runs as its own unprivileged user with memory/CPU caps, and Caddy proxies
# https://<domain>/ to it (WebSockets included).
set -euo pipefail
target="${1:?usage: $0 user@host [domain]}"
domain="${2:-tetherdesk.54-151-75-113.nip.io}"
key="${SSH_KEY:-$HOME/.ssh/login.pem}"
repo="samuelbanapour/tetherdesk"
tag="$(gh release list --repo "$repo" --limit 1 --json tagName --jq '.[0].tagName')"
url="https://github.com/$repo/releases/download/$tag/TetherDesk-${tag#v}-linux-x64.zip"
echo "deploying relay from $tag to $target ($domain)"

ssh -i "$key" -o BatchMode=yes "$target" "sudo bash -s -- '$url' '$domain'" <<'REMOTE'
set -euo pipefail
url="$1"; domain="$2"; port=4020; dir=/opt/tetherdesk-relay

id -u tetherdesk >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin tetherdesk

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
curl -fsSL "$url" -o "$tmp/td.zip"
python3 -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$tmp/td.zip" "$tmp"
src="$(find "$tmp" -maxdepth 2 -type d -name relay | head -1)"
install -d -m 0755 "$dir/public"
install -m 0755 "$src/tetherdesk-relay" "$dir/tetherdesk-relay.new"
mv -f "$dir/tetherdesk-relay.new" "$dir/tetherdesk-relay"
install -m 0644 "$src"/public/* "$dir/public/"

cat > /etc/systemd/system/tetherdesk-relay.service <<UNIT
[Unit]
Description=TetherDesk relay
After=network-online.target
Wants=network-online.target

[Service]
User=tetherdesk
ExecStart=$dir/tetherdesk-relay --bind 127.0.0.1 --port $port --web-root $dir/public
Restart=always
RestartSec=2
# Share the machine politely with the other apps on it.
MemoryMax=150M
CPUQuota=50%
LimitNOFILE=8192
# Hardening: no privileges, read-only system, no access to home dirs.
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
PrivateDevices=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
RestrictAddressFamilies=AF_INET AF_INET6
LockPersonality=true

[Install]
WantedBy=multi-user.target
UNIT
systemctl daemon-reload
systemctl enable tetherdesk-relay >/dev/null 2>&1
systemctl restart tetherdesk-relay

caddyfile=/etc/caddy/Caddyfile
if ! grep -q "^$domain {" "$caddyfile"; then
    backup="$caddyfile.bak-tetherdesk-$(date +%s)"
    cp "$caddyfile" "$backup"
    printf '\n%s {\n\treverse_proxy localhost:%s\n}\n' "$domain" "$port" >> "$caddyfile"
    if ! caddy validate --config "$caddyfile" --adapter caddyfile >/dev/null 2>&1; then
        cp "$backup" "$caddyfile"
        echo "Caddyfile validation failed - restored $backup" >&2
        exit 1
    fi
    systemctl reload caddy
    echo "added $domain to Caddy (backup: $backup)"
fi

sleep 1
curl -fsS "http://127.0.0.1:$port/healthz"
systemctl --no-pager --lines=0 status tetherdesk-relay | head -3
REMOTE
echo "relay: https://$domain/   (download page: https://$domain/get)"
