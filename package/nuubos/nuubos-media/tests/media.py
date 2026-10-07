#!/usr/bin/env python3
# Host test: nuubos-mediad session control against a fake mpv speaking the
# JSON IPC on fd 3, and nuubos-jellyfinctl against a fake Jellyfin server.
# Run: python3 package/nuubos/nuubos-media/tests/media.py
import http.server, json, os, socket, subprocess, sys, tarfile, tempfile, threading, time

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..', '..', '..', '..'))
src = os.path.join(here, '..', 'src')
t = tempfile.mkdtemp()
run, users, state = t + '/run', t + '/users', t + '/state'
user = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
for d in (run + '/user', users + '/' + user, state + '/' + user, t + '/inc/nuubos', t + '/cj/cjson', t + '/music'):
    os.makedirs(d, exist_ok=True)
open(run + '/user/active', 'w').write(user + '\n')
open(t + '/inc/nuubos/notify.h', 'w').write(open(os.path.join(root, 'package/nuubos/nuubos-notify/src/notify.h')).read())
with tarfile.open(root + '/dl/cjson/cjson-1.7.19.tar.gz') as tf:
    for m in tf.getmembers():
        if m.name.endswith(('/cJSON.c', '/cJSON.h')):
            m.name = os.path.basename(m.name); tf.extract(m, t + '/cj/')
open(t + '/cj/cjson/cJSON.h', 'w').write(open(t + '/cj/cJSON.h').read())
for n in ('b.mp3', 'a.flac', 'c.txt', 'd.ogg'):
    open(t + '/music/' + n, 'w').write('x')
open(t + '/movie.mkv', 'w').write('x')

fake_mpv = t + '/mpv'
open(fake_mpv, 'w').write('''#!/usr/bin/env python3
import json, os, socket, sys
args = sys.argv[1:]
open(os.environ.get("HOME", "/tmp") + "/args", "w").write("\\n".join(args))
s = socket.socket(fileno=3)
f = s.makefile("rw")
paused = False
target = args[-1]
plist = [a.split("=", 1)[1] for a in args if a.startswith("--playlist=")]
items = open(plist[0]).read().split() if plist else [target]
start = [int(a.split("=", 1)[1]) for a in args if a.startswith("--playlist-start=")]
pos = start[0] if start else 0
def ev(name, data):
    f.write(json.dumps({"event": "property-change", "name": name, "data": data}) + "\\n"); f.flush()
for line in f:
    m = json.loads(line); c = m.get("command", [])
    if c[:1] == ["observe_property"]:
        n = c[2]
        if n == "path": ev("path", items[pos])
        elif n == "media-title": ev("media-title", os.path.basename(items[pos]))
        elif n == "pause": ev("pause", False)
        elif n == "playlist-pos": ev("playlist-pos", pos)
        elif n == "playlist-count": ev("playlist-count", len(items))
        elif n == "vid": ev("vid", 1 if items[pos].endswith(".mkv") else False)
    elif c == ["cycle", "pause"]:
        paused = not paused; ev("pause", paused)
    elif c[:1] == ["playlist-next"]:
        pos += 1; ev("playlist-pos", pos); ev("path", items[pos])
    elif c[:1] == ["get_property"]:
        f.write(json.dumps({"data": 42.5, "request_id": m.get("request_id"), "error": "success"}) + "\\n"); f.flush()
    elif c == ["quit-watch-later"]:
        sys.exit(0)
''')
os.chmod(fake_mpv, 0o755)

common = ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation', '-I' + t + '/inc', '-I' + t + '/cj']
subprocess.check_call(['gcc'] + common + ['-DRUN_ROOT="%s"' % run, '-DMPV_BIN="%s"' % fake_mpv,
    '-DUSERDATA_USERS="%s"' % users, '-DNUUBOS_NOTIFY_SOCKET="%s/notifyd.sock"' % run,
    src + '/mediad.c', t + '/cj/cJSON.c', '-o', t + '/mediad'])
subprocess.check_call(['gcc'] + common + ['-DRUN_ROOT="%s"' % run, src + '/playerctl.c', '-o', t + '/playerctl'])
subprocess.check_call(['gcc'] + common + ['-DSTATE_USERS="%s"' % state, src + '/jellyfinctl.c', t + '/cj/cJSON.c', '-o', t + '/jf', '-lcurl'])

fails = 0
def check(name, cond):
    global fails
    if not cond:
        print('[FAIL]', name); fails += 1
def pc(*a):
    return subprocess.run([t + '/playerctl'] + list(a), capture_output=True, text=True).stdout
def status():
    return dict(l.split('=', 1) for l in pc('status').splitlines() if '=' in l)
def wait(cond, secs=5):
    end = time.time() + secs
    while time.time() < end:
        if cond(): return True
        time.sleep(0.05)
    return False

