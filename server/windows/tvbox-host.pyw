"""tvbox-remote host for Windows.

Normal run (pythonw.exe tvbox-host.pyw, started at log-on with no window) does what the two systemd services do
on Linux:
  - runs MediaMTX and the ffmpeg capture, restarting either one if it stops
  - runs the control page server (server.py) in this process

Setup (python.exe tvbox-host.pyw --setup, run by install.ps1): finds the capture card, picks a video format and
an encoder, asks for the Pico's IP and token, and writes tvbox.env.

Everything lives in the folder this file is in. Settings: tvbox.env. Logs: logs\\.
"""
import os
import re
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ENV_FILE = os.path.join(HERE, "tvbox.env")
LOGS = os.path.join(HERE, "logs")
FFMPEG = os.path.join(HERE, "ffmpeg", "ffmpeg.exe")
MEDIAMTX = os.path.join(HERE, "mediamtx", "mediamtx.exe")
NO_WINDOW = getattr(subprocess, "CREATE_NO_WINDOW", 0)
CFG = {}


# ------------------------------------------------------------------ settings

def load_env(path):
    env = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if line and not line.startswith("#") and "=" in line:
                    k, v = line.split("=", 1)
                    env[k.strip()] = v.strip().strip('"')
    except FileNotFoundError:
        pass
    return env


def fps_value(fps):
    """'60', '59.94' or '10000000/166833' -> float."""
    if "/" in fps:
        a, b = fps.split("/", 1)
        return float(a) / float(b)
    return float(fps)


# ------------------------------------------------------------------ the ffmpeg pipeline

