# How this setup came to be

This is the story behind every piece of tvbox-remote: what was tried, what broke, and why the final design
looks the way it does. If something in the setup seems odd, the reason is probably in here. Most of the
lessons apply to any Android TV box, not only the vSeeBox it was built on.

## The goal

Watch and control a vSeeBox (an Android-based IPTV box) from anywhere, including from a laptop or phone while
traveling. The box was only watchable on the living room TV.

"Control" had a specific meaning: **point, click and type**, plus the normal remote buttons (arrows, OK, Back,
Home, volume). An on-screen picture of a remote wasn't the point. Common commands had to work, and input lag
had to be low enough to actually use it.

## Moving the box and the network

- The box moved into a small rack with the rest of the home lab, hardwired, on its own isolated network on the
  UniFi gateway.
- The TV is in another room, so an **OREI HDMI extender** (HDMI over Cat, 4K30, with IR and a loop-out) carries
  the picture back to the living room TV.
- The capture computer is an old **HP Omen 15 laptop** running **Proxmox**. The streaming software runs in a
  small Linux container on it.

None of this is required. Any always-on computer with a USB 3 port can do the capture and hosting (see the
README). The rack and Proxmox are just how this particular build is set up.

## Getting the picture: capture and streaming

- **Capture:** an **Elgato Cam Link 4K** (USB HDMI capture). It shows up on Linux as a normal video device and
  ALSA audio device.
- **Encoding:** `ffmpeg` encodes H.264 with every low-latency option turned on: `superfast` preset, no
  lookahead (`rc-lookahead=0`, `sync-lookahead=0`), `-fflags nobuffer -flags low_delay`, a small
  `thread_queue_size`, a VBV buffer of half the bitrate, and packets flushed right away.
- **Serving:** **MediaMTX** takes the ffmpeg stream and serves it over **WebRTC (WHEP)**. HLS is also on as a
  slower backup player.
- **Result:** close to real time. It was measured by putting a clock on the box's screen and comparing it to the
  real clock. The browser player also sets `jitterBufferTarget = 0`, so the browser doesn't add its own delay.
- **Audio:** comes from the Cam Link's audio device. It was turned off for a while during lag testing, and
  ffmpeg reports harmless "non-monotonic DTS" warnings on the audio track that still need cleaning up.

## Keeping the TV working: switch, then splitter

With the box in the rack, its single HDMI output has to feed both the Cam Link and the TV (through the extender).

1. **The OREI extender's loop-out** looked like the obvious answer. It gave **no picture**.
2. **Anker HDMI switch (A83H1), used backwards** (one source, two outputs) worked, but you had to press its
   button to flip between the TV and the capture card.
3. The plan was to have the Pico press that button through a **relay** (GP15). The relay works and is still
   wired, but opening the Anker to solder onto the button wasn't appealing.
4. **Current plan: an HDMI splitter** (ezcoo SP12H2 ordered). It has EDID modes and a downscaler, so both
   outputs get the picture at once and nothing needs switching. *Status: being tested.*

> TODO (Sam): describe the HDCP problem we hit and how it got resolved.

## Control: why a USB keyboard and mouse at all

The vSeeBox has **no Developer options and no ADB**, so there's no way to control it from software. Anything
that controls it has to look like a physical input device. The options considered:

