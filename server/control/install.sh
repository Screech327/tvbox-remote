#!/usr/bin/env bash
# Runs inside the camstream container (CT 120). Installs the control page on port 80.
# Usage: install.sh <nano-ip> <token>     (both optional if /etc/vseebox-control.env already exists)
set -euo pipefail
export LC_ALL=C.UTF-8 LANG=C.UTF-8
SRC="$(cd "$(dirname "$0")" && pwd)"
ENV=/etc/vseebox-control.env

command -v python3 >/dev/null || { echo "== Installing Python"; apt-get update -qq; apt-get install -y -qq python3-minimal >/dev/null; }
id vsbctl >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin vsbctl

install -d /opt/vseebox-control
install -m 644 "$SRC/server.py" "$SRC/index.html" /opt/vseebox-control/

if [ -n "${1:-}" ] || [ ! -f "$ENV" ]; then
  cat > "$ENV" <<ENVFILE
# vSeeBox control page settings. After editing: systemctl restart vseebox-control
ESP_HOST=${1:-}
ESP_TOKEN=${2:-}
ESP_PORT=4210
PORT=80
WHEP_URL=http://127.0.0.1:8889/vseebox/whep
ENVFILE
fi
chown root:vsbctl "$ENV"; chmod 640 "$ENV"

install -m 644 "$SRC/vseebox-control.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable vseebox-control >/dev/null 2>&1
systemctl restart vseebox-control
sleep 2
systemctl is-active --quiet vseebox-control && echo "== Running: http://$(hostname -I | awk '{print $1}')/" \
  || { systemctl --no-pager --lines=15 status vseebox-control; exit 1; }