def encoder_args(enc, bitrate, gop, cfg=None):
    cfg = CFG if cfg is None else cfg
    mb = int("".join(c for c in bitrate if c.isdigit()) or 8)
    rate = ["-b:v", bitrate, "-maxrate", bitrate, "-bufsize", "%dM" % max(1, mb // 2), "-g", str(gop)]
    if enc == "nvenc":     # NVIDIA
        return ["-c:v", "h264_nvenc", "-preset", "p1", "-tune", "ull", "-zerolatency", "1", "-delay", "0",
                "-rc", "cbr", "-profile:v", "baseline", "-bf", "0", "-pix_fmt", "yuv420p"] + rate
    if enc == "qsv":       # Intel Quick Sync
        return ["-c:v", "h264_qsv", "-preset", "veryfast", "-look_ahead", "0", "-async_depth", "1",
                "-low_delay_brc", "1", "-profile:v", cfg.get("QSV_PROFILE", "baseline"), "-bf", "0",
                "-pix_fmt", "nv12"] + rate
    if enc == "amf":       # AMD
        return ["-c:v", "h264_amf", "-usage", "ultralowlatency", "-quality", "speed", "-rc", "cbr",
                "-profile:v", "constrained_baseline", "-bf", "0", "-pix_fmt", "yuv420p"] + rate
    # CPU (same as the Linux stream)
    return ["-c:v", "libx264", "-preset", cfg.get("PRESET", "superfast"), "-tune", "zerolatency",
            "-profile:v", "baseline", "-pix_fmt", "yuv420p", "-keyint_min", str(max(1, gop // 2)),
            "-sc_threshold", "0", "-x264-params", "rc-lookahead=0:sync-lookahead=0"] + rate


def ffmpeg_cmd(cfg=None, ffmpeg=FFMPEG):
    """Same pipeline as server/stream/camstream-run on Linux, with a DirectShow input."""
    cfg = CFG if cfg is None else cfg
    fps = cfg.get("FPS", "60")
    gop = int(round(fps_value(fps))) * 2
    size = cfg.get("SIZE", "1920x1080")
    audio = cfg.get("AUDIO", "none")
    test = cfg.get("VIDEO") == "testpattern"
    args = [ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostdin", "-fflags", "nobuffer", "-flags", "low_delay"]
    if test:
        args += ["-re", "-f", "lavfi", "-i", "testsrc2=size=%s:rate=%s" % (size, fps),
                 "-re", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000"]
        amap = "1:a"
    else:
        # Small real-time buffer: if the encoder ever falls behind, drop frames instead of building up lag.
        args += ["-f", "dshow", "-rtbufsize", "32M", "-thread_queue_size", "8", "-video_size", size, "-framerate", fps]
        fmt = cfg.get("INPUT_FORMAT", "")
        if fmt.startswith("vcodec="):
            args += ["-vcodec", fmt.split("=", 1)[1]]
        elif fmt.startswith("pixel_format="):
            args += ["-pixel_format", fmt.split("=", 1)[1]]
        dev = "video=" + cfg["VIDEO"]
        if audio != "none":
            args += ["-audio_buffer_size", "50"]    # DirectShow's default audio buffer adds ~0.5 s of delay
            dev += ":audio=" + audio
        args += ["-i", dev]
        amap = "0:a"
    args += ["-map", "0:v"] + encoder_args(cfg.get("ENCODER", "x264"), cfg.get("BITRATE", "8M"), gop, cfg)
    if audio != "none" or test:
        args += ["-map", amap, "-c:a", "libopus", "-b:a", "128k", "-ar", "48000", "-ac", "2"]
    args += ["-flush_packets", "1", "-f", "rtsp", "-rtsp_transport", "tcp", "rtsp://127.0.0.1:8554/vseebox"]
    return args


# ------------------------------------------------------------------ setup: find the card, format, encoder

DEV_RE = re.compile(r'"(.+)" \((video|audio)[^)]*\)\s*$')
OPT_RE = re.compile(r'(pixel_format|vcodec)=(\S+)\s+min s=(\d+)x(\d+) fps=([\d.e+]+) max s=(\d+)x(\d+) fps=([\d.e+]+)')
CARD_WORDS = re.compile(r"cam ?link|capture|hdmi|usb ?3|uvc|elgato|ugreen|guermok|ms21|video", re.I)
AUDIO_WORDS = re.compile(r"cam ?link|capture|hdmi|digital audio interface|usb ?3|ms21|elgato|ugreen|guermok", re.I)


def run(args, timeout=60):
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                       timeout=timeout, creationflags=NO_WINDOW)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def parse_devices(text):
    """ffmpeg -list_devices output -> [(name, 'video'|'audio')]"""
    out = []
    for line in text.splitlines():
        m = DEV_RE.search(line)
        if m and "Alternative name" not in line:
            out.append((m.group(1), m.group(2)))
    return out


def parse_options(text):
    out = []
    for m in OPT_RE.finditer(text):
        kind, val, w0, h0, f0, w1, h1, f1 = m.groups()
        out.append({"kind": kind, "value": val, "min": (int(w0), int(h0)), "max": (int(w1), int(h1)),
                    "fps": float(f1)})
    return out


def pick_format(opts, w=1920, h=1080):
    """Best way to get w x h: 60 fps if possible, else 30. Uncompressed beats MJPEG.
    Returns (INPUT_FORMAT, FPS) or None. FPS is an exact frame interval, so DirectShow accepts it."""
    rank = {"nv12": 0, "yuyv422": 1, "yuv420p": 2}
    for want in (59.5, 29.5):
        best = None
        for o in opts:
            if not (o["min"][0] <= w <= o["max"][0] and o["min"][1] <= h <= o["max"][1]) or o["fps"] < want:
                continue
            score = rank.get(o["value"], 5) if o["kind"] == "pixel_format" else 10
            if best is None or score < best[0]:
                best = (score, o)
        if best:
            o = best[1]
            interval = int(round(1e7 / o["fps"]))
            if want < 59.5:
                interval = max(interval, 333333)            # don't ask for more than 30 fps in this branch
            return "%s=%s" % (o["kind"], o["value"]), "10000000/%d" % interval
    return None


def choose(prompt, items, default):
    for i, it in enumerate(items, 1):
        print("   %d) %s%s" % (i, it, "   <- best guess" if i - 1 == default else ""))
    while True:
        a = input("%s [%d]: " % (prompt, default + 1)).strip()
        if not a:
            return default
        if a.isdigit() and 1 <= int(a) <= len(items):
            return int(a) - 1


def probe_encoder(enc, cfg):
    args = [FFMPEG, "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i", "testsrc2=size=1920x1080:rate=60",
            "-frames:v", "60"] + encoder_args(enc, "8M", 120, cfg) + ["-f", "null", "-"]
    try:
        return run(args, 60)[0] == 0
    except subprocess.TimeoutExpired:
        return False


def pico_ping(ip, token):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(1.0)
    try:
        s.sendto((token + " ping").encode(), (ip, 4210))
        return s.recvfrom(256)[0].decode(errors="ignore")
    except OSError as e:
        return str(e) or "no answer"
    finally:
        s.close()


def setup(argv):
    def arg(name, default=None):
        return argv[argv.index(name) + 1] if name in argv and argv.index(name) + 1 < len(argv) else default

    old = load_env(ENV_FILE)
    cfg = {"SIZE": "1920x1080", "BITRATE": old.get("BITRATE", "8M"), "PRESET": old.get("PRESET", "superfast")}

    print("\n== Capture card")
    if "--test-pattern" in argv:
        cfg.update(VIDEO="testpattern", AUDIO="none", FPS="60", INPUT_FORMAT="")
        print("   Test pattern instead of a capture card (for trying it out).")
    else:
        devs = parse_devices(run([FFMPEG, "-hide_banner", "-list_devices", "true", "-f", "dshow", "-i", "dummy"])[1])
        videos = [n for n, k in devs if k == "video"]
        audios = [n for n, k in devs if k == "audio"]
        if not videos:
            sys.exit("   No video devices found. Plug the capture card into a USB 3 port, wait a few seconds and "
                     "run the installer again.\n   Also check Settings > Privacy & security > Camera > "
                     "'Let desktop apps access your camera' is On.")
        guess = next((i for i, n in enumerate(videos) if CARD_WORDS.search(n)), 0)
        v = videos[choose("   Which one is the capture card?", videos, guess) if len(videos) > 1 else guess]
        print("   Video: %s" % v)
        rc, text = run([FFMPEG, "-hide_banner", "-list_options", "true", "-f", "dshow", "-i", "video=" + v])
        picked = pick_format(parse_options(text))
        if not picked:
            sys.exit("   %s doesn't offer 1920x1080. Supported formats:\n%s" % (v, text))
        cfg.update(VIDEO=v, INPUT_FORMAT=picked[0], FPS=picked[1])
        print("   Format: %s, %.0f fps" % (picked[0].split("=")[1], fps_value(picked[1])))

        print("\n== Sound")
        choices = audios + ["No sound"]
        aguess = next((i for i, n in enumerate(audios) if v.lower() in n.lower()), None)
        if aguess is None:
            aguess = next((i for i, n in enumerate(audios) if AUDIO_WORDS.search(n)), len(audios))
        a = choose("   Which one is the capture card's sound?", choices, aguess)
        cfg["AUDIO"] = "none" if a == len(audios) else audios[a]
        print("   Sound: %s" % cfg["AUDIO"])

    print("\n== Video encoder (graphics card if possible)")
    cfg["ENCODER"] = "x264"
    for enc, label in (("nvenc", "NVIDIA"), ("qsv", "Intel"), ("amf", "AMD")):
        ok = probe_encoder(enc, cfg)
        if not ok and enc == "qsv":
            cfg["QSV_PROFILE"] = "main"                 # some Intel chips don't do baseline
            ok = probe_encoder(enc, cfg)
            if not ok:
                cfg.pop("QSV_PROFILE")
        print("   %-7s %s" % (label, "works" if ok else "not available"))
        if ok:
            cfg["ENCODER"] = enc
            break
    if cfg["ENCODER"] == "x264":
        print("   Using the CPU (x264). Fine on most desktop CPUs; set FPS=30 in tvbox.env if it struggles.")

    print("\n== Pico 2 W")
    ip = arg("--pico") or old.get("ESP_HOST") or input("   The Pico's IP address (from your router): ").strip()
    token = arg("--token") or old.get("ESP_TOKEN") or input("   The Pico's token: ").strip()
    reply = pico_ping(ip, token)
    print("   " + ("Pico answered." if reply == "pong" else
                   "No answer from the Pico (%s). Saving anyway; check the IP/token later." % reply))
    cfg.update(ESP_HOST=ip, ESP_TOKEN=token, ESP_PORT="4210",
               BOX_NAME=arg("--name") or old.get("BOX_NAME", "TV Box"),
               PORT=arg("--port") or old.get("PORT", "80"),
               WHEP_URL="http://127.0.0.1:8889/vseebox/whep")

    with open(ENV_FILE, "w", encoding="utf-8") as f:
        f.write("# tvbox-remote settings. After editing, restart it: install.ps1 -Restart (or log off and on).\n")
        f.write("# VIDEO/AUDIO are DirectShow device names (VIDEO=testpattern for a test picture).\n")
        for k in ("VIDEO", "AUDIO", "INPUT_FORMAT", "SIZE", "FPS", "ENCODER", "QSV_PROFILE", "BITRATE", "PRESET",
                  "ESP_HOST", "ESP_TOKEN", "ESP_PORT", "PORT", "BOX_NAME", "WHEP_URL"):
            if k in cfg:
                f.write("%s=%s\n" % (k, cfg[k]))
    print("\n   Saved settings to %s" % ENV_FILE)


# ------------------------------------------------------------------ normal run

def log_file(name):
    os.makedirs(LOGS, exist_ok=True)
    path = os.path.join(LOGS, name)
    try:
        if os.path.getsize(path) > 2_000_000:   # keep logs small: start over past 2 MB
            os.replace(path, path + ".old")
    except OSError:
        pass
    return open(path, "a", buffering=1, encoding="utf-8", errors="replace")


def say(msg):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), msg, flush=True)


def port_in_use(port):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.settimeout(0.3)
        return s.connect_ex(("127.0.0.1", port)) == 0


def kill_children_with_us():
    """Put this process in a Windows job that's killed when we exit, so ffmpeg and MediaMTX never outlive us."""
    try:
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateJobObjectW.restype = wintypes.HANDLE
        k32.CreateJobObjectW.argtypes = [wintypes.LPVOID, wintypes.LPCWSTR]
        k32.SetInformationJobObject.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
        k32.AssignProcessToJobObject.argtypes = [wintypes.HANDLE, wintypes.HANDLE]
        k32.GetCurrentProcess.restype = wintypes.HANDLE

        class LIMIT(ctypes.Structure):      # JOBOBJECT_BASIC_LIMIT_INFORMATION
            _fields_ = [("a", ctypes.c_int64), ("b", ctypes.c_int64), ("LimitFlags", wintypes.DWORD),
                        ("c", ctypes.c_size_t), ("d", ctypes.c_size_t), ("e", wintypes.DWORD),
                        ("f", ctypes.c_size_t), ("g", wintypes.DWORD), ("h", wintypes.DWORD)]

        class EXT(ctypes.Structure):        # JOBOBJECT_EXTENDED_LIMIT_INFORMATION
            _fields_ = [("basic", LIMIT), ("io", ctypes.c_uint64 * 6), ("i", ctypes.c_size_t),
                        ("j", ctypes.c_size_t), ("k", ctypes.c_size_t), ("l", ctypes.c_size_t)]

        job = k32.CreateJobObjectW(None, None)
        info = EXT()
        info.basic.LimitFlags = 0x2000                  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        ok = k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info))
        ok = ok and k32.AssignProcessToJobObject(job, k32.GetCurrentProcess())
        if not ok:
            say("couldn't set up the child-process job (error %d)" % ctypes.get_last_error())
        globals()["_job"] = job                         # keep the handle open for our whole life
    except Exception as e:
        say("couldn't set up the child-process job: %r" % e)


def keep_running(name, cmd, cwd, wait_for=None):
    """Like systemd Restart=always: start, wait for it to exit, start again 3 s later."""
    while True:
        if wait_for:
            while not port_in_use(wait_for):
                time.sleep(1)
        out = log_file(name + ".log")
        say("starting %s" % name)
        try:
            p = subprocess.Popen(cmd, cwd=cwd, stdout=out, stderr=out, stdin=subprocess.DEVNULL,
                                 creationflags=NO_WINDOW)
            code = p.wait()
            say("%s exited (code %s), restarting in 3 s (see logs\\%s.log)" % (name, code, name))
        except OSError as e:
            say("%s failed to start: %s" % (name, e))
        out.close()
        time.sleep(3)


def main():
    sys.stdout = sys.stderr = log_file("host.log")    # pythonw has no console
    CFG.update(load_env(ENV_FILE))
    port = int(CFG.get("PORT", "80"))
    if port_in_use(port):
        say("port %d already in use: tvbox-remote is probably running already. Exiting." % port)
        return
    kill_children_with_us()
    os.environ["VSEEBOX_ENV"] = ENV_FILE          # server.py reads its settings from here
    sys.path.insert(0, HERE)
    import server                                  # noqa: E402  (needs the env var set first)

    threading.Thread(target=keep_running, daemon=True,
                     args=("mediamtx", [MEDIAMTX, os.path.join(HERE, "mediamtx", "mediamtx.yml")],
                           os.path.join(HERE, "mediamtx"))).start()
    threading.Thread(target=keep_running, daemon=True, args=("ffmpeg", ffmpeg_cmd(), HERE, 8554)).start()
    say("control page on :%d -> Pico %s (%s)" % (port, server.ESP_HOST or "?",
                                                   "ready" if server.configured() else "NOT CONFIGURED"))
    server.ThreadingHTTPServer(("", port), server.Handler).serve_forever()


if __name__ == "__main__":
    if "--setup" in sys.argv:
        setup(sys.argv)
    elif "--print-ffmpeg" in sys.argv:
        CFG.update(load_env(ENV_FILE))
        print(subprocess.list2cmdline(ffmpeg_cmd()))
    else:
        try:
            main()
        except Exception as e:                      # make sure a crash at least leaves a trace
            say("crashed: %r" % e)
            raise
