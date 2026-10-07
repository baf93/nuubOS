#!/usr/bin/env python3
# Host test: nuubos-scraper against a local fake ScreenScraper API and the
# real libraryd. Checks checksum match (auto + media), name-only match
# (review, no media until accept), not found, quota stop, credentials never
# stored, reject/clear, account storage.
# Run: python3 package/nuubos/nuubos-scraper/tests/scraper.py
import http.server, json, os, socket, subprocess, sys, tarfile, tempfile, threading, time, zlib

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..', '..', '..', '..'))
src = os.path.join(here, '..', 'src')
lib = os.path.join(root, 'package/nuubos/nuubos-library/src')
t = tempfile.mkdtemp()
run, ud, share, state = t + '/run', t + '/userdata', t + '/share', t + '/state'
user = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
for d in (run + '/user', ud + '/roms/snes', share, state + '/users/' + user, t + '/cj'):
    os.makedirs(d, exist_ok=True)
open(share + '/systems.conf', 'w').write(open(lib + '/systems.conf').read())
open(run + '/user/active', 'w').write(user + '\n')
open(state + '/users/%s/localization.conf' % user, 'w').write('LANGUAGE=it\n')
roms = {'Mario.sfc': b'M' * 1000, 'Zelda.sfc': b'Z' * 2000, 'Unknown.sfc': b'U' * 300, 'Quota.sfc': b'Q' * 400}
for n, d in roms.items():
    open(ud + '/roms/snes/' + n, 'wb').write(d)
crc = {n: '%08X' % (zlib.crc32(d) & 0xffffffff) for n, d in roms.items()}
with tarfile.open(root + '/dl/cjson/cjson-1.7.19.tar.gz') as tf:
    for m in tf.getmembers():
        if m.name.endswith(('/cJSON.c', '/cJSON.h')):
            m.name = os.path.basename(m.name); tf.extract(m, t + '/cj/')
os.makedirs(t + '/cj/cjson'); open(t + '/cj/cjson/cJSON.h', 'w').write(open(t + '/cj/cJSON.h').read())

seen_urls = []
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        seen_urls.append(self.path)
        from urllib.parse import urlparse, parse_qs
        u = urlparse(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}
        if u.path.endswith('/media.png'):
            self.send_response(200); self.end_headers(); self.wfile.write(b'\x89PNG' + b'x' * 200); return
        name = q.get('romnom')
        if name == 'Quota.sfc':
            self.send_response(430); self.end_headers(); return
        if name == 'Unknown.sfc':
            self.send_response(404); self.end_headers(); self.wfile.write(b'Erreur'); return
        if q.get('devid') != 'dev' or q.get('devpassword') != 'devpw':
            self.send_response(403); self.end_headers(); return
        media = 'http://127.0.0.1:%d/media.png?devid=dev&devpassword=devpw&ssid=me&sspassword=pw&media=box' % port
        jeu = {'id': '42', 'noms': [{'region': 'us', 'text': 'US Name'}, {'region': 'wor', 'text': 'World ' + name}],
               'synopsis': [{'langue': 'en', 'text': 'English'}, {'langue': 'it', 'text': 'Italiano\nriga'}],
               'dates': [{'region': 'us', 'text': '1991-08-23'}], 'developpeur': {'id': '1', 'text': 'Nintendo'},
               'editeur': {'text': 'Nintendo'}, 'joueurs': {'text': '1-2'}, 'note': {'text': '18'},
               'genres': [{'noms': [{'langue': 'en', 'text': 'Platform'}, {'langue': 'it', 'text': 'Piattaforme'}]}],
               'medias': [{'type': 'box-2D', 'region': 'us', 'url': media, 'format': 'png'},
                          {'type': 'ss', 'region': 'wor', 'url': media, 'format': 'png'}]}
        # Mario matches by checksum; Zelda only by name (another CRC).
        jeu['rom'] = {'romcrc': crc['Mario.sfc'] if name == 'Mario.sfc' else 'DEADBEEF'}
        body = json.dumps({'response': {'jeu': jeu}}).encode()
        self.send_response(200); self.send_header('Content-Type', 'application/json'); self.end_headers()
        self.wfile.write(body)
srv = http.server.HTTPServer(('127.0.0.1', 0), H); port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

common = ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-DRUN_ROOT="%s"' % run]
subprocess.check_call(['gcc'] + common + ['-DUSERDATA_ROOT="%s"' % ud, '-DSHARE_ROOT="%s"' % share,
                       lib + '/libraryd.c', '-o', t + '/libraryd', '-lpthread'])
