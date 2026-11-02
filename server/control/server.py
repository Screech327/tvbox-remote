#!/usr/bin/env python3
"""TV box control page (tvbox-remote).

Serves one page that shows the live stream and turns clicks, keys and buttons into
commands for the Pico 2 W plugged into the TV box.

  GET  /             the page
  POST /api/cmd      body = commands, one per line  -> one UDP packet to the Pico
  GET  /api/status   pings the Pico, reports round-trip time
  POST /whep         WebRTC offer, passed through to MediaMTX (keeps the page on one origin)

Settings come from /etc/vseebox-control.env (see install.sh). Standard library only.
"""
import html
import json
import os
import socket
import threading
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
ENV_FILE = os.environ.get("VSEEBOX_ENV", "/etc/vseebox-control.env")
ALLOWED = {"k", "t", "p", "r", "x", "m", "mk", "b", "w", "rel", "rb", "ping", "usb", "sw"}


def load_env(path):
    env = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                env[k.strip()] = v.strip().strip('"').strip("'")
    except FileNotFoundError:
        pass
    return env


CFG = load_env(ENV_FILE)
ESP_HOST = CFG.get("ESP_HOST", "")
ESP_PORT = int(CFG.get("ESP_PORT", "4210"))
TOKEN = CFG.get("ESP_TOKEN", "")
LISTEN = int(CFG.get("PORT", "80"))
WHEP = CFG.get("WHEP_URL", "http://127.0.0.1:8889/vseebox/whep")
BOX_NAME = CFG.get("BOX_NAME", "TV Box")   # shown on the page and on the phone home-screen icon

_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
_lock = threading.Lock()


def configured():
    return bool(ESP_HOST and TOKEN and TOKEN != "change-me-to-something-random")


def clean(lines):
    """Keep only known commands, one per line, no control characters."""
    out = []
    for raw in lines:
        line = "".join(ch for ch in raw if ch >= " " and ch != "\x7f").strip()
        if not line or len(line) > 200:
            continue
        if line.split(" ", 1)[0] in ALLOWED:
            out.append(line)
    return out[:64]


def send(cmds):
    """Send commands to the Pico in one packet (token once, then one command per line)."""
    payload = (TOKEN + " " + "\n".join(cmds)).encode("utf-8", "ignore")[:1000]
    with _lock:
        _sock.sendto(payload, (ESP_HOST, ESP_PORT))


def ping():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.8)
    try:
        t0 = time.monotonic()
        s.sendto((TOKEN + " ping").encode(), (ESP_HOST, ESP_PORT))
        data, _ = s.recvfrom(256)
        ms = (time.monotonic() - t0) * 1000
        reply = data.decode(errors="ignore")
        return {"ok": reply == "pong", "ms": round(ms, 1), "reply": reply}
    except (socket.timeout, OSError) as e:
        return {"ok": False, "ms": None, "reply": str(e) or "no answer"}
    finally:
        s.close()


class Handler(BaseHTTPRequestHandler):
    server_version = "vseebox-control"

    def log_message(self, fmt, *args):  # quiet: only log errors
        pass

    def _reply(self, code, body, ctype="application/json", extra=None):
        if isinstance(body, (dict, list)):
            body = json.dumps(body)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _body(self, limit=200_000):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(min(n, limit))

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                page = f.read()
            if BOX_NAME != "TV Box":
                name = html.escape(BOX_NAME).encode()
                page = page.replace(b"<title>TV Box Control</title>", b"<title>" + name + b" Control</title>")
                page = page.replace(b"<h1>TV Box</h1>", b"<h1>" + name + b"</h1>")
            return self._reply(200, page, "text/html; charset=utf-8")
        if path == "/api/status":
            if not configured():
                return self._reply(200, {"configured": False, "esp": {"ok": False, "ms": None,
                                         "reply": "ESP_HOST / ESP_TOKEN not set in " + ENV_FILE}})
            return self._reply(200, {"configured": True, "host": ESP_HOST, "esp": ping()})
        if path == "/manifest.json":   # lets phones "Add to Home screen" and open it full screen, no browser bar
            return self._reply(200, {"name": BOX_NAME, "short_name": BOX_NAME, "start_url": "/", "display": "fullscreen",
                                     "orientation": "landscape", "background_color": "#000000", "theme_color": "#0e1013"},
                               "application/manifest+json")
        if path == "/healthz":
            return self._reply(200, "ok", "text/plain")
        self._reply(404, {"error": "not found"})

    def do_POST(self):
        path = self.path.split("?", 1)[0]
        if path == "/api/cmd":
            if not configured():
                return self._reply(503, {"error": "Pico not set up yet (edit " + ENV_FILE + ")"})
            cmds = clean(self._body(8000).decode("utf-8", "ignore").split("\n"))
            if not cmds:
                return self._reply(400, {"error": "no valid commands"})
            try:
                send(cmds)
            except OSError as e:
                return self._reply(502, {"error": str(e)})
            return self._reply(200, {"sent": len(cmds)})
        if path == "/whep":
            req = urllib.request.Request(WHEP, data=self._body(), method="POST",
                                         headers={"Content-Type": "application/sdp"})
            try:
                with urllib.request.urlopen(req, timeout=10) as r:
                    return self._reply(r.status, r.read(), "application/sdp")
            except urllib.error.HTTPError as e:
                return self._reply(e.code, e.read() or b"stream error", "text/plain")
            except (urllib.error.URLError, OSError) as e:
                return self._reply(502, "MediaMTX not reachable: %s" % e, "text/plain")
        self._reply(404, {"error": "not found"})


if __name__ == "__main__":
    print("vseebox-control on :%d -> Pico %s:%d (%s)" % (
        LISTEN, ESP_HOST or "?", ESP_PORT, "ready" if configured() else "NOT CONFIGURED"), flush=True)
    ThreadingHTTPServer(("", LISTEN), Handler).serve_forever()
