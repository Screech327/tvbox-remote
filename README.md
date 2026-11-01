# tvbox-remote

Watch and control an Android TV box from anywhere, in a browser, on a computer or a phone.

- **Watch:** the box's HDMI output goes into a USB capture card. ffmpeg and MediaMTX stream it over WebRTC with
  near-real-time latency.
- **Control:** a **Raspberry Pi Pico 2 W** plugs into the box's USB port and acts as a USB keyboard + mouse,
  taking commands over Wi-Fi. Point, click, type, arrows, OK, Back, Home, volume, and a Restart button.
- **Anywhere:** use Tailscale for access away from home. No port forwarding.

Tested on a **vSeeBox**. It should work with most Android TV boxes that accept a USB keyboard and mouse.

> **Status: work in progress (private).** This is the reference build from one home lab. Packaging for
> everyone else is next: one-command Docker setup, automatic Pico discovery, prebuilt firmware, setup guides and
> diagrams for each layout.

## Set it up

**Follow [docs/SETUP.md](docs/SETUP.md).** It goes from parts to watching on your phone, with a check after every
step and a troubleshooting table. Using an AI assistant? Point it at this repo. [CLAUDE.md](CLAUDE.md) tells it
how to walk you through the setup and which mistakes to avoid.

## What's in here

| Path | What |
|---|---|
| `firmware/` | Pico 2 W firmware, how to flash it, how to build it, and the command protocol |
| `firmware/patches/` | Optional TinyUSB patch that enables the `/usblog` USB request log |
| `server/stream/` | Capture + streaming: ffmpeg → MediaMTX (WebRTC/HLS), systemd units |
| `server/control/` | Control page + small Python server (no dependencies) |
| `server/examples/` | Optional: Proxmox LXC installer |
| `docs/SETUP.md` | **Step-by-step setup guide** |
| `docs/HISTORY.md` | **Why everything is the way it is**: what was tried, what broke, lessons learned |

## Parts (reference build)

| Part | Notes |
|---|---|
| Raspberry Pi Pico 2 W | The USB keyboard/mouse. About $7. |
| USB HDMI capture card | Elgato Cam Link 4K used here. Cheaper USB capture cards should work too (untested). |
| Always-on computer | Linux host for capture + web server. Here: an old laptop running Proxmox. |
| HDMI splitter *(optional)* | Only if the TV should keep working too. Tested model to be confirmed. |
| 5V relay module *(optional)* | Only for pressing a physical HDMI-switch button (GP15). |

## Quick facts

- Pico setup: on first boot it opens a Wi-Fi network called `vSeeBox-Remote-Setup`. Join it and enter your Wi-Fi
  and a token (password).
- Pico pages: `/` (test remote), `/setup`, `/update` (firmware upload), `/status`, `/usblog`.
- If the TV box starts acting strange (laggy, wrong directions, stuck keys), **restart the box first.** See
  [docs/HISTORY.md](docs/HISTORY.md).
