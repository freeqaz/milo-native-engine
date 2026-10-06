#!/usr/bin/env python3
"""Drive rb3-native to a UI screen over HTTP and screenshot at frame offsets
from its arrival. Default: main_hub_screen with Play Now / Quickplay open,
the city scene the retail hub draws behind the menu (compare with a xenia
capture of the retail XEX at the same screen).
usage: screen_capture.py <rb3-native> <outdir> [offsets, default 120,240] [screen, default main_hub_screen]
env:   RB3_SCREEN_NAV overrides the input script (venue_capture.py's syntax)"""
import http.client, json, os, signal, socket, subprocess, sys, time
NAV = os.environ.get("RB3_SCREEN_NAV", "@10:start,@30:confirm,@140:select:pn_quickplay.btn")
def port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
def get(p, path, t=25):
    c = http.client.HTTPConnection("127.0.0.1", p, timeout=t)
    try:
        c.request("GET", path); r = c.getresponse(); return r.status, r.read()
    finally: c.close()
def health(p):
    try:
        st, b = get(p, "/api/health", 8)
        d = json.loads(b)["data"]; return int(d["frame"]), d["currentScreen"]
    except Exception: return None
b, out = sys.argv[1], sys.argv[2]
offs = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "120,240").split(",")]
screen = sys.argv[4] if len(sys.argv) > 4 else "main_hub_screen"
os.makedirs(out, exist_ok=True)
p = port()
env = dict(os.environ, RB3_GAME="1", RB3_HTTP="1", RB3_HTTP_PORT=str(p), MILO_HEADLESS="1",
           RB3_DATA=os.environ.get("RB3_DATA", os.path.expanduser("~/code/milohax/rb3/orig-assets/extracted")), RB3_GAME_INPUT=NAV)
log = open(os.path.join(out, "run.log"), "w")
proc = subprocess.Popen([b], env=env, stdout=log, stderr=subprocess.STDOUT,
                        cwd=os.environ.get("RB3_CHECKOUT", os.path.expanduser("~/code/milohax/rb3")), start_new_session=True)
try:
    dl = time.time() + 600; g0 = None
    while time.time() < dl and proc.poll() is None:
        h = health(p)
        if h and h[1] == screen: g0 = h[0]; break
        time.sleep(0.3)
    if g0 is None: print("never reached", screen); sys.exit(2)
    print(screen, "at frame", g0)
    for o in offs:
        while proc.poll() is None:
            h = health(p)
            if h and h[0] >= g0 + o: break
            time.sleep(0.1)
        st, data = get(p, "/api/screenshot")
        h = health(p)
        fn = os.path.join(out, "s%04d.png" % o)
        if st == 200 and data[:4] == b"\x89PNG":
            open(fn, "wb").write(data); print("captured", fn, "frame", h[0] if h else "?", h[1] if h else "?")
        else: print("screenshot failed", st)
finally:
    try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM); proc.wait(timeout=8)
    except Exception:
        try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception: pass
