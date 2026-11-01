# Instructions for AI assistants setting this up for someone

You're helping a person set up tvbox-remote: watch and control an Android TV box from a browser. Follow
`docs/SETUP.md` step by step. Read `docs/HISTORY.md` before troubleshooting, since most failure modes are
already documented there.

## How to run the setup

1. **Ask first:**
   - Which layout from `docs/SETUP.md` §0 (A, B or C)?
   - What will the host be (OS and CPU)?
   - Which capture card?
   - Do they want remote access (Tailscale)?
2. **Do the steps in order, and don't skip a step's Check.** Each step in SETUP.md ends with one. Run it, or have
   the person confirm it, before moving on.
3. **Some steps need the person's hands.** Tell them exactly what to do and wait:
   - holding BOOTSEL and dragging the `.uf2` (first flash)
   - joining the Pico's setup Wi-Fi and filling in the form (pick a token together, and have them keep it)
   - plugging cables
   - reserving the Pico's IP in their router
   - logging into Tailscale
   - restarting the TV box
4. **The rest can run on the host** if you have shell access: `server/stream/install.sh` and
   `server/control/install.sh <pico-ip> <token>`. Otherwise give the exact commands one at a time, as separate
   code blocks.
5. Firmware: use the prebuilt `.uf2`/`.bin` from the Actions artifacts or Releases. Only build from source if
   asked; `firmware/README.md` has the exact board options, and they matter.

## Rules learned the hard way (see docs/HISTORY.md)

- **If the TV box misbehaves (lag, bunched keys, stuck keys, wrong directions), have them restart the box before
  changing any code or settings.** Earlier stuck keys can leave Android's input state broken. In the original
  build, this cost hours of misdiagnosis.
- **Black capture with a working TV = HDCP.** Fix it in the HDMI chain, not in software.
- **Don't add an absolute/touch pointer HID to the firmware.** Android then ignores the keyboard.
- **Don't send key repeats as repeated taps.** Hold keys with `p`/`r` and let the box repeat, or lists overshoot.
- **Back = media Back (`m aback`).** Not Backspace, except for some remotes' own USB IDs.
- **Never send a long Power press.** It sleeps the box, and the Pico can't wake it. A short press opens the power
  menu, with a countdown on "Shut down".
- **Verify what you think you see.** A screenshot of the stream can include the viewer's own mouse cursor; the TV
  box's pointer is a different, larger arrow. Confirm assumptions with the person before acting on them.
- **Never commit secrets:** the token, Wi-Fi details, IPs.

## Layout of the repo

- `firmware/tvbox-remote/`: Pico 2 W firmware (arduino-pico 6.3.1, Adafruit TinyUSB, 4 MB with a 2 MB FS)
- `server/stream/`: ffmpeg → MediaMTX (WebRTC :8889 path `/vseebox`, HLS :8888), systemd units
- `server/control/`: `server.py` (stdlib only) + `index.html`, port 80, talks UDP 4210 to the Pico with the token
- `server/examples/proxmox-lxc-setup.sh`: optional Proxmox LXC installer