| Option | Verdict |
|---|---|
| **IR blaster** (replay the IR remote's codes) | Works with no lag, since the IR remote is instant. But it needs line of sight and gives no mouse. Kept as a fallback. |
| **Bluetooth keyboard/mouse** (ESP32 BLE) | Pairing and reconnects on Android TV boxes are flaky. Not pursued. |
| **KVM-over-IP** (NanoKVM, PiKVM) | NanoKVM had reports (Dec 2025) of an undocumented microphone and weak security defaults, and Android-box compatibility was unverified. PiKVM costs much more. |
| **Pi Zero 2 W as a USB gadget** (proven on Android TV) | Out of stock everywhere at the time (Digi-Key said March 2027). |
| **A microcontroller acting as a USB keyboard + mouse** | Chosen. Cheap, plugs into the box's USB port, takes commands over Wi-Fi. |

## Attempt 1: Arduino Nano ESP32 (ESP32-S3)

The first controller was an Arduino Nano ESP32. It never became usable. The problems, in order:

1. **One HID interface only.** The ESP32 Arduino core's TinyUSB is compiled with `CFG_TUD_HID = 1`, so the
   keyboard, mouse and media keys all had to share **one** USB interface, told apart by report IDs. Real
   keyboards and remote dongles use **separate** interfaces, and a boot keyboard is on its own. The box accepted
   the mouse but **ignored the keys**.
2. **USB setup order.** Creating the HID objects inside `setup()` (after USB had already started) broke
   enumeration completely: Linux logged `usbhid probe failed -22`. They have to be globals.
3. **Identity.** The Arduino core forces its own USB VID. Using the Espressif core with USB-CDC disabled made it
   possible to copy the XING WEI keyboard-remote dongle's ID. Typing then worked, but keys stuck or repeated, it
   was laggy, and input stalled roughly every 20 seconds.
4. **Blocking sends.** `SendReport()` fails instantly when the endpoint isn't ready, and otherwise blocks for up
   to 100 ms. Sending every mouse step directly built up a **15-second backlog** and lost key releases, which
   caused the stuck keys. Coalescing mouse movement and waiting for the endpoint helped, but didn't fix the
   underlying problem.

## Attempt 2: Raspberry Pi Pico 2 W (the one that works)

- **Why:** the Pico core's Adafruit TinyUSB allows **two separate HID interfaces**: a plain boot keyboard, plus
  a mouse/media-keys interface. That's the same split the box's own keyboard-remote dongle uses. It also has
  Wi-Fi and costs about $7.
- **Firmware design:**
  - Nothing blocks. Commands become USB reports in two queues, one per interface, which drain as fast as the box
    takes them.
  - Mouse motion is added up and sent whenever the mouse interface is free.
  - If the box stops taking reports for 1.5 s, the queue is dropped instead of piling up.
  - Setup is a captive-portal web page: first boot opens a "setup" Wi-Fi network. Holding BOOTSEL for 5 s wipes
    the settings.
  - Firmware updates go through `/update` in the browser. No cable needed after the first flash.
  - From pico-5: it keeps retrying Wi-Fi after an update, instead of falling back to setup mode.

## The debugging detour (lessons that cost a whole afternoon)

After the Pico went in, keys were erratic: presses ignored, then several landing at once, keys stuck
auto-repeating, and directions apparently wrong. A lot got tried:

- **Copying the dongle exactly.** The real XING WEI dongle (`1915:1025`) was captured with `lsusb -v` and its
  HID report descriptors. It has **4 interfaces**: a dummy microphone (audio interfaces 0–1), a boot keyboard on
  interface 2 with 8-byte packets, and a boot-mouse interface carrying mouse, media, system and vendor reports
  with 32-byte packets. The Pico now mirrors that layout byte for byte.
- **Timing the real remote.** A press is one report held about 200 ms, then one release. Back is plain
  **Backspace** (`0x2A`), and Home is media Home (`0x0223`). The Pico's tap timing copies that.
- **A USB request log** (`/usblog`, which needs `firmware/patches/tinyusb-usbd-logging.patch`) records every reset,
  suspend, control request and report pickup. It proved the box collects every report **within 1 ms**. USB was
  never the problem.

**The actual cause: the box itself had gotten stuck.** The stuck keys from the earlier attempts left Android's
input handling in a bad state. Even the real keyboard remote went in random directions until the box was
**restarted**. After a restart, everything worked.

**Lesson #1: if the box starts acting strange, restart it before changing anything.** It's also why the control
page has a Restart box button now.

## Control page details (and why)

- **One small Python server** (standard library only). It serves the page, passes the WebRTC handshake through to
  MediaMTX so everything is on one address, and sends commands to the Pico over **UDP with a token**. The token
  stays on the server and never reaches the browser.
- **Keys are held for real.** Arrows and OK press on key-down and release on key-up, so the box does its own key
  repeat, like a real keyboard. Earlier, each OS key-repeat became a 0.3 s tap. Those queued up and kept arriving
  after you let go, so lists overshot and wrapped back to the top.
- **The scroll wheel moves the highlight.** TV apps scroll by moving the highlight. A real mouse-wheel scroll gets
  snapped back to the highlighted item: you'd reach season 2 and jump back to season 5. Shift+wheel still sends a
  real scroll.
- **Back sends the media Back key** (AC Back, `0x0224`). Plain Backspace only works as Back for the dongle's own
  USB ID. With the Pico's generic ID it just deletes text.
- **In the video player, Up opens Server / Quality / Subtitle.** That's how you switch to an English server.
- **Restart box:**
  - A **short** Power press opens Sleep / Shut down / Reboot, with **Shut down** selected and a countdown that
    runs it. The button presses Power, then Right, then OK, well before the countdown ends. It needs a second
    click to confirm.
  - A **long** Power press goes straight to sleep. While asleep, the box turns off USB without allowing remote
    wake-up, so the Pico can't wake it. That takes the IR remote, or a smart plug.
- **Full screen on phones:** iPhone Safari (and some other browsers) won't full-screen a page element, so the page
  fills the window instead. Use **Add to Home screen** for no browser bar.
- **Remote access:** **Tailscale** on the host and on the phone or laptop. No port forwarding.

## Things that did NOT matter (so you don't chase them)

- Which of the box's USB ports is used. Both behave the same.
- Copying the dongle's USB ID. The generic Raspberry Pi ID works fine (and any ID looked broken while the box was
  in its stuck state).
- Sending duplicate reports "in case they get lost." Nothing gets lost.
