#!/usr/bin/env bash
# Installs the capture + stream side on a Debian/Ubuntu/Raspberry Pi OS machine (bare metal, VM or LXC).
# Run as root:  sudo bash server/stream/install.sh
#
# What it does:
#   - installs ffmpeg + v4l-utils + alsa-utils
#   - downloads MediaMTX (latest, or MTX_VERSION=vX.Y.Z; right CPU architecture) to /usr/local/bin
#   - finds the USB capture card's video and audio devices (override with VIDEO=/dev/videoN AUDIO=hw:N,0)
#   - writes /etc/camstream.env (only if it doesn't exist yet), installs and starts two services:
#       mediamtx   (WebRTC on :8889, HLS on :8888, RTSP on :8554)
#       camstream  (ffmpeg: capture card -> H.264/Opus -> MediaMTX path "vseebox")
set -euo pipefail
SRC="$(cd "$(dirname "$0")" && pwd)"
MTX_VERSION="${MTX_VERSION:-latest}"      # or pin one, e.g. MTX_VERSION=v1.13.1
[ "$(id -u)" = 0 ] || { echo "Run as root (sudo)."; exit 1; }
export LC_ALL=C.UTF-8 LANG=C.UTF-8

echo "== Installing ffmpeg, v4l-utils, alsa-utils, curl"
apt-get update -qq
apt-get install -y -qq ffmpeg v4l-utils alsa-utils curl ca-certificates >/dev/null

if [ "$MTX_VERSION" = latest ]; then
  MTX_VERSION=$(curl -fsSLI -o /dev/null -w '%{url_effective}' https://github.com/bluenviron/mediamtx/releases/latest | sed 's#.*/tag/##')
fi
[ -n "$MTX_VERSION" ] || { echo "Could not look up the MediaMTX version; set MTX_VERSION=vX.Y.Z"; exit 1; }
echo "== Installing MediaMTX $MTX_VERSION"
case "$(uname -m)" in
  x86_64)  ARCH=amd64 ;;
  aarch64) ARCH=arm64 ;;
  armv7l)  ARCH=armv7 ;;
  *) echo "Unsupported CPU: $(uname -m)"; exit 1 ;;
esac
curl -fsSL -o /tmp/mtx.tgz \
  "https://github.com/bluenviron/mediamtx/releases/download/${MTX_VERSION}/mediamtx_${MTX_VERSION}_linux_${ARCH}.tar.gz"
tar xzf /tmp/mtx.tgz -C /usr/local/bin mediamtx
/usr/local/bin/mediamtx --version || true

echo "== Finding the capture card"
if [ -z "${VIDEO:-}" ]; then
  # First USB capture device that exposes a video-index0 node (Cam Link, generic USB3/USB2 capture sticks)
  LINK=$(ls /dev/v4l/by-id/*video-index0 2>/dev/null | head -1 || true)
  [ -n "$LINK" ] && VIDEO=$(readlink -f "$LINK")
fi
[ -n "${VIDEO:-}" ] || { echo "No capture card found. Plug it in (USB 3 port if it has a blue plug), then check: v4l2-ctl --list-devices"; exit 1; }
if [ -z "${AUDIO:-}" ]; then
  # USB capture cards show up as a USB audio card; take the first one that isn't the onboard sound
  CARD=$(awk -F'[][]' '/USB-Audio|Cam Link|Capture|MS2109|USB3/ {print $0}' /proc/asound/cards | awk '{print $1; exit}')
  AUDIO=${CARD:+hw:$CARD,0}
  AUDIO=${AUDIO:-none}
fi
FMT=yuyv422
v4l2-ctl -d "$VIDEO" --list-formats 2>/dev/null | grep -q NV12 && FMT=nv12
v4l2-ctl -d "$VIDEO" --list-formats 2>/dev/null | grep -q MJPG && ! v4l2-ctl -d "$VIDEO" --list-formats 2>/dev/null | grep -qE 'NV12|YUYV' && FMT=mjpeg
echo "   video: $VIDEO ($FMT)   audio: $AUDIO"

echo "== Writing settings"
cp "$SRC/mediamtx.yml" /etc/mediamtx.yml
install -m 755 "$SRC/camstream-run" /usr/local/bin/camstream-run
if [ ! -f /etc/camstream.env ]; then
  cat > /etc/camstream.env <<ENV
# camstream settings. After editing: systemctl restart camstream
VIDEO=$VIDEO
AUDIO=$AUDIO             # ALSA device of the capture card, or none for video only
INPUT_FORMAT=$FMT        # nv12 / yuyv422 / mjpeg (see: v4l2-ctl -d $VIDEO --list-formats-ext)
SIZE=1920x1080
FPS=60                   # 30 halves the CPU use
BITRATE=8M               # lower (4M) if it stutters away from home
PRESET=superfast         # veryfast = sharper but more CPU; ultrafast = least CPU
ENV
else
  echo "   /etc/camstream.env already exists, left unchanged"
fi
cp "$SRC/mediamtx.service" "$SRC/camstream.service" /etc/systemd/system/
systemctl daemon-reload
systemctl enable --now mediamtx >/dev/null 2>&1
systemctl enable camstream >/dev/null 2>&1
systemctl restart camstream
sleep 4
systemctl --no-pager --lines=5 status camstream || true
echo
echo "Test it: http://$(hostname -I | awk '{print $1}'):8889/vseebox"
