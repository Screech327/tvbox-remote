# Pico 2 W firmware

The Pico plugs into the TV box's USB port and shows up as a **USB keyboard** (interface 0, boot keyboard) plus a
**mouse + media keys** device (interface 1). It joins your Wi-Fi and takes commands over UDP port 4210 from the
control page server.

## Easiest: use a prebuilt file

Every push to `main` builds the firmware automatically: open the **Actions** tab, then the latest **firmware**
run, then **Artifacts**. Tagged releases attach the files to the **Releases** page.

- `tvbox-remote.uf2`: for the **first** flash, by USB drag-and-drop
- `tvbox-remote.bin`: for **updates** over Wi-Fi at `http://<pico-ip>/update`

### First flash

1. Hold the white **BOOTSEL** button on the Pico. While holding it, plug the Pico into a computer with a
   **data** micro-USB cable, then let go.
2. A drive called **RP2350** appears. Drag `tvbox-remote.uf2` onto it. The Pico restarts by itself.

### First-time setup (no computer needed)

1. The Pico's LED blinks slowly and it opens a Wi-Fi network called **TVBox-Remote-Setup** (**vSeeBox-Remote-Setup** on firmware before pico-6). Join it from a
   phone. A setup page opens; if it doesn't, browse to `http://192.168.42.1`.
2. Enter:
   - your **Wi-Fi name and password** (2.4 GHz only, the Pico can't use 5 GHz)
   - a **token**: any password you make up. The control page server needs the same one.
3. Tap **Save and restart**. The LED goes solid once it's on your Wi-Fi.
4. In your router, find the device named **tvbox-remote** (**vseebox-remote** before pico-6) and give it a **fixed (reserved) IP**.

To start over: hold **BOOTSEL for 5 seconds** while it's running. That wipes the settings and reopens setup.

### Pico web pages

| Page | What |
|---|---|
| `/` | Built-in test remote (needs the token) |
| `/setup` | Change Wi-Fi, token, USB identity, relay press length (login `admin` / your token) |
| `/update` | Upload a new `.bin` (login `admin` / your token). Use the **Firmware** row. |
| `/status` | JSON: firmware version, IP, Wi-Fi signal, USB state, counters |
| `/usblog` | Log of what the TV box does over USB (full detail needs the TinyUSB patch, see below) |

## Building it yourself

Toolchain: the **arduino-pico** core by Earle Philhower, version **6.3.1** (the tested version).

```bash
arduino-cli config add board_manager.additional_urls \
  https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json
arduino-cli core update-index
arduino-cli core install rp2040:rp2040@6.3.1
arduino-cli compile \
  -b "rp2040:rp2040:rpipico2w:usbstack=tinyusb,flash=4194304_2097152" \
  --output-dir out firmware/tvbox-remote
# out/tvbox-remote.ino.uf2  (first flash)    out/tvbox-remote.ino.bin  (Wi-Fi update)
```

Arduino IDE 2 works too: install **Raspberry Pi Pico/RP2040/RP2350** by Earle Philhower from Boards Manager, then
set **Board: Raspberry Pi Pico 2W**, **USB Stack: Adafruit TinyUSB** and **Flash Size: 4MB (Sketch: 2MB,
FS: 2MB)**. The 2 MB file system is needed for Wi-Fi updates.

Both settings matter:
- **USB stack = Adafruit TinyUSB.** The default "Pico SDK" stack can't make two separate HID interfaces, and the
  TV box needs the keyboard on its own interface. `docs/HISTORY.md` explains why.
- **Flash split 2 MB / 2 MB.** The `/update` page stages the new firmware in the file system area.

### Optional: full USB log (`firmware/patches/tinyusb-usbd-logging.patch`)

Without the patch, `/usblog` only shows the reports the Pico sends. With it, the log also shows every request,
reset and suspend from the TV box, and when each report was picked up. It's only for debugging.

```bash
cd ~/.arduino15/packages/rp2040/hardware/rp2040/6.3.1/libraries/Adafruit_TinyUSB_Arduino/src/device
patch -p0 < /path/to/tvbox-remote/firmware/patches/tinyusb-usbd-logging.patch
```

## Command protocol (for the server, or for scripting)

Send UDP to port 4210: `<TOKEN> <command>`. Several commands go one per line, with the token once at the start.

| Command | Does |
|---|---|
| `k up/down/left/right/enter/ok/esc/tab/backspace/space/pgup/pgdn/delete/menu` | tap a key (about 200 ms, like a real remote) |
| `t <text>` | type ASCII text |
| `p <code>` / `r <code>` | hold / release an Arduino key code (arrows: up 218, down 217, left 216, right 215, Enter 176) |
| `x` | release everything |
| `m home/aback/menu/play/stop/next/prev/ff/rew/volup/voldown/mute/search/power/chup/chdn` | media key. `aback` = Android **Back** |
| `rel <dx> <dy>` | move the mouse |
| `rb <1/2/3> <0/1>` | mouse button up/down (left/right/middle) |
| `w <n>` | scroll wheel |
| `sw [ms]` | pulse the relay on GP15 (from the [old HDMI-switch setup](../docs/archive/anker-switch-setup.md); nothing needs it now) |
| `tm <tap> <gap> <type> <typegap> <media> <copies>` | change key timing live |
| `ping`, `usb`, `stats`, `reboot` | diagnostics |
