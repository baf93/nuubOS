#!/usr/bin/env python3
# Host test: nuubos-syncd runs Syncthing only for the active, enabled user
# (switches with the user, no restart storm) and nuubos-syncctl configures
# folders/devices through the REST API on the unix socket. Fake Syncthing.
# Run: python3 package/nuubos/nuubos-sync/tests/sync.py
import json, os, signal, socket, subprocess, sys, tarfile, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..', '..', '..', '..'))
src = os.path.join(here, '..', 'src')
t = tempfile.mkdtemp()
run, users = t + '/run', t + '/users'
U1, U2 = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa', 'bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb'
for d in (run + '/user', users + '/' + U1, users + '/' + U2, t + '/cj/cjson'):
    os.makedirs(d, exist_ok=True)
with tarfile.open(root + '/dl/cjson/cjson-1.7.19.tar.gz') as tf:
    for m in tf.getmembers():
        if m.name.endswith(('/cJSON.c', '/cJSON.h')):
            m.name = os.path.basename(m.name); tf.extract(m, t + '/cj/')
open(t + '/cj/cjson/cJSON.h', 'w').write(open(t + '/cj/cJSON.h').read())

fake = t + '/syncthing'
open(fake, 'w').write(r'''#!/usr/bin/env python3
import http.server, json, os, socketserver, sys
args = dict(a.split("=", 1) for a in sys.argv[2:] if "=" in a)
home = args["--home"]; key = args["--gui-apikey"]; sock = args["--gui-address"][len("unix://"):]
open(home + "/ran", "a").write("x")
if os.path.exists(home + "/crash"): sys.exit(3)
state = {"folders": {}, "devices": {"SELF-AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-GGGG-HHHH-IIII-JJJJ-KKKK": {"deviceID": "SELF-AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-GGGG-HHHH-IIII-JJJJ-KKKK", "name": "me"}}}
MY = "SELF-AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-GGGG-HHHH-IIII-JJJJ-KKKK"
PEND = "PEER-AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-GGGG-HHHH-IIII-JJJJ-KKKK"
class H(http.server.BaseHTTPRequestHandler):
    def address_string(self): return "unix"
    def log_message(self, *a): pass
    def out(self, code, obj=None):
        b = json.dumps(obj).encode() if obj is not None else b""
        self.send_response(code); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def auth(self):
        if self.headers.get("X-API-Key") != key: self.out(403); return False
        return True
    def do_GET(self):
        if not self.auth(): return
        p = self.path
        if p == "/rest/system/status": self.out(200, {"myID": MY})
        elif p == "/rest/config/folders": self.out(200, list(state["folders"].values()))
        elif p == "/rest/config/devices": self.out(200, list(state["devices"].values()))
        elif p == "/rest/system/connections": self.out(200, {"connections": {d: {"connected": d != MY} for d in state["devices"]}})
        elif p == "/rest/cluster/pending/devices": self.out(200, {} if PEND in state["devices"] else {PEND: {"name": "Laptop"}})
        else: self.out(404)
    def body(self):
        return json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"null")
    def do_PUT(self):
        if not self.auth(): return
        kind, _, ident = self.path.rpartition("/")
        b = self.body()
        if kind == "/rest/config/folders": state["folders"][ident] = b
        elif kind == "/rest/config/devices": state["devices"][ident] = b
        json.dump(state, open(home + "/state.json", "w"))
        self.out(200)
    def do_DELETE(self):
        if not self.auth(): return
        kind, _, ident = self.path.rpartition("/")
        tbl = state["folders" if kind.endswith("folders") else "devices"]
        code = 200 if tbl.pop(ident, None) is not None else 404
        json.dump(state, open(home + "/state.json", "w"))
        self.out(code)
class S(socketserver.ThreadingMixIn, socketserver.UnixStreamServer): pass
try: os.unlink(sock)
except OSError: pass
S(sock, H).serve_forever()
''')
os.chmod(fake, 0o755)
cc = ['gcc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation', '-I' + t + '/cj',
      '-DRUN_ROOT="%s"' % run, '-DUSERDATA_USERS="%s"' % users]
