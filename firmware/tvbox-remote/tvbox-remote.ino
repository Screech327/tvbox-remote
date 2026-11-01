/*
  vSeeBox remote — Raspberry Pi Pico 2 W                                   firmware "pico-1"

  Plugs into a vSeeBox USB port and shows up the way the XING WEI keyboard-remote dongle does:
    USB interface 0: a plain boot keyboard (no report IDs)
    USB interface 1: mouse (report 1) + media/consumer keys (report 2)
  It takes commands over Wi-Fi and also pulses a relay on GP15 (the Anker HDMI switch button).

  First boot: no settings yet, so it opens a Wi-Fi network called "vSeeBox-Remote-Setup".
  Join it, a setup page opens (or browse to http://192.168.42.1), enter Wi-Fi + token, Save.
  Hold the white BOOTSEL button for 5 seconds while it runs to wipe the settings and start setup again.

  Command format (UDP port 4210, or /cmd?q=):  <TOKEN> <command> [args]     several commands = one per line
    k <name>        tap a key: up down left right enter ok esc tab backspace space pgup pgdn delete
                    (k back / k home send the media Back / Home keys)
    t <text>        type text (ASCII)
    p <code>        hold an Arduino key code (0-255)     r <code> release it     x  release everything
    m <name>        media key: home back menu play stop next prev ff rew volup voldown mute search power chup chdn
    mk <name>       media key sent as a keyboard key instead (fallback test)
    rel <dx> <dy>   move the mouse            rb|b <btn> <0|1>  mouse button (1 left, 2 right, 3 middle)
    w <steps>       scroll wheel (+ up, - down)
    sw [ms]         pulse the relay (presses the Anker switch button)
    ping            reply "pong"       usb  reply USB state       stats  reset counters      reboot

  Nothing here blocks: commands become USB reports in two queues (one per interface) that drain as fast
  as the box accepts them. Mouse motion is added up and sent whenever the mouse interface is free.
  If the box stops taking reports for 1.5 s, the queue is dropped instead of piling up.
*/
#include <WiFi.h>
#include <WiFiUdp.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <LEAmDNS.h>
#include <HTTPUpdateServer.h>
#include <EEPROM.h>
#include <Adafruit_TinyUSB.h>
#include "device/usbd_pvt.h"

#define FW_VERSION "pico-6"
#define UDP_PORT 4210
#define RELAY_PIN 15
#define CFG_MAGIC 0x56534232UL
#define SETUP_SSID "vSeeBox-Remote-Setup"

// ---------------------------------------------------------------- settings (flash)
struct Config {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  char token[65];
  char host[33];
  uint8_t usbId;      // 1 = generic Raspberry Pi ID (default), 0 = XING WEI 2.4G dongle look-alike
  uint8_t relayLow;   // 1 = relay board turns on with a LOW signal
  uint16_t pulseMs;   // how long the relay "presses" the button
};
Config cfg;
bool haveCfg = false;

