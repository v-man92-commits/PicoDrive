#!/usr/bin/env python3
# Drives the real web UI (web_harness) like a browser would and checks the
# security rules and every API action. Run from the folder with web_harness.
import http.client, json, os, socket, subprocess, sys, time

passed = failed = 0
def check(cond, what):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("  FAIL:", what)

for f in ("stop", "busy", "pin.txt"):
    if os.path.exists(f):
        os.remove(f)
proc = subprocess.Popen(["./web_harness"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
for _ in range(100):
    if os.path.exists("pin.txt"):
        break
    time.sleep(0.05)
pin = open("pin.txt").read()
check(len(pin) == 4 and pin.isdigit(), "PIN is 4 digits: %r" % pin)

def req(method, path, cookie=None, timeout=10):
    c = http.client.HTTPConnection("127.0.0.1", 18090, timeout=timeout)
    h = {"Cookie": cookie} if cookie else {}
    c.request(method, path, headers=h)
    r = c.getresponse()
    body = r.read()
    hdrs = dict(r.getheaders())
    c.close()
    return r.status, hdrs, body

# page loads without a session; API doesn't
st, h, body = req("GET", "/")
check(st == 200 and b"PicoDrive" in body and "Content-Security-Policy" in h, "page served")
check(h.get("Cache-Control") == "no-store", "no caching")
st, h, body = req("GET", "/api/status")
check(st == 401 and json.loads(body).get("login") is True, "status needs login (%d)" % st)
st, _, _ = req("POST", "/api/mount?name=IMAGE_01.img")
check(st == 401, "actions need login")

# login: GET refused, wrong PIN, then right PIN
st, _, _ = req("GET", "/api/login?pin=" + pin)
check(st == 401, "login over GET refused (%d)" % st)
wrong = "%04d" % ((int(pin) + 1) % 10000)
st, h, body = req("POST", "/api/login?pin=" + wrong)
check(json.loads(body)["ok"] is False and "Set-Cookie" not in h, "wrong PIN refused")
st, h, body = req("POST", "/api/login?pin=" + pin)
cookie_hdr = h.get("Set-Cookie", "")
check(json.loads(body)["ok"] is True and "imsess=" in cookie_hdr and "HttpOnly" in cookie_hdr and "SameSite=Strict" in cookie_hdr,
      "right PIN gives a HttpOnly SameSite cookie")
cookie = cookie_hdr.split(";")[0]
check(len(cookie.split("=")[1]) == 16, "session token is 16 hex chars")
st, _, body = req("GET", "/api/status", "imsess=0000000000000000")
check(st == 401, "forged cookie refused")
st, _, body = req("GET", "/api/status", "other=1; " + cookie + "; x=y")
check(st == 200, "cookie found among others")

# status and images are valid JSON; names are escaped properly
st, _, body = req("GET", "/api/status", cookie)
s = json.loads(body)
check(st == 200 and s["mounted"] is False and s["fs"] == "exFAT", "status JSON")
st, _, body = req("GET", "/api/images", cookie)
imgs = json.loads(body)
check(len(imgs) == 3 and imgs[2]["name"] == 'Disney "<b>" & co.cue' and imgs[2]["cue"] is True, "images JSON with tricky name")
check(imgs[1]["disc"] is True and imgs[0]["kind"].startswith("FATX"), "image kinds")

# actions are POST only
st, _, _ = req("GET", "/api/mount?name=IMAGE_01.img", cookie)
check(st == 405, "GET on an action refused (%d)" % st)

def act(path):
    st, _, body = req("POST", path, cookie)
    return st, json.loads(body)

st, r = act("/api/mount?name=IMAGE_01.img&mode=auto")
check(r["ok"] and json.loads(req("GET", "/api/status", cookie)[2])["name"] == "IMAGE_01.img", "mount")
st, r = act("/api/mount?name=UBISCRABBLE.iso")
check(not r["ok"] and "Unmount" in r["msg"], "second mount refused")
st, r = act("/api/unmount?force=0")
check(r["ok"], "unmount")
st, r = act("/api/mount?name=Disney%20%22%3Cb%3E%22%20%26%20co.cue&mode=auto")
s = json.loads(req("GET", "/api/status", cookie)[2])
check(r["ok"] and s["cd"] is True and s["name"] == 'Disney "<b>" & co.cue', "mount tricky name as CD")
open("busy", "w").close()
time.sleep(0.1)
st, r = act("/api/unmount?force=0")
check(not r["ok"] and "PC" in r["msg"], "unmount refused while the PC is busy")
st, r = act("/api/unmount?force=1")
check(r["ok"], "forced unmount")
os.remove("busy")
st, r = act("/api/mount?name=nope.img")
check(not r["ok"] and "nope.img" in r["msg"], "unknown image")
st, r = act("/api/create?name=NEW_ONE&mib=8")
check(r["ok"] and any(i["name"] == "NEW_ONE.img" for i in json.loads(req("GET", "/api/images", cookie)[2])), "create")
st, r = act("/api/delete?name=NEW_ONE.img")
check(r["ok"] and not any(i["name"] == "NEW_ONE.img" for i in json.loads(req("GET", "/api/images", cookie)[2])), "delete")
st, r = act("/api/sharesd")
check(r["ok"] and json.loads(req("GET", "/api/status", cookie)[2])["sdShared"] is True, "share SD")
check(json.loads(req("GET", "/api/images", cookie)[2]) == [], "no image list while shared")
st, r = act("/api/unmount?force=0")
check(r["ok"], "unshare")
s = json.loads(req("GET", "/api/status", cookie)[2])
check(s["last"] == "Unmounted", "last action reported")
st, _, _ = req("POST", "/api/bogus", cookie)
check(st == 404, "unknown action 404")

# idle (preconnect-style) connections don't stall real requests
idle = [socket.create_connection(("127.0.0.1", 18090)) for _ in range(6)]
t = time.time()
st, _, _ = req("GET", "/api/status", cookie)
check(st == 200 and time.time() - t < 1.0, "fast with 6 idle connections (%.2fs)" % (time.time() - t))
for s_ in idle:
    s_.close()

# junk and oversized requests
s_ = socket.create_connection(("127.0.0.1", 18090)); s_.sendall(b"garbage\r\n\r\n")
check(s_.recv(100).startswith(b"HTTP/1.0 400"), "garbage -> 400"); s_.close()
s_ = socket.create_connection(("127.0.0.1", 18090)); s_.sendall(b"A" * 5000)
check(s_.recv(100).startswith(b"HTTP/1.0 400"), "oversized -> 400"); s_.close()

# brute force: 5 wrong PINs lock logins out, even with the right PIN
for _ in range(5):
    req("POST", "/api/login?pin=" + wrong)
st, h, body = req("POST", "/api/login?pin=" + pin)
check(json.loads(body)["ok"] is False and "Wait" in json.loads(body)["msg"], "locked out after 5 wrong PINs")
st, _, _ = req("GET", "/api/status", cookie)
check(st == 200, "existing session still works during lockout")

open("stop", "w").close()
proc.wait(timeout=10)
out = proc.stdout.read().decode()
check("runtime error" not in out and "ERROR: AddressSanitizer" not in out, "no sanitizer errors")
if out.strip():
    print(out[-2000:])
print("web: %d checks passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
