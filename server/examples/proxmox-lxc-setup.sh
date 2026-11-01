#!/usr/bin/env bash
# OPTIONAL, for Proxmox users. Run on the Proxmox host as root. Creates an LXC container, passes the capture card
# through and runs server/stream/install.sh inside it.
# Usage: IP=192.168.1.50 GW=192.168.1.1 bash server/examples/proxmox-lxc-setup.sh
set -euo pipefail
CTID="${CTID:-120}"
IP="${IP:?set IP=<free LAN address for the container, e.g. 192.168.1.50>}"
GW="${GW:?set GW=<your router, e.g. 192.168.1.1>}"
HERE="$(cd "$(dirname "$0")/../stream" && pwd)"
die() { echo "ERROR: $*" >&2; exit 1; }

echo "== Finding the Cam Link"
LINK=$(ls /dev/v4l/by-id/*video-index0 2>/dev/null | head -1) || true
[ -n "$LINK" ] || die "Capture card video device not found. Check the USB cable (lsusb)."
VIDEO=$(readlink -f "$LINK")
CARD=$(awk '/USB-Audio|Cam Link/ {print $1; exit}' /proc/asound/cards)
[ -n "$CARD" ] || die "Capture card audio device not found in /proc/asound/cards."
echo "   video: $VIDEO   audio: card $CARD"

if ! pct status "$CTID" >/dev/null 2>&1; then
  echo "== Getting the Debian 13 template"
  pveam update >/dev/null
  T=$(pveam available --section system | awk '/debian-13-standard.*amd64/ {print $2}' | tail -1)
  [ -n "$T" ] || die "No Debian 13 amd64 template found."
  [ -f "/var/lib/vz/template/cache/$T" ] || pveam download local "$T"
  echo "== Creating container $CTID at $IP"
  pct create "$CTID" "local:vztmpl/$T" --hostname camstream --cores 6 --memory 1024 --swap 256 \
    --rootfs local-lvm:8 --net0 "name=eth0,bridge=vmbr0,ip=$IP/24,gw=$GW" --nameserver "$GW" \
    --unprivileged 1 --features nesting=1 --onboot 1
else
  echo "== Container $CTID already exists, updating it"
  pct stop "$CTID" >/dev/null 2>&1 || true
fi

echo "== Passing the Cam Link through"
pct set "$CTID" -dev0 "$VIDEO,mode=0666" -dev1 "/dev/snd/pcmC${CARD}D0c,mode=0666" \
  -dev2 "/dev/snd/controlC${CARD},mode=0666" -dev3 "/dev/snd/timer,mode=0666"
pct start "$CTID"
sleep 6

echo "== Installing the stream inside the container"
tar czf /tmp/camstream.tar.gz -C "$(dirname "$HERE")" stream
pct push "$CTID" /tmp/camstream.tar.gz /root/camstream.tar.gz
pct exec "$CTID" -- bash -c "cd /root && rm -rf stream && tar xzf camstream.tar.gz && VIDEO=$VIDEO AUDIO=hw:$CARD,0 bash stream/install.sh"
echo
echo "Done. Watch it at:  http://$IP:8889/vseebox"