static void loadCfg() {
  EEPROM.begin(512);
  EEPROM.get(0, cfg);
  haveCfg = cfg.magic == CFG_MAGIC && cfg.ssid[0] && cfg.token[0];
  if (cfg.magic != CFG_MAGIC) {
    memset(&cfg, 0, sizeof(cfg));
    strcpy(cfg.host, "vseebox-remote");
    cfg.pulseMs = 250;
    cfg.usbId = 1;     // pico-6: generic ID by default (tested on vSeeBox); the look-alike is optional
  }
  cfg.ssid[32] = cfg.pass[64] = cfg.token[64] = cfg.host[32] = 0;
  if (!cfg.host[0]) strcpy(cfg.host, "vseebox-remote");
  if (cfg.pulseMs < 50 || cfg.pulseMs > 3000) cfg.pulseMs = 250;
}
static void saveCfg() {
  cfg.magic = CFG_MAGIC;
  EEPROM.put(0, cfg);
  EEPROM.commit();
}
static void wipeCfg() {
  memset(&cfg, 0, sizeof(cfg));
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------------------------------------------------------------- USB
// pico-3: byte-for-byte copy of the XING WEI 2.4G dongle (1915:1025) as captured with lsusb on the Omen:
//   interface 0 = audio control ("Mic Device"), 1 = audio streaming (16 kHz mic, iso EP 0x83)
//   interface 2 = boot keyboard, EP 0x81, 8-byte packets
//   interface 3 = boot-mouse interface carrying mouse (ID 1), consumer (ID 2), system (ID 3), vendor (ID 8/4), EP 0x82, 32-byte packets
// The vSeeBox ignores keyboards with any other identity, so we match it exactly.
enum { RID_MOUSE = 1, RID_CONSUMER = 2 };
static uint8_t const descKb[] = {   // 65 bytes, from /sys/bus/hid/devices/0003:1915:1025.000B/report_descriptor
  0x05,0x01,0x09,0x06,0xa1,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0x00,0x25,0x01,
  0x75,0x01,0x95,0x08,0x81,0x02,0x75,0x08,0x95,0x01,0x81,0x03,0x75,0x01,0x95,0x05,
  0x05,0x08,0x19,0x01,0x29,0x05,0x91,0x02,0x75,0x03,0x95,0x01,0x91,0x03,0x75,0x08,
  0x95,0x06,0x15,0x00,0x26,0xff,0x00,0x05,0x07,0x19,0x00,0x2a,0xff,0x00,0x81,0x00,
  0xc0 };
static uint8_t const descMc[] = {   // 151 bytes, from .../0003:1915:1025.000C/report_descriptor
  0x05,0x01,0x09,0x02,0xa1,0x01,0x85,0x01,0x09,0x01,0xa1,0x00,0x05,0x09,0x19,0x01,
  0x29,0x05,0x15,0x00,0x25,0x01,0x95,0x05,0x75,0x01,0x81,0x02,0x95,0x01,0x75,0x03,
  0x81,0x01,0x05,0x01,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,0x75,0x08,
  0x95,0x03,0x81,0x06,0x05,0x0c,0x0a,0x38,0x02,0x95,0x01,0x81,0x06,0xc0,0xc0,0x05,
  0x0c,0x09,0x01,0xa1,0x01,0x85,0x02,0x15,0x00,0x26,0x3c,0x04,0x19,0x00,0x2a,0x3c,
  0x04,0x95,0x01,0x75,0x10,0x81,0x00,0xc0,0x05,0x01,0x09,0x80,0xa1,0x01,0x85,0x03,
  0x05,0x01,0x19,0x81,0x29,0x83,0x15,0x00,0x25,0x01,0x75,0x01,0x95,0x03,0x81,0x02,
  0x75,0x01,0x95,0x05,0x81,0x01,0xc0,0x06,0x01,0xff,0x09,0x01,0xa1,0x02,0x85,0x08,
  0x09,0x14,0x75,0x08,0x95,0x10,0x15,0x80,0x25,0x7f,0x81,0x22,0x85,0x04,0x09,0x04,
  0x75,0x08,0x95,0x08,0x91,0x22,0xc0 };

// HID interface with the dongle's exact interface/HID/endpoint descriptors (bcdHID 2.01, per-interface packet size)
class DongleHid : public Adafruit_USBD_HID {
public:
  DongleHid(uint8_t proto, uint8_t mps) : _p(proto), _mps(mps) {}
  uint16_t getInterfaceDescriptor(uint8_t, uint8_t* buf, uint16_t bufsize) override {
    const uint16_t len = 9 + 9 + 7;
    if (!buf) return len;
    if (bufsize < len) return 0;
    uint8_t itf = TinyUSBDevice.allocInterface(1);
    uint8_t ep = TinyUSBDevice.allocEndpoint(TUSB_DIR_IN);
    uint16_t rl = _rlen;
    uint8_t d[len] = {
      9, TUSB_DESC_INTERFACE, itf, 0, 1, TUSB_CLASS_HID, 1, _p, 0,
      9, HID_DESC_TYPE_HID, 0x01, 0x02, 0, 1, HID_DESC_TYPE_REPORT, (uint8_t)(rl & 0xff), (uint8_t)(rl >> 8),
      7, TUSB_DESC_ENDPOINT, ep, TUSB_XFER_INTERRUPT, _mps, 0, 1 };
    memcpy(buf, d, len);
    return len;
  }
  void setDesc(const uint8_t* d, uint16_t l) { _rlen = l; setReportDescriptor(d, l); }
private:
  uint8_t _p, _mps; uint16_t _rlen = 0;
};
DongleHid hidKb(HID_ITF_PROTOCOL_KEYBOARD, 8);
DongleHid hidMc(HID_ITF_PROTOCOL_MOUSE, 32);

// The dongle's (unused) voice-mic interfaces 0 and 1, so the keyboard lands on interface 2 like the real one.
class DongleMic : public Adafruit_USBD_Interface {
public:
  uint16_t getInterfaceDescriptor(uint8_t, uint8_t* buf, uint16_t bufsize) override {
    static const uint8_t tmpl[] = {
      9,4, 0,0,0, 1,1,0, 0,                       // AC interface (iInterface patched below)
      9,0x24,1, 0x00,0x01, 0x27,0x00, 1, 1,       // header: bcdADC 1.00, wTotalLength 0x27, 1 streaming itf = 1
      12,0x24,2, 4, 0x01,0x02, 0, 1, 0x00,0x00, 0, 0,   // input terminal 4: microphone
      9,0x24,3, 6, 0x01,0x01, 2, 7, 0,            // output terminal 6: USB streaming, source 7
      7,0x24,5, 7, 1, 5, 0,                       // selector unit 7
      9,0x24,6, 5, 4, 1, 0x43, 0x00, 0,           // feature unit 5: mute/volume/AGC
      9,4, 1,0,0, 1,2,0, 0,                       // AS interface 1 alt 0
      9,4, 1,1,1, 1,2,0, 0,                       // AS interface 1 alt 1
      7,0x24,1, 6, 1, 0x01,0x00,                  // AS general: PCM
      11,0x24,2, 1, 1, 2, 16, 1, 0x80,0x3e,0x00,  // format: mono 16-bit 16 kHz
      9,5, 0x83, 0x05, 0x20,0x00, 1, 0, 0,        // iso IN EP 0x83, 32 bytes
      7,0x25,1, 0, 0, 0x00,0x00 };
    if (!buf) return sizeof(tmpl);
    if (bufsize < sizeof(tmpl)) return 0;
    uint8_t first = TinyUSBDevice.allocInterface(2);
    uint8_t strid = TinyUSBDevice.addStringDescriptor("Mic Device");
    memcpy(buf, tmpl, sizeof(tmpl));
    buf[2] = first; buf[8] = strid;
    buf[9 + 8] = first + 1;                          // header baInterfaceNr(0)
    buf[55 + 2] = first + 1; buf[64 + 2] = first + 1; // AS interface numbers
    return sizeof(tmpl);
  }
};
DongleMic mic;

// Minimal class driver that claims the mic interfaces and answers their requests (no audio is ever sent).
static uint8_t micAlt = 0, micBuf[8];
static void micInit(void) {}
static bool micDeinit(void) { return true; }
static void micReset(uint8_t) { micAlt = 0; }
static uint16_t micOpen(uint8_t, tusb_desc_interface_t const* itf, uint16_t max_len) {
  if (itf->bInterfaceClass != TUSB_CLASS_AUDIO || itf->bInterfaceSubClass != 1) return 0;
  uint8_t const* p = (uint8_t const*)itf;
  uint8_t const* end = p + max_len;
  uint16_t len = 0;
  uint8_t seen = 0;
  while (p + len < end) {           // swallow AC + both AS alternates, stop at the next non-audio interface
    uint8_t const* d = p + len;
    if (d[1] == TUSB_DESC_INTERFACE && len) {
      if (d[5] != TUSB_CLASS_AUDIO) break;
      seen++;
    }
    len += d[0];
  }
  (void)seen;
  return len;
}
static bool micControl(uint8_t rhport, uint8_t stage, tusb_control_request_t const* req) {
  if (stage != CONTROL_STAGE_SETUP) return true;
  if (req->bmRequestType_bit.type == TUSB_REQ_TYPE_STANDARD) {
    if (req->bRequest == TUSB_REQ_SET_INTERFACE) { micAlt = (uint8_t)req->wValue; return tud_control_status(rhport, req); }
    if (req->bRequest == TUSB_REQ_GET_INTERFACE) { micBuf[0] = micAlt; return tud_control_xfer(rhport, req, micBuf, 1); }
    return false;
  }
  uint16_t n = req->wLength > sizeof(micBuf) ? sizeof(micBuf) : req->wLength;
  memset(micBuf, 0, sizeof(micBuf));
  return tud_control_xfer(rhport, req, micBuf, n);   // class GET: zeros; class SET: accept and ignore
}
static bool micXfer(uint8_t, uint8_t, xfer_result_t, uint32_t) { return true; }
static usbd_class_driver_t const micDriver = { "MIC", micInit, micDeinit, micReset, micOpen, micControl, micXfer, nullptr, nullptr };
extern "C" usbd_class_driver_t const* usbd_app_driver_get_cb(uint8_t* count) { *count = 1; return &micDriver; }

static uint8_t const ascii2kc[128][2] = { HID_ASCII_TO_KEYCODE };

// pico-4: log of everything the host does to us (resets, sleeps, control requests, report pickups),
// so we can see what the vSeeBox is doing without access to it. Shown at /usblog.
struct ULog { uint32_t ms; uint8_t type; uint8_t a, b; uint16_t c, d, e; };
// type: 1 bus reset, 2 unplug, 3 suspend, 4 resume, 5 xfer error, 6 EP0 stall, 7 report picked up (a=ep),
//       8 report queued (a=ep, c=first key/usage), 9 setup (a=bmRequestType b=bRequest c=wValue d=wIndex e=wLength)
#define ULOG_N 160
volatile ULog ulog[ULOG_N]; volatile uint32_t ulogHead = 0;
volatile uint32_t ucnt[10] = {0};
static void ulogAdd(uint8_t t, uint8_t a, uint8_t b, uint16_t c, uint16_t d, uint16_t e) {
  if (t < 10) ucnt[t]++;
  uint32_t i = ulogHead++ % ULOG_N;
  ulog[i].ms = millis(); ulog[i].type = t; ulog[i].a = a; ulog[i].b = b; ulog[i].c = c; ulog[i].d = d; ulog[i].e = e;
}
extern "C" void usbdlog_event(uint8_t e, uint32_t v) { ulogAdd(e, (uint8_t)v, (uint8_t)(v >> 8), 0, 0, 0); }
extern "C" void usbdlog_setup(tusb_control_request_t const* r) {
  ulogAdd(9, r->bmRequestType, r->bRequest, r->wValue, r->wIndex, r->wLength);
}

static void startUsb() {
  if (!TinyUSBDevice.isInitialized()) TinyUSBDevice.begin(0);
  TinyUSBDevice.clearConfiguration();          // drop the default USB serial port
  if (cfg.usbId == 1) {
    TinyUSBDevice.setID(0x2E8A, 0x10F0);
    TinyUSBDevice.setManufacturerDescriptor("Raspberry Pi");
    TinyUSBDevice.setProductDescriptor("vSeeBox Remote");
  } else {
    TinyUSBDevice.setID(0x1915, 0x1025);
    TinyUSBDevice.setManufacturerDescriptor("XING WEI 2.4G USB");
    TinyUSBDevice.setProductDescriptor("USB Composite Device");
  }
  TinyUSBDevice.setDeviceVersion(0x0100);
  TinyUSBDevice.setVersion(0x0110);
  TinyUSBDevice.addInterface(mic);             // interfaces 0-1
  hidKb.setDesc(descKb, sizeof(descKb));       // interface 2, EP 0x81
  hidKb.begin();
  hidMc.setDesc(descMc, sizeof(descMc));       // interface 3, EP 0x82
  hidMc.begin();
  TinyUSBDevice.detach();
  delay(30);
  TinyUSBDevice.attach();
}

// ---------------------------------------------------------------- report queues
struct KbEv { uint8_t mod; uint8_t keys[6]; uint16_t hold; };
struct McEv { uint8_t type; uint16_t val; uint16_t hold; };   // type 0 = consumer usage, 1 = mouse buttons
enum { MC_CONSUMER = 0, MC_BUTTONS = 1 };

template <typename T, int N> struct Ring {
  T buf[N]; int head = 0, count = 0;
  bool push(const T& v) { if (count >= N) return false; buf[(head + count) % N] = v; count++; return true; }
  T& front() { return buf[head]; }
  void pop() { head = (head + 1) % N; count--; }
  void clear() { head = count = 0; }
};
Ring<KbEv, 640> kbQ;
Ring<McEv, 64> mcQ;

uint8_t heldMod = 0, heldKeys[6] = {0};   // keys held with "p"
uint8_t mouseBtn = 0;                     // buttons as last queued
uint8_t sentBtn = 0;                      // buttons as last sent
long pendDx = 0, pendDy = 0, pendW = 0;
uint32_t kbNext = 0, mcNext = 0, stallSince = 0, lastWake = 0;
uint32_t stSent = 0, stDropped = 0, stWakeups = 0, stOverflow = 0;
uint32_t lastActivity = 0;

// pico-2: every report goes out more than once (a few ms apart). The vSeeBox's USB appears to drop the
// odd report; a repeated identical report is ignored by the host, so this only protects against loss.
#define DUP_GAP_MS 4
// Live-tunable (command: tm <tap_ms> <tap_gap> <type_ms> <type_gap> <media_ms> <copies>)
uint16_t tapMs = 200, tapGap = 80, typeMs = 100, typeGap = 50, mediaMs = 300; uint8_t gCopies = 1;
static void qKb(uint8_t mod, const uint8_t* keys, uint16_t hold, uint8_t copies = 0) {
  bool release = !mod && !keys[0] && !keys[1] && !keys[2] && !keys[3] && !keys[4] && !keys[5];
  if (!copies) copies = gCopies;
  (void)release;
  KbEv e; e.mod = mod; memcpy(e.keys, keys, 6);
  for (uint8_t i = 0; i < copies; i++) {
    e.hold = (i + 1 < copies) ? DUP_GAP_MS : (hold > DUP_GAP_MS * (copies - 1) ? hold - DUP_GAP_MS * (copies - 1) : DUP_GAP_MS);
    if (!kbQ.push(e)) { stOverflow++; return; }
  }
}
static void qKbHeld(uint16_t hold) { qKb(heldMod, heldKeys, hold); }
static void tapUsage(uint8_t usage, uint8_t mod, uint16_t down = 0, uint16_t up = 0) {
  if (!down) down = tapMs;
  if (!up) up = tapGap;
  uint8_t k[6]; memcpy(k, heldKeys, 6);
  bool placed = false;
  for (int i = 0; i < 6 && !placed; i++) if (k[i] == usage) placed = true;
  for (int i = 0; i < 6 && !placed; i++) if (!k[i]) { k[i] = usage; placed = true; }
  if (!placed) k[5] = usage;
  qKb(heldMod | mod, k, down);
  qKbHeld(up);
}
static void qMc(uint8_t type, uint16_t val, uint16_t hold) {
  uint8_t copies = gCopies;
  McEv e; e.type = type; e.val = val;
  for (uint8_t i = 0; i < copies; i++) {
    e.hold = (i + 1 < copies) ? DUP_GAP_MS : (hold > DUP_GAP_MS * (copies - 1) ? hold - DUP_GAP_MS * (copies - 1) : DUP_GAP_MS);
    if (!mcQ.push(e)) { stOverflow++; return; }
  }
}
static void tapConsumer(uint16_t usage) {
  qMc(MC_CONSUMER, usage, mediaMs);
  qMc(MC_CONSUMER, 0, tapGap);
}

static bool sendMouse(int8_t dx, int8_t dy, int8_t w) {
  hid_mouse_report_t r = { sentBtn, dx, dy, w, 0 };
  return hidMc.sendReport(RID_MOUSE, &r, sizeof(r));
}
static bool sendMotion() {
  int8_t dx = constrain(pendDx, -127, 127), dy = constrain(pendDy, -127, 127), w = constrain(pendW, -127, 127);
  if (!sendMouse(dx, dy, w)) return false;
  pendDx -= dx; pendDy -= dy; pendW -= w;
  stSent++;
  return true;
}

static void releaseAll() {
  kbQ.clear(); mcQ.clear();
  heldMod = 0; memset(heldKeys, 0, 6);
  mouseBtn = 0; pendDx = pendDy = pendW = 0;
  qKbHeld(5);
  qMc(MC_CONSUMER, 0, 5);
  qMc(MC_BUTTONS, 0, 5);
}

static void pumpUsb() {
  uint32_t now = millis();
  bool work = kbQ.count || mcQ.count || pendDx || pendDy || pendW;
  if (!work) { stallSince = 0; return; }
  if (!TinyUSBDevice.mounted()) {
    if (!stallSince) stallSince = now;
  } else if (TinyUSBDevice.suspended()) {
    if (!stallSince) stallSince = now;
    if (now - lastWake > 100) { lastWake = now; stWakeups++; TinyUSBDevice.remoteWakeup(); }
  } else {
    bool moved = false;
    if (kbQ.count && (int32_t)(now - kbNext) >= 0 && hidKb.ready()) {
      KbEv& e = kbQ.front();
      if (hidKb.keyboardReport(0, e.mod, e.keys)) { ulogAdd(8, 0x81, e.mod, e.keys[0], 0, 0); kbNext = now + e.hold; kbQ.pop(); stSent++; moved = true; }
    }
    if ((int32_t)(now - mcNext) >= 0 && hidMc.ready()) {
      if (mcQ.count) {
        McEv& e = mcQ.front();
        if (e.type == MC_BUTTONS && (pendDx || pendDy)) {
          moved = sendMotion();                            // land the cursor before the click
        } else if (e.type == MC_BUTTONS) {
          uint8_t old = sentBtn; sentBtn = (uint8_t)e.val;
          if (sendMouse(0, 0, 0)) { mcNext = now + e.hold; mcQ.pop(); stSent++; moved = true; } else sentBtn = old;
        } else {
          uint16_t u = e.val;
          if (hidMc.sendReport(RID_CONSUMER, &u, 2)) { ulogAdd(8, 0x82, 2, u, 0, 0); mcNext = now + e.hold; mcQ.pop(); stSent++; moved = true; }
        }
      } else if (pendDx || pendDy || pendW) {
        moved = sendMotion();
      }
    }
    if (moved) { stallSince = 0; return; }
    if (!stallSince) stallSince = now;
  }
  if (stallSince && now - stallSince > 1500) {   // the box isn't listening: don't let a backlog build up
    stDropped += kbQ.count + mcQ.count;
    kbQ.clear(); mcQ.clear(); pendDx = pendDy = pendW = 0;
    stallSince = 0;
  }
}

// ---------------------------------------------------------------- relay
uint32_t relayOffAt = 0;
static void relayWrite(bool on) { digitalWrite(RELAY_PIN, (on ^ (cfg.relayLow != 0)) ? HIGH : LOW); }
static void relayPulse(uint16_t ms) {
  if (ms < 50 || ms > 3000) ms = cfg.pulseMs;
  relayWrite(true);
  relayOffAt = millis() + ms;
  if (!relayOffAt) relayOffAt = 1;
}

// ---------------------------------------------------------------- commands
static int keyUsage(const String& n) {
  if (n == "up") return 0x52;
  if (n == "down") return 0x51;
  if (n == "left") return 0x50;
  if (n == "right") return 0x4F;
  if (n == "enter" || n == "ok") return 0x28;
  if (n == "esc") return 0x29;
  if (n == "tab") return 0x2B;
  if (n == "backspace") return 0x2A;
  if (n == "space") return 0x2C;
  if (n == "pgup") return 0x4B;
  if (n == "pgdn") return 0x4E;
  if (n == "delete") return 0x4C;
  if (n == "menu") return 0x76;
  return -1;
}
static uint16_t mediaUsage(const String& n) {
  if (n == "home") return 0x0223;
  if (n == "back") return 0x0224;
  if (n == "menu") return 0x0040;
  if (n == "play") return 0x00CD;
  if (n == "stop") return 0x00B7;
  if (n == "next") return 0x00B5;
  if (n == "prev") return 0x00B6;
  if (n == "ff") return 0x00B3;
  if (n == "rew") return 0x00B4;
  if (n == "volup") return 0x00E9;
  if (n == "voldown") return 0x00EA;
  if (n == "mute") return 0x00E2;
  if (n == "search") return 0x0221;
  if (n == "power") return 0x0030;
  if (n == "chup") return 0x009C;
  if (n == "chdn") return 0x009D;
  return 0;
}
// Arduino Keyboard.press() codes -> HID usage (+ modifier bits)
static uint8_t arduinoUsage(uint8_t k, uint8_t& mod) {
  if (k >= 136) return k - 136;
  if (k >= 128) { mod |= 1 << (k - 128); return 0; }
  if (ascii2kc[k][0]) mod |= KEYBOARD_MODIFIER_LEFTSHIFT;
  return ascii2kc[k][1];
}
static String usbState() {
  return String("mounted=") + (TinyUSBDevice.mounted() ? 1 : 0) + " suspended=" + (TinyUSBDevice.suspended() ? 1 : 0) +
         " kb_ready=" + (hidKb.ready() ? 1 : 0) + " mc_ready=" + (hidMc.ready() ? 1 : 0) +
         " id=" + (cfg.usbId == 1 ? "rpi" : "xingwei");
}

uint32_t rebootAt = 0;

String runCommand(String line) {
  line.trim();
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? "" : line.substring(sp + 1);
  rest.trim();
  lastActivity = millis();
  if (!lastActivity) lastActivity = 1;

  if (cmd == "ping") return "pong";
  if (cmd == "k") {
    if (rest == "back") { tapUsage(0x2A, 0); return "ok"; }          // the real remote's Back button sends Backspace
    if (rest == "home") { tapConsumer(0x0223); return "ok"; }
    if (kbQ.count >= 8) return "busy";      // already 4+ taps waiting: drop instead of overshooting
    int u = keyUsage(rest);
    if (u < 0 && rest.length() == 1 && (uint8_t)rest[0] < 128) {
      uint8_t mod = 0; u = arduinoUsage((uint8_t)rest[0], mod);
      if (u) { tapUsage(u, mod); return "ok"; }
    }
    if (u < 0) return "err unknown key";
    tapUsage((uint8_t)u, 0);
    return "ok";
  }
  if (cmd == "t") {
    for (unsigned i = 0; i < rest.length(); i++) {
      uint8_t c = (uint8_t)rest[i];
      if (c >= 128) continue;
      uint8_t mod = 0, u = arduinoUsage(c, mod);
      if (u) tapUsage(u, mod, typeMs, typeGap);
    }
    return "ok";
  }
  if (cmd == "p" || cmd == "r") {
    uint8_t mod = 0, u = arduinoUsage((uint8_t)rest.toInt(), mod);
    if (cmd == "p") {
      heldMod |= mod;
      bool have = false;
      for (int i = 0; i < 6; i++) if (u && heldKeys[i] == u) have = true;
      for (int i = 0; i < 6 && u && !have; i++) if (!heldKeys[i]) { heldKeys[i] = u; have = true; }
    } else {
      heldMod &= ~mod;
      for (int i = 0; i < 6; i++) if (u && heldKeys[i] == u) heldKeys[i] = 0;
    }
    qKbHeld(10);
    return "ok";
  }
  if (cmd == "x") { releaseAll(); return "ok"; }
  if (cmd == "m" && rest == "back") { tapUsage(0x2A, 0); return "ok"; }
  if (cmd == "m" && rest == "aback") { tapConsumer(0x0224); return "ok"; }
  if (cmd == "tm") {
    long v[6] = {tapMs, tapGap, typeMs, typeGap, mediaMs, gCopies};
    int i = 0, pos = 0;
    while (i < 6 && pos < (int)rest.length()) {
      int sp2 = rest.indexOf(' ', pos); if (sp2 < 0) sp2 = rest.length();
      String f = rest.substring(pos, sp2); f.trim();
      if (f.length()) v[i++] = f.toInt();
      pos = sp2 + 1;
    }
    tapMs = constrain(v[0], 10, 2000); tapGap = constrain(v[1], 5, 2000); typeMs = constrain(v[2], 10, 2000);
    typeGap = constrain(v[3], 5, 2000); mediaMs = constrain(v[4], 10, 2000); gCopies = constrain(v[5], 1, 4);
    return String("tm ") + tapMs + " " + tapGap + " " + typeMs + " " + typeGap + " " + mediaMs + " " + gCopies;
  }
  if (cmd == "m") {
    uint16_t u = mediaUsage(rest);
    if (!u) return "err unknown media key";
    tapConsumer(u);
    return "ok";
  }
  if (cmd == "mk") {             // media keys as keyboard keys (what the Nano v8 sent)
    if (rest == "back") tapUsage(0x29, 0);
    else if (rest == "home") tapUsage(0x28, KEYBOARD_MODIFIER_LEFTGUI);
    else if (rest == "menu") tapUsage(0x76, 0);
    else if (rest == "play") tapUsage(0xE8, 0);
    else if (rest == "volup") tapUsage(0x80, 0);
    else if (rest == "voldown") tapUsage(0x81, 0);
    else if (rest == "mute") tapUsage(0x7F, 0);
    else return "err unknown media key";
    return "ok";
  }
  if (cmd == "w") { pendW = constrain(pendW + rest.toInt(), -500, 500); return "ok"; }
  if (cmd == "rel") {
    int s2 = rest.indexOf(' ');
    if (s2 < 0) return "err need dx dy";
    pendDx = constrain(pendDx + rest.substring(0, s2).toInt(), -4000, 4000);
    pendDy = constrain(pendDy + rest.substring(s2 + 1).toInt(), -4000, 4000);
    return "ok";
  }
  if (cmd == "rb" || cmd == "b") {
    int s2 = rest.indexOf(' ');
    if (s2 < 0) return "err need button state";
    int b = rest.substring(0, s2).toInt();
    uint8_t m = b == 2 ? MOUSE_BUTTON_RIGHT : b == 3 ? MOUSE_BUTTON_MIDDLE : MOUSE_BUTTON_LEFT;
    if (rest.substring(s2 + 1).toInt()) mouseBtn |= m; else mouseBtn &= ~m;
    qMc(MC_BUTTONS, mouseBtn, 8);
    return "ok";
  }
  if (cmd == "sw") { relayPulse(rest.length() ? rest.toInt() : cfg.pulseMs); return "ok"; }
  if (cmd == "usb") return usbState();
  if (cmd == "stats") { stSent = stDropped = stWakeups = stOverflow = 0; return "ok stats reset"; }
  if (cmd == "reboot") { rebootAt = millis() + 300; return "ok restarting"; }
  if (cmd == "a" || cmd == "c") return "err absolute pointer not supported";
  return "err unknown command";
}

// Checks the token, then runs each line. Returns the last reply.
String handlePacket(const String& raw) {
  if (!cfg.token[0]) return "err not set up";
  String tok = String(cfg.token) + " ";
  String s = raw;
  while (s.length() && (s[0] == ' ' || s[0] == '\r' || s[0] == '\n')) s.remove(0, 1);
  if (!s.startsWith(tok)) return "err bad token";
  String body = s.substring(tok.length()), reply = "ok";
  int start = 0;
  while (start <= (int)body.length()) {
    int nl = body.indexOf('\n', start);
    if (nl < 0) nl = body.length();
    String one = body.substring(start, nl);
    one.trim();
    if (one.length()) reply = runCommand(one);
    start = nl + 1;
  }
  return reply;
}

// ---------------------------------------------------------------- network
WiFiUDP udp;
WebServer web(80);
DNSServer dns;
HTTPUpdateServer updater;
bool portal = false;
uint32_t portalUntil = 0;

static String esc(const String& s) {
  String o; o.reserve(s.length() + 8);
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

static const char CSS[] = "<meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><style>"
  "body{font-family:system-ui,sans-serif;background:#121417;color:#e7e9ed;max-width:440px;margin:24px auto;padding:0 16px}"
  "h1{font-size:20px}label{display:block;margin-top:12px;font-size:14px;color:#9aa1ad}"
  "input,select{width:100%;padding:10px;margin:4px 0;box-sizing:border-box;border-radius:8px;border:1px solid #444;background:#1b1e23;color:inherit;font-size:15px}"
  "input[type=checkbox]{width:auto}.g{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:12px 0}"
  "button{padding:14px;border-radius:10px;border:1px solid #3a3f47;background:#1b1e23;color:inherit;font-size:15px;width:100%}"
  ".p{background:#2d6cdf;border-color:#2d6cdf;margin-top:16px}a{color:#7aa7ff}small,#log{color:#9aa1ad}</style>";

static bool needAuth() {
  if (portal) return false;
  if (web.authenticate("admin", cfg.token)) return false;
  web.requestAuthentication(BASIC_AUTH, "vSeeBox remote", "User: admin   Password: your token");
  return true;
}

static void pageSetup() {
  if (needAuth()) return;
  String h = String("<!doctype html><html><head><title>vSeeBox remote setup</title>") + CSS + "</head><body>"
    "<h1>vSeeBox remote · setup</h1><form method=post action=/setup>"
    "<label>Wi-Fi name</label><input name=ssid required maxlength=32 autocapitalize=off autocorrect=off spellcheck=false value=\"" + esc(cfg.ssid) + "\">"
    "<label>Wi-Fi password" + (cfg.pass[0] ? " (leave blank to keep the saved one)" : "") + "</label>"
    "<input name=pass type=password maxlength=64>"
    "<label>Token (same as ESP_TOKEN on the control page server)</label>"
    "<input name=token required maxlength=64 autocapitalize=off autocorrect=off spellcheck=false value=\"" + esc(cfg.token) + "\">"
    "<label>Device name</label><input name=host maxlength=32 value=\"" + esc(cfg.host) + "\">"
    "<label>USB identity</label><select name=usbid>"
    "<option value=1" + (cfg.usbId == 1 ? " selected" : "") + ">Raspberry Pi (generic) (recommended)</option>"
    "<option value=0" + (cfg.usbId == 0 ? " selected" : "") + ">XING WEI 2.4G dongle look-alike</option></select>"
    "<label>Relay press length (ms)</label><input name=pulse type=number min=50 max=3000 value=" + String(cfg.pulseMs) + ">"
    "<label><input type=checkbox name=relaylow value=1" + (cfg.relayLow ? " checked" : "") + "> Relay board switches on LOW (only if the relay is stuck on)</label>"
    "<button class=p type=submit>Save and restart</button></form>"
    "<p><small>Firmware " FW_VERSION ". Hold BOOTSEL 5 s to wipe settings.</small></p></body></html>";
  web.send(200, "text/html", h);
}

static void saveSetup() {
  if (needAuth()) return;
  String ssid = web.arg("ssid"), pass = web.arg("pass"), token = web.arg("token"), host = web.arg("host");
  ssid.trim(); token.trim(); host.trim();
  if (!ssid.length() || !token.length() || token.indexOf(' ') >= 0) {
    web.send(400, "text/plain", "Wi-Fi name and token are required, and the token can't contain spaces.");
    return;
  }
  host.toLowerCase();
  String h2;
  for (unsigned i = 0; i < host.length(); i++) { char c = host[i]; if (isalnum(c) || c == '-') h2 += c; }
  if (!h2.length()) h2 = "vseebox-remote";
  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  if (pass.length()) strlcpy(cfg.pass, pass.c_str(), sizeof(cfg.pass));
  strlcpy(cfg.token, token.c_str(), sizeof(cfg.token));
  strlcpy(cfg.host, h2.c_str(), sizeof(cfg.host));
  cfg.usbId = web.arg("usbid").toInt() == 1 ? 1 : 0;
  cfg.relayLow = web.hasArg("relaylow") ? 1 : 0;
  int p = web.arg("pulse").toInt();
  cfg.pulseMs = (p >= 50 && p <= 3000) ? p : 250;
  saveCfg();
  web.send(200, "text/html", String("<!doctype html><html><head>") + CSS + "</head><body><h1>Saved</h1>"
    "<p>Restarting and joining <b>" + esc(cfg.ssid) + "</b>. The light on the Pico goes solid once it's on your Wi-Fi.</p>"
    "<p>Find it in UniFi as <b>" + esc(cfg.host) + "</b>.</p></body></html>");
  rebootAt = millis() + 1500;
}

static const char REMOTE[] = R"HTML(<h1>vSeeBox remote</h1><div id=st><small>…</small></div>
<input id=tok placeholder="Token" type=password autocapitalize=off autocorrect=off spellcheck=false>
<div class=g>
<button data-c="m back">Back</button><button data-c="k up">▲</button><button data-c="m home">Home</button>
<button data-c="k left">◀</button><button data-c="k ok">OK</button><button data-c="k right">▶</button>
<button data-c="m voldown">Vol −</button><button data-c="k down">▼</button><button data-c="m volup">Vol +</button>
<button data-c="m prev">⏮</button><button data-c="m play">⏯</button><button data-c="m next">⏭</button>
<button data-c="sw">Anker switch</button><button data-c="x">Unstick</button><button data-c="usb">USB?</button>
</div>
<input id=txt placeholder="Type text, then Enter"><div id=log></div>
<p><a href=/setup>Settings</a> · <a href=/update>Firmware update</a> · <a href=/status>Status JSON</a></p>
<script>
const tok=document.getElementById('tok'),log=document.getElementById('log');
try{tok.value=localStorage.getItem('tok')||''}catch(e){}
tok.onchange=()=>{try{localStorage.setItem('tok',tok.value)}catch(e){}};
async function send(c){if(!tok.value.trim()){log.textContent='Enter the token at the top first';tok.focus();return}try{const r=await fetch('/cmd?q='+encodeURIComponent(tok.value+' '+c));log.textContent=c+' → '+await r.text()}catch(e){log.textContent='error: '+e}}
document.querySelectorAll('button').forEach(b=>b.onclick=()=>send(b.dataset.c));
document.getElementById('txt').onkeydown=e=>{if(e.key==='Enter'){send('t '+e.target.value);e.target.value=''}};
fetch('/status').then(r=>r.json()).then(j=>{document.getElementById('st').innerHTML='<small>'+j.fw+' · '+j.ip+' · Wi-Fi '+j.rssi+' dBm · USB '+(j.usb.mounted?'connected':'not connected')+'</small>'});
</script>)HTML";

static void pageRoot() {
  if (portal) { pageSetup(); return; }
  web.send(200, "text/html", String("<!doctype html><html><head><title>vSeeBox remote</title>") + CSS + "</head><body>" + REMOTE + "</body></html>");
}

static void pageStatus() {
  String j = String("{\"fw\":\"" FW_VERSION "\",\"host\":\"") + cfg.host + "\",\"ip\":\"" + WiFi.localIP().toString() +
    "\",\"rssi\":" + String(WiFi.RSSI()) + ",\"uptime_s\":" + String(millis() / 1000) + ",\"udp_port\":" + String(UDP_PORT) +
    ",\"usb\":{\"id\":\"" + (cfg.usbId == 1 ? "rpi" : "xingwei") + "\",\"mounted\":" + (TinyUSBDevice.mounted() ? "true" : "false") +
    ",\"suspended\":" + (TinyUSBDevice.suspended() ? "true" : "false") +
    ",\"kb_ready\":" + (hidKb.ready() ? "true" : "false") + ",\"mc_ready\":" + (hidMc.ready() ? "true" : "false") +
    ",\"sent\":" + String(stSent) + ",\"dropped\":" + String(stDropped) + ",\"overflow\":" + String(stOverflow) +
    ",\"wakeups\":" + String(stWakeups) + ",\"queued\":[" + String(kbQ.count) + "," + String(mcQ.count) + "]" +
    ",\"pending\":[" + String(pendDx) + "," + String(pendDy) + "," + String(pendW) + "]}" +
    ",\"relay\":{\"pin\":" + String(RELAY_PIN) + ",\"active_low\":" + (cfg.relayLow ? "true" : "false") +
    ",\"pulse_ms\":" + String(cfg.pulseMs) + "}}";
  web.send(200, "application/json", j);
}

static void setupWeb() {
  web.on("/", pageRoot);
  web.on("/setup", HTTP_GET, pageSetup);
  web.on("/setup", HTTP_POST, saveSetup);
  web.on("/status", pageStatus);
  web.on("/usblog", []() {
    String o; o.reserve(9000);
    o += "now_ms " + String(millis()) + "\ncounts reset=" + String(ucnt[1]) + " unplug=" + String(ucnt[2]) + " suspend=" + String(ucnt[3]) +
         " resume=" + String(ucnt[4]) + " xfer_err=" + String(ucnt[5]) + " ep0_stall=" + String(ucnt[6]) + " picked_up=" + String(ucnt[7]) +
         " queued=" + String(ucnt[8]) + " setup=" + String(ucnt[9]) + "\n";
    uint32_t h = ulogHead, n = h < ULOG_N ? h : ULOG_N;
    bool skipPick = web.hasArg("nopick");
    for (uint32_t k = h - n; k < h; k++) {
      volatile ULog& L = ulog[k % ULOG_N];
      if (skipPick && L.type == 7) continue;
      char line[96];
      if (L.type == 9) snprintf(line, sizeof line, "%lu setup rt=%02x req=%02x val=%04x idx=%04x len=%u\n", (unsigned long)L.ms, L.a, L.b, L.c, L.d, L.e);
      else if (L.type == 8) snprintf(line, sizeof line, "%lu queued ep=%02x mod=%02x key=%04x\n", (unsigned long)L.ms, L.a, L.b, L.c);
      else if (L.type == 7) snprintf(line, sizeof line, "%lu picked_up ep=%02x\n", (unsigned long)L.ms, L.a);
      else snprintf(line, sizeof line, "%lu event %u v=%02x%02x\n", (unsigned long)L.ms, L.type, L.b, L.a);
      o += line;
    }
    web.send(200, "text/plain", o);
  });
  web.on("/cmd", []() {
    if (portal) { web.send(503, "text/plain", "err in setup mode"); return; }
    String r = handlePacket(web.arg("q"));
    web.send(r.startsWith("err bad token") ? 403 : 200, "text/plain", r);
  });
  web.onNotFound([]() {
    if (portal) {          // captive portal: send every phone/PC "am I online?" check to the setup page
      web.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/setup", true);
      web.send(302, "text/plain", "");
    } else {
      web.send(404, "text/plain", "not found");
    }
  });
  if (!portal) updater.setup(&web, "/update", "admin", cfg.token);
  web.begin();
}

static void startPortal(uint32_t forMs) {
  portal = true;
  portalUntil = forMs ? millis() + forMs : 0;
  WiFi.disconnect();
  WiFi.mode(WIFI_AP);
  WiFi.softAP(SETUP_SSID);
  delay(200);
  dns.start(53, "*", WiFi.softAPIP());
}

static bool joinWiFi(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(cfg.host);
  WiFi.begin(cfg.ssid, cfg.pass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) {
    digitalWrite(LED_BUILTIN, (millis() / 150) % 2);
    delay(50);
  }
  return WiFi.status() == WL_CONNECTED;
}

// ---------------------------------------------------------------- main
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  loadCfg();
  relayWrite(false);
  startUsb();
  pinMode(LED_BUILTIN, OUTPUT);

  if (!haveCfg) {
    startPortal(0);                      // no settings yet: stay in setup until saved
  } else {
    // pico-5: never drop into setup just because the first join was slow (that happened after every
    // firmware update). Keep retrying in the background; setup only opens after 10 minutes with no Wi-Fi.
    joinWiFi(30000);
    udp.begin(UDP_PORT);
  }
  setupWeb();
}

