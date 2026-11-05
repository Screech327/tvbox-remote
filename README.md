# tvbox-remote

Watch and control an Android TV box from anywhere, in a browser, on a computer or a phone.

- **Watch:** the box's HDMI output goes into a USB capture card. ffmpeg and MediaMTX stream it over WebRTC with
  near-real-time latency.
- **Control:** a **Raspberry Pi Pico 2 W** plugs into the box's USB port and acts as a USB keyboard + mouse,
  taking commands over Wi-Fi. Point, click, type, arrows, OK, Back, Home, volume, and a Restart button.
- **Anywhere:** use Tailscale for access away from home. No port forwarding.

Tested on a **vSeeBox**. It should work with most Android TV boxes that accept a USB keyboard and mouse.

> **Status:** working and in daily use on the reference build (Linux host, Elgato Cam Link 4K, ezcoo SP12H2
> splitter, vSeeBox). A Windows host version is new (tested with a test picture on an AMD PC; first real
> capture card on Windows still to come). Next up: a one-command Docker setup and automatic Pico discovery.

## Set it up

**Follow [docs/SETUP.md](docs/SETUP.md).** It goes from parts to watching on your phone, with a check after every
step and a troubleshooting table. Using an AI assistant? Point it at this repo. [CLAUDE.md](CLAUDE.md) tells it
how to walk you through the setup and which mistakes to avoid.

**Something not working, or found a fix?** Open an issue (*Issues → New issue → Problem or fix*). See
[CONTRIBUTING.md](CONTRIBUTING.md). AI assistants following CLAUDE.md will offer to write and post it for you.

## What's in here

| Path | What |
|---|---|
| `firmware/` | Pico 2 W firmware, how to flash it, how to build it, and the command protocol |
| `firmware/patches/` | Optional TinyUSB patch that enables the `/usblog` USB request log |
| `server/stream/` | Capture + streaming: ffmpeg → MediaMTX (WebRTC/HLS), systemd units |
| `server/control/` | Control page + small Python server (no dependencies) |
| `server/windows/` | Windows host: one-command installer (`install.ps1`) and the host program |
| `server/examples/` | Optional: Proxmox LXC installer |
| `docs/SETUP.md` | **Step-by-step setup guide** |
| `docs/DIAGRAMS.md` | Which layout you need, how it all connects, and the reference home-lab build |
| `docs/HISTORY.md` | **Why everything is the way it is**: what was tried, what broke, lessons learned |
| `docs/archive/` | Retired setups, kept for history (the Anker HDMI switch build) |

## Parts (reference build)

| Part | Notes |
|---|---|
| Raspberry Pi Pico 2 W | The USB keyboard/mouse. About $7. |
| USB HDMI capture card | Elgato Cam Link 4K used here. Cheaper cards should work if they meet the requirements in [SETUP.md](docs/SETUP.md#1-parts) (untested so far). |
| Always-on computer | Linux host for capture + web server (here: an old laptop running Proxmox), or a **Windows PC**. |
| HDMI splitter *(optional)* | Only if the TV should keep working too. Tested: **ezcoo SP12H2** (1x2), default settings. |

## Quick facts

- Pico setup: on first boot it opens a Wi-Fi network called `TVBox-Remote-Setup` (`vSeeBox-Remote-Setup` on
  firmware before pico-6). Join it and enter your Wi-Fi and a token (password).
- Pico pages: `/` (test remote), `/setup`, `/update` (firmware upload), `/status`, `/usblog`.
- If the TV box starts acting strange (laggy, wrong directions, stuck keys), **restart the box first.** See
  [docs/HISTORY.md](docs/HISTORY.md).
- Some internal names (the `vseebox-control` service, the `/vseebox` stream path) come from the original build.
  They're kept so existing installs keep working.