subprocess.check_call(cc + ['-DSYNCTHING_BIN="%s"' % fake, src + '/syncd.c', '-o', t + '/syncd'])
subprocess.check_call(cc + [src + '/syncctl.c', t + '/cj/cJSON.c', '-o', t + '/syncctl', '-lcurl'])
fails = 0
def check(n, c):
    global fails
    if not c: print('[FAIL]', n); fails += 1
def ctl(*a): return subprocess.run([t + '/syncctl'] + list(a), capture_output=True, text=True).stdout
def st(): return open(run + '/syncd.state').read() if os.path.exists(run + '/syncd.state') else ''
def wait(c, s=6):
    e = time.time() + s
    while time.time() < e:
        if c(): return True
        time.sleep(0.05)
    return False
def set_user(u):
    open(run + '/user/active.tmp', 'w').write(u + '\n'); os.rename(run + '/user/active.tmp', run + '/user/active')

set_user(U1)
d = subprocess.Popen([t + '/syncd'], stderr=open(t + '/syncd.log', 'w'))
try:
    check('disabled by default: not running', wait(lambda: 'running=0' in st()))
    check('enable', ctl('enable').startswith('OK'))
    check('runs for U1', wait(lambda: 'running=1' in st() and U1 in st()))
    check('per-user api key', 'APIKEY=' in open(users + '/' + U1 + '/appdata/syncthing/nuubos.conf').read())
    wait(lambda: os.path.exists(run + '/syncthing.sock'))
    time.sleep(0.3)
    s = ctl('status')
    check('status device id + pending', 'device_id=SELF' in s and 'pending=PEER' in s and 'folder=saves\t0' in s)
    check('folder saves on', ctl('folder', 'saves', 'on').startswith('OK'))
    check('accept device', ctl('accept', 'PEER-AAAA-BBBB-CCCC-DDDD-EEEE-FFFF-GGGG-HHHH-IIII-JJJJ-KKKK', 'Laptop').startswith('OK'))
    state = json.load(open(users + '/' + U1 + '/appdata/syncthing/state.json'))
    f = state['folders'].get('nuubos-saves', {})
    check('saves folder path per user', f.get('path') == users + '/' + U1 + '/saves')
    check('saves shared with new device', any(x['deviceID'].startswith('PEER') for x in f.get('devices', [])))
    s = ctl('status')
    check('device listed connected', 'device=PEER' in s and '\tLaptop\t1' in s and 'folder=saves\t1' in s)
    check('bad device id', ctl('accept', '../etc').startswith('Usage') or ctl('accept', '../etc') == '')
    set_user(U2)
    check('user switch stops U1 (U2 not enabled)', wait(lambda: 'running=0' in st()))
    set_user(U1)
    check('back to U1 restarts it', wait(lambda: 'running=1' in st()))
    # Crash: no restart storm.
    open(users + '/' + U1 + '/appdata/syncthing/crash', 'w').write('1')
    pid = int(open(run + '/syncd.pid').read())
    subprocess.call(['pkill', '-f', fake])
    check('crash -> failed, not restarted', wait(lambda: 'failed=1' in st() and 'running=0' in st()))
    runs = open(users + '/' + U1 + '/appdata/syncthing/ran').read().count('x')
    time.sleep(1)
    check('no restart loop', open(users + '/' + U1 + '/appdata/syncthing/ran').read().count('x') == runs)
    os.unlink(users + '/' + U1 + '/appdata/syncthing/crash')
    check('disable', ctl('disable').startswith('OK') and wait(lambda: 'running=0' in st()))
finally:
    d.terminate(); d.wait(); subprocess.call(['pkill', '-f', fake])
print('[PASS] sync' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