void loop() {
  uint32_t now = millis();

  if (portal) {
    dns.processNextRequest();
    digitalWrite(LED_BUILTIN, (now / 700) % 2);
    if (portalUntil && (int32_t)(now - portalUntil) > 0) rp2040.reboot();
  } else {
    int n;
    while ((n = udp.parsePacket()) > 0) {
      static char buf[1200];
      int len = udp.read(buf, sizeof(buf) - 1);
      buf[len > 0 ? len : 0] = 0;
      String reply = handlePacket(String(buf));
      if (reply != "ok") {                          // only answer pings, questions and errors
        udp.beginPacket(udp.remoteIP(), udp.remotePort());
        udp.print(reply);
        udp.endPacket();
      }
      pumpUsb();
    }
    static bool mdnsUp = false;
    static uint32_t lastTry = 0, offlineSince = 0;
    if (WiFi.status() != WL_CONNECTED) {
      digitalWrite(LED_BUILTIN, (now / 150) % 2);
      if (!offlineSince) offlineSince = now ? now : 1;
      if (now - lastTry > 15000) { lastTry = now; WiFi.begin(cfg.ssid, cfg.pass); }
      if (now - offlineSince > 10UL * 60 * 1000) startPortal(5UL * 60 * 1000);   // Wi-Fi changed? offer setup, then retry
    } else {
      offlineSince = 0;
      digitalWrite(LED_BUILTIN, HIGH);
      if (!mdnsUp) { mdnsUp = true; MDNS.begin(cfg.host); MDNS.addService("http", "tcp", 80); }
      MDNS.update();
    }
  }

  web.handleClient();
  pumpUsb();

  if (relayOffAt && (int32_t)(now - relayOffAt) >= 0) { relayWrite(false); relayOffAt = 0; }

  // Safety: something held and nothing has happened for 10 s -> let go.
  if (lastActivity && now - lastActivity > 10000) {
    if (heldMod || heldKeys[0] || heldKeys[1] || heldKeys[2] || heldKeys[3] || heldKeys[4] || heldKeys[5] || mouseBtn) releaseAll();
    lastActivity = 0;
  }

  // Hold BOOTSEL for 5 s: wipe settings and restart into setup.
  static uint32_t bootselSince = 0, lastPoll = 0;
  if (now - lastPoll > 200) {
    lastPoll = now;
    if (BOOTSEL) {
      if (!bootselSince) bootselSince = now;
      else if (now - bootselSince > 5000) { wipeCfg(); rp2040.reboot(); }
    } else bootselSince = 0;
  }

  if (rebootAt && (int32_t)(now - rebootAt) >= 0) { releaseAll(); rp2040.reboot(); }
}