d = subprocess.Popen([t + '/mediad'], stderr=open(t + '/mediad.log', 'w'))
try:
    wait(lambda: os.path.exists(run + '/mediad.sock'))
    check('missing file refused', pc('play', t + '/nope.mkv').startswith('ERR file'))
    check('play video', pc('play', t + '/movie.mkv', '30').startswith('OK'))
    check('state playing, video', wait(lambda: status().get('state') == 'playing' and status().get('kind') == 'video'))
    args = open(users + '/' + user + '/appdata/media/args').read()
    check('cedrus decode + dmabuf', '--hwdec=drm' in args and '--vo=dmabuf-wayland,gpu' in args)
    check('per-user resume', 'watch-later' in args and '--save-position-on-quit=yes' in args and '--start=30' in args)
    check('busy while playing', pc('play', t + '/movie.mkv').startswith('ERR busy'))
    check('pause', pc('pause').startswith('OK') and wait(lambda: status().get('state') == 'paused'))
    check('stop', pc('stop').startswith('OK'))
    check('idle after stop + position kept', wait(lambda: status().get('state') == 'idle') and status().get('last_position') == '42' and status().get('last_item', '').endswith('movie.mkv'))
    check('playdir', pc('playdir', t + '/music', 'b.mp3').startswith('OK'))
    check('audio playlist from file', wait(lambda: status().get('kind') == 'audio' and status().get('count') == '3' and status().get('index') == '1'))
    pl = open(run + '/media-playlist.m3u').read().split()
    check('only audio files, sorted', [os.path.basename(p) for p in pl] == ['a.flac', 'b.mp3', 'd.ogg'])
    check('next', pc('next').startswith('OK') and wait(lambda: status().get('item', '').endswith('d.ogg')))
    check('pre-power stops and replies', pc('pre-power').startswith('OK') and status().get('state') == 'idle')
    check('commands need a session', pc('pause').startswith('ERR no-session'))
finally:
    d.terminate(); d.wait()

# Jellyfin
seen = []
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def reply(self, code, obj=None):
        self.send_response(code); self.send_header('Content-Type', 'application/json'); self.end_headers()
        if obj is not None: self.wfile.write(json.dumps(obj).encode())
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get('Content-Length', 0))).decode()
        seen.append((self.path, body, self.headers.get('X-Emby-Authorization')))
        if self.path == '/Users/AuthenticateByName':
            b = json.loads(body)
            if b['Pw'] == 'secret': self.reply(200, {'AccessToken': 'tok', 'User': {'Id': 'u1'}})
            else: self.reply(401)
        elif self.path == '/Sessions/Playing/Stopped': self.reply(204)
    def do_GET(self):
        seen.append((self.path, '', self.headers.get('X-Emby-Authorization')))
        if 'Token="tok"' not in (self.headers.get('X-Emby-Authorization') or ''): return self.reply(401)
        if self.path == '/Users/u1/Views':
            self.reply(200, {'Items': [{'Id': 'lib1', 'Name': 'Movies', 'Type': 'CollectionFolder', 'IsFolder': True}]})
        elif self.path.startswith('/Users/u1/Items?ParentId=lib1'):
            self.reply(200, {'Items': [{'Id': 'm1', 'Name': 'Big\tBuck', 'Type': 'Movie', 'IsFolder': False,
                                        'RunTimeTicks': 6000000000, 'UserData': {'PlaybackPositionTicks': 1200000000}}]})
srv = http.server.HTTPServer(('127.0.0.1', 0), H); port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()
jf = lambda *a, stdin=None: subprocess.run([t + '/jf'] + list(a), input=stdin, capture_output=True, text=True).stdout
url = 'http://127.0.0.1:%d' % port
check('wrong password', jf('login', user, url, 'bob', stdin='nope\n').startswith('ERR auth'))
check('login', jf('login', user, url, 'bob', stdin='secret\n').startswith('OK'))
conf = open(state + '/' + user + '/secrets/jellyfin.conf').read()
check('token stored, password not', 'TOKEN=tok' in conf and 'secret' not in conf)
check('creds root-only', (os.stat(state + '/' + user + '/secrets/jellyfin.conf').st_mode & 0o077) == 0)
check('views', 'item=lib1\tMovies\tCollectionFolder\t1' in jf('views', user))
out = jf('items', user, 'lib1')
check('items with resume', 'item=m1\tBig Buck\tMovie\t0\t120\t600' in out)
check('stream url', jf('stream', user, 'm1').strip() == url + '/Items/m1/Download?api_key=tok')
check('progress', jf('progress', user, 'm1', '42').startswith('OK') and any('"PositionTicks":420000000' in b for p, b, _ in seen))
check('bad item id', jf('items', user, '../x').startswith('ERR'))
check('logout', jf('logout', user).startswith('OK') and jf('status', user).count('signed=0') == 1)
srv.shutdown()
print('[PASS] media' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