subprocess.check_call(['gcc'] + common + ['-Wno-format-truncation', '-I' + t + '/cj', '-DUSERDATA_ROOT="%s"' % ud, '-DSTATE_ROOT="%s"' % state,
    '-DAPI_BASE="http://127.0.0.1:%d/api2"' % port, '-DSS_DEVID="dev"', '-DSS_DEVPASSWORD="devpw"',
    src + '/scraper.c', t + '/cj/cJSON.c', '-o', t + '/scraper', '-lcurl', '-lz'])
lp = subprocess.Popen([t + '/libraryd'], stderr=open(t + '/lib.log', 'w'))
fails = 0
def check(name, cond):
    global fails
    if not cond:
        print('[FAIL]', name); fails += 1
def sc(*args, stdin=None, env=None):
    e = dict(os.environ); e.update(env or {})
    return subprocess.run([t + '/scraper'] + list(args), input=stdin, capture_output=True, text=True, env=e).stdout
def libcmd(line):
    s = socket.socket(socket.AF_UNIX)
    for _ in range(50):
        try: s.connect(run + '/libraryd.sock'); break
        except OSError: time.sleep(0.1)
    s.sendall((line + '\n').encode()); s.settimeout(2); d = b''
    while not d.endswith(b'end=1\n'):
        try:
            c = s.recv(65536)
        except socket.timeout: break
        if not c: break
        d += c
    return d.decode()
try:
    for _ in range(50):
        if 'games=4' in libcmd('STATUS'): break
        time.sleep(0.1)
    ids = {l.split('\t')[2]: l[5:21] for l in libcmd('GAMES\tsystem:snes').splitlines() if l.startswith('game=')}

    check('account set', sc('account', 'set', 'me', stdin='pw\n').startswith('OK'))
    st = os.stat(state + '/scraper/screenscraper.conf')
    check('account file root-only', (st.st_mode & 0o077) == 0)

    out = sc('game', ids['Mario'])
    check('auto match', out.startswith('OK scraped'))
    meta = open(ud + '/library/metadata/snes/Mario.txt').read()
    check('match=auto', 'match=auto' in meta)
    check('title by region pref', 'title=World Mario.sfc' in meta)
    check('language pref (it)', 'description=Italiano\\nriga' in meta and 'genre=Piattaforme' in meta)
    check('fields', 'developer=Nintendo' in meta and 'players=1-2' in meta and 'release=1991-08-23' in meta)
    check('cover downloaded', os.path.exists(ud + '/library/covers/snes/Mario.png'))
    check('screenshot downloaded', 'screenshot=' in meta and os.path.exists(ud + '/library/screenshots/snes/Mario.png'))
    check('no credentials stored', 'devpw' not in meta and 'sspassword' not in meta and 'pw' not in meta.replace('png', ''))
    check('crc sent', any('crc=' + crc['Mario.sfc'] in u for u in seen_urls))
    d = libcmd('DETAILS\t' + ids['Mario'])
    check('libraryd shows metadata', 'meta_developer=Nintendo' in d)
    check('libraryd picked up cover', '/library/covers/snes/Mario.png' in d)

    check('matched game skipped without force', 'matched' in sc('game', ids['Mario']))

    out = sc('game', ids['Zelda'])
    check('name-only -> review', out.startswith('OK review'))
    check('review: no cover yet', not os.path.exists(ud + '/library/covers/snes/Zelda.png'))
    check('accept', sc('accept', ids['Zelda']).startswith('OK'))
    check('accepted: match=user + cover', 'match=user' in open(ud + '/library/metadata/snes/Zelda.txt').read()
          and os.path.exists(ud + '/library/covers/snes/Zelda.png'))

    check('not found', 'not-found' in sc('game', ids['Unknown']))
    out = sc('game', ids['Quota'], env={'NUUBOS_JOB_ID': '1'})
    check('quota stops (job protocol)', '@error quota' in out)

    check('reject', sc('reject', ids['Zelda']).startswith('OK'))
    check('rejected marker, metadata gone', os.path.exists(ud + '/library/metadata/snes/Zelda.rejected')
          and not os.path.exists(ud + '/library/metadata/snes/Zelda.txt'))
    check('clear removes scraped media', sc('clear', ids['Mario']).startswith('OK')
          and not os.path.exists(ud + '/library/covers/snes/Mario.png')
          and not os.path.exists(ud + '/library/screenshots/snes/Mario.png'))

    out = sc('bulk', 'system:snes', env={'NUUBOS_JOB_ID': '2'})
    check('bulk stops on quota, reports progress', '@progress' in out and '@error quota' in out)
    check('bulk skipped rejected game', not os.path.exists(ud + '/library/metadata/snes/Zelda.txt'))
    check('no partial files', not any(f.endswith('.part') for _, _, fs in os.walk(ud) for f in fs))
finally:
    lp.terminate(); lp.wait(); srv.shutdown()
print('[PASS] scraper' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
