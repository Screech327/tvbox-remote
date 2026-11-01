#!/usr/bin/env bash
# Runs inside the camstream container. Installs ffmpeg + MediaMTX and starts the stream.
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
echo "== Installing ffmpeg"
export LC_ALL=C.UTF-8 LANG=C.UTF-8
apt-get update -qq
apt-get install -y -qq ffmpeg v4l-utils curl ca-certificates >/dev/null

echo "== Installing MediaMTX"
VER=$(curl -fsSL https://api.github.com/repos/bluenviron/mediamtx/releases/latest | grep '"tag_name"' | cut -d'"' -f4)
[ -n "$VER" ] || { echo "Could not look up the MediaMTX version"; exit 1; }
curl -fsSL -o /tmp/mtx.tgz "https://github.com/bluenviron/mediamtx/releases/download/${VER}/mediamtx_${VER}_linux_amd64.tar.gz"
tar xzf /tmp/mtx.tgz -C /usr/local/bin mediamtx
echo "   MediaMTX $VER"

echo "== Writing settings"
cp "$SRC/mediamtx.yml" /etc/mediamtx.yml
install -m 755 "$SRC/camstream-run" /usr/local/bin/camstream-run
VIDEO="${VIDEO:-/dev/video2}"
FMT=yuyv422
v4l2-ctl -d "$VIDEO" --list-formats 2>/dev/null | grep -q NV12 && FMT=nv12
if [ ! -f /etc/camstream.env ]; then
  cat > /etc/camstream.env <<ENV
# camstream settings. After editing: systemctl restart camstream
VIDEO=$VIDEO
AUDIO=${AUDIO:-hw:2,0}   # set to none for video only
INPUT_FORMAT=$FMT
SIZE=1920x1080
FPS=60                   # 30 halves the CPU use
BITRATE=8M               # lower (4M) if it stutters away from home
PRESET=veryfast          # superfast/ultrafast = less CPU, softer picture
ENV
fi
cp "$SRC/mediamtx.service" "$SRC/camstream.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now mediamtx >/dev/null 2>&1
systemctl enable camstream >/dev/null 2>&1
systemctl restart camstream
sleep 4
systemctl --no-pager --lines=5 status camstream || true
