#!/usr/bin/env python3
"""Drive rb3-native to game_screen over HTTP and screenshot at game-relative frame offsets.
usage: venue_capture.py <rb3-native> <outdir> [offsets e.g. 60,300,600,900]"""
import http.client, json, os, signal, socket, subprocess, sys, time
NAV = ("@10:start,@30:confirm,@140:select:pn_quickplay.btn,@220:select:qp_quickplay.btn,"
       "@320:down,@350:msg:music_library:select_highlighted_node,@380:part:guitar,@400:diff:expert,"
       "@500:nofail,@520:autohit")
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
offs = [int(x) for x in (sys.argv[3] if len(sys.argv) > 3 else "60,300,600,900").split(",")]
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
        if h and h[1] == "game_screen": g0 = h[0]; break
        time.sleep(0.3)
    if g0 is None: print("never reached game_screen"); sys.exit(2)
    print("game_screen at frame", g0)
    for o in offs:
        while proc.poll() is None:
            h = health(p)
            if h and h[0] >= g0 + o: break
            time.sleep(0.1)
        st, data = get(p, "/api/screenshot")
        h = health(p)
        fn = os.path.join(out, "g%04d.png" % o)
        if st == 200 and data[:4] == b"\x89PNG":
            open(fn, "wb").write(data); print("captured", fn, "frame", h[0] if h else "?", h[1] if h else "?")
        else: print("screenshot failed", st)
finally:
    try: os.killpg(os.getpgid(proc.pid), signal.SIGTERM); proc.wait(timeout=8)
    except Exception:
        try: os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except Exception: pass
