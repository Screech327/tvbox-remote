# Setup guide (start to finish)

Every step from parts to watching on your phone. Each step ends with a **check**. Don't move on until it passes.
If something fails, see [Troubleshooting](#troubleshooting).

Steps marked **(hands-on)** need a person at the hardware. Everything else can be done over SSH, including by an
AI assistant (see `CLAUDE.md`).

## 0. Pick your layout

Pictures of each layout: [docs/DIAGRAMS.md](DIAGRAMS.md).

| You want… | Layout |
|---|---|
| Only to watch remotely (TV not used) | **A:** box → capture card → host |
| Remote + the TV in the same room | **B:** box → HDMI splitter → TV + capture card |
| Remote + a TV in another room | **C:** box → HDMI splitter → capture card + HDMI extender → TV |

**HDCP warning:** the TV box's output may be copy-protected (HDCP), and capture cards can't receive
copy-protected video: the picture stays black. A plain pass-through, like an extender's loop-out, doesn't change
that. In layouts B and C, the capture card only gets a picture if the splitter's output is one it accepts.
See `docs/HISTORY.md`.

## 1. Parts

| Part | Notes |
|---|---|
| Raspberry Pi Pico 2 W | Must be the **2 W** (Wi-Fi + RP2350). About $7. |
| Micro-USB **data** cable | For the first flash only. Charge-only cables won't show the drive. |
| USB HDMI capture card | Tested: Elgato Cam Link 4K. Any card should work if it's **UVC / driver-free** (shows up like a webcam), captures **HDMI audio** too, accepts the box's output resolution, and does **1080p60** (USB 3 sends it uncompressed; USB 2 cards send compressed MJPEG, often only 1080p30, which `install.sh` detects). No passthrough needed. Plug USB 3 cards into a USB 3 port. |
| Host computer | Any always-on Linux machine: an old laptop or mini PC (x86), a Raspberry Pi 5, or a VM/LXC on a home server. Needs a USB 3 port for the capture card. Cheap option: a used business mini PC with an 8th-gen Intel i5 or newer (Lenovo ThinkCentre Tiny, HP EliteDesk Mini, Dell OptiPlex Micro). |
| HDMI splitter (layouts B/C) | Tested model: *to be confirmed*. |
| HDMI cables | One per connection in your layout. |

## 2. Flash and set up the Pico (hands-on)

Follow **First flash** and **First-time setup** in [`firmware/README.md`](../firmware/README.md).

**Check:** browse to `http://<pico-ip>/status` and see `"fw":"pico-…"` and your IP.
Also note: **the Pico's IP** (reserved in your router) and **the token** you chose.

Plug the Pico into a **USB port on the TV box**. Any port works. The Pico is powered by the box.

**Check:** `http://<pico-ip>/status` shows `"mounted":true`.

## 3. Prepare the host

- Debian 12/13, Ubuntu 22.04+ or Raspberry Pi OS (64-bit). A fresh install is fine.
- Plug the capture card into the host, and the TV box's HDMI into the capture card (or the splitter, for B/C).
- Copy this repo to the host, e.g. `git clone` or download the ZIP.

**Check:**
```bash
v4l2-ctl --list-devices        # (sudo apt install v4l-utils) should list the capture card
cat /proc/asound/cards         # the capture card's audio usually shows up as USB-Audio
```

*Proxmox users:* `server/examples/proxmox-lxc-setup.sh` creates an LXC container, passes the capture card
through and runs step 4 for you.

## 4. Install the stream

```bash
sudo bash server/stream/install.sh
```
It installs ffmpeg and MediaMTX, finds the capture card, writes `/etc/camstream.env` and starts two services
(`mediamtx`, `camstream`). To pick devices by hand: `sudo VIDEO=/dev/video0 AUDIO=hw:1,0 bash server/stream/install.sh`.

**Check:** open `http://<host-ip>:8889/vseebox`. You should see the TV box live.
- Black picture? See HDCP in Troubleshooting.
- Logs: `journalctl -u camstream -n 50`

Tuning: edit `/etc/camstream.env`, then `sudo systemctl restart camstream`. Options: `FPS=30` uses half the CPU,
`BITRATE=4M` helps on slow connections, `PRESET=ultrafast` uses the least CPU.

## 5. Install the control page

```bash
sudo bash server/control/install.sh <pico-ip> <token>
```
**Check:** open `http://<host-ip>/`. Both status pills should turn green. Press **▲ ▼** on the page and watch the
highlight move on the stream.

Settings are in `/etc/vseebox-control.env`. After editing: `sudo systemctl restart vseebox-control`.

## 6. Watch from anywhere (Tailscale)

1. Install Tailscale on the host and log in: `curl -fsSL https://tailscale.com/install.sh | sh && sudo tailscale up`
   (the login is **hands-on**).
2. Install the Tailscale app on your phone and laptop, and log into the same account.
3. Open `http://<host's Tailscale IP or name>/` from anywhere.

**Don't** port-forward this page to the internet. Anyone who can open it controls the box.

**Phones:** for no browser bar, use the browser menu → **Add to Home screen**, then open it from that icon.

## 7. Using it

- **Keyboard:** arrows, Enter = OK, **Esc = Back**, letters type, Backspace deletes, Home = Home.
- **Mouse:** click the picture to grab the mouse, **Esc** lets go. Right-click = Back.
  - The **scroll wheel** moves the highlight, which is what TV apps expect. Shift + wheel sends a real scroll.
- **Buttons under the picture:**
  - **Restart box** (click twice): restarts the TV box
  - **Unstick**: releases any stuck keys
  - **Switch HDMI**: only for the optional relay
- **vSeeBox player:** press **Up** to open Server / Quality / Subtitle.

## Troubleshooting

| Symptom | Fix |
|---|---|
| **Keys lag, land in bunches, stick, or go the wrong way** | **Restart the TV box first** (Restart box button, or unplug it). A stuck key can leave Android's input handling in a bad state that looks exactly like a USB problem. |
| Capture picture is black, but the TV works | **HDCP.** The capture card is getting copy-protected video. The device in front of the card, the splitter or switch, decides what the card receives. A loop-out passes the protection straight through. |
| Back does nothing | Use the page's Back / Esc / right-click, which send the media Back key. Plain Backspace only works as Back for some remotes' own USB IDs. |
| The mouse pointer isn't visible | It's parked at a screen edge. Move the mouse toward the middle of the picture. Video players also hide it while paused. |
| Pico LED blinking slowly | It's in setup mode: no settings, or no Wi-Fi for 10 minutes. Join `vSeeBox-Remote-Setup` and save again. |
| Pico LED blinking fast | Joining Wi-Fi. If it stays that way, check the 2.4 GHz network and the password. |
| Control page: "Remote" pill red | Wrong Pico IP or token in `/etc/vseebox-control.env`, or the Pico is offline. Test: `http://<pico-ip>/status`. |
| Lists overshoot when holding a key | Update to the latest control page. It holds keys for real instead of repeating taps. |
| Box went to sleep and won't wake | A **long** Power press sleeps it, and the Pico can't wake it, because USB is off while it's asleep. Use the IR remote or a smart plug. |
