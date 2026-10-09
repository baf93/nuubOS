#!/usr/bin/env python3
# Host test: nuubos-scraper against a fake Skyscraper (gather + Pegasus game
# list generation), a fake thumbnails.libretro.com and the real libraryd.
# Checks: Skyscraper arguments, account/key reach Skyscraper only through
# the tmpfs config (0600), results mapped into the store (fields, rating,
# multi-line description, cover, screenshot), search name -> --query, fill
# mode keeps existing data and adds the missing, update (force), quota stop,
# not found, libretro exact and title match with region preference,
# reject/clear, no scratch or partial files left.
# Run: python3 package/nuubos/nuubos-scraper/tests/scraper.py
import http.server, os, socket, stat, subprocess, sys, tempfile, threading, time
from urllib.parse import quote, unquote

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..', '..', '..', '..'))
src = os.path.join(here, '..', 'src')
lib = os.path.join(root, 'package/nuubos/nuubos-library/src')
t = tempfile.mkdtemp()
run, ud, share, state = t + '/run', t + '/userdata', t + '/share', t + '/state'
user = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
for d in (run + '/user', ud + '/roms/snes', share, state + '/users/' + user):
    os.makedirs(d, exist_ok=True)
open(share + '/systems.conf', 'w').write(open(lib + '/systems.conf').read())
open(run + '/user/active', 'w').write(user + '\n')
open(state + '/users/%s/localization.conf' % user, 'w').write('LANGUAGE=it\n')
roms = ['Mario.sfc', 'Zelda.sfc', 'Unknown.sfc', 'Quota.sfc', 'Super Metroid (Europe).sfc', 'Metroid.sfc']
for i, n in enumerate(roms):
    open(ud + '/roms/snes/' + n, 'wb').write(bytes([65 + i]) * (1000 + i))
PNG = b'\x89PNG\r\n\x1a\n' + b'x' * 200

# Fake Skyscraper: gathering (-s) logs its arguments; generation (-f pegasus)
# writes a game list with media for the games it "knows".
fake = t + '/Skyscraper'
open(fake, 'w').write('''#!/usr/bin/env python3
import os, sys
a = sys.argv[1:]
log = os.environ['HOME'] + '/calls.log'
open(log, 'a').write(repr(a) + '\\n')
def opt(k):
    return a[a.index(k) + 1] if k in a else None
files = [x for x in a if x.startswith('/') and x.endswith('.sfc')]
if opt('--includefrom'):
    files = [l.strip() for l in open(opt('--includefrom')) if l.strip()]
known = {'Mario.sfc': 'Super Mario World', 'Zelda.sfc': 'Zelda', 'Quota.sfc': None}
if opt('-s'):
    for i, f in enumerate(files):
        print('#%d/%d ---- Game found ----' % (i + 1, len(files)), flush=True)
        print('#%d/%d, (%d/0)' % (i + 1, len(files), i + 1), flush=True)
        if f.endswith('Quota.sfc'):
            print('Your daily ScreenScraper request limit has been reached, exiting nicely...')
            sys.exit(0)
    sys.exit(0)
if opt('-f') == 'pegasus':
    g, o = opt('-g'), opt('-o')
    os.makedirs(o + '/covers', exist_ok=True); os.makedirs(o + '/screenshots', exist_ok=True)
    out = ['collection: snes', 'shortname: snes', 'launch: x']
    q = open(log).read()
    for f in files:
        b = os.path.basename(f)
        title = known.get(b)
        if b == 'Unknown.sfc' and "'--query', 'Super+Mario+Land'" in q:
            title = 'Super Mario Land'
        if not title:
            continue
        stem = b[:-4]
        open(o + '/covers/' + stem + '.png', 'wb').write(b'\\x89PNG\\r\\n\\x1a\\n' + b'c' * 300)
        open(o + '/screenshots/' + stem + '.png', 'wb').write(b'\\x89PNG\\r\\n\\x1a\\n' + b's' * 300)
        out += ['', 'game: ' + title, 'file: ' + f, 'rating: 90%',
                'description: First line', '  .', '  Second\uA789 paragraph',
                'release: 1990-11-21', 'developer: Nintendo EAD', 'publisher: Nintendo',
                'genre: Platform', 'players: 1-2',
                'assets.screenshot: ' + o + '/screenshots/' + stem + '.png',
                'assets.boxFront: ' + o + '/covers/' + stem + '.png']
    open(g + '/metadata.pegasus.txt', 'w').write('\\n'.join(out) + '\\n')
''')
os.chmod(fake, 0o755)

folder = 'Nintendo - Super Nintendo Entertainment System'
thumbs = ['Super Metroid (Europe)', 'Super Metroid (Japan, USA)', 'Super Metroid (USA) (Beta)', 'Mario Paint (Japan)']
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        p = unquote(self.path)
        if p == '/%s/Named_Boxarts/' % folder:
            body = '<html>' + ''.join('<a href="%s.png">x</a>' % quote(n) for n in thumbs) + '</html>'
            self.send_response(200); self.end_headers(); self.wfile.write(body.encode()); return
        for kind in ('Named_Boxarts', 'Named_Snaps'):
            pre = '/%s/%s/' % (folder, kind)
            if p.startswith(pre) and p[len(pre):-4] in thumbs:
                self.send_response(200); self.end_headers(); self.wfile.write(PNG); return
        self.send_response(404); self.end_headers()
srv = http.server.HTTPServer(('127.0.0.1', 0), H); port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

common = ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-DRUN_ROOT="%s"' % run]
subprocess.check_call(['gcc'] + common + ['-DUSERDATA_ROOT="%s"' % ud, '-DSHARE_ROOT="%s"' % share,
                       lib + '/libraryd.c', '-o', t + '/libraryd', '-lpthread'])
subprocess.check_call(['gcc'] + common + ['-Wno-format-truncation', '-DUSERDATA_ROOT="%s"' % ud,
    '-DSTATE_ROOT="%s"' % state, '-DSKYSCRAPER_BIN="%s"' % fake,
    '-DLIBRETRO_BASE="http://127.0.0.1:%d"' % port, src + '/scraper.c', '-o', t + '/scraper', '-lcurl'])
lp = subprocess.Popen([t + '/libraryd'], stderr=open(t + '/lib.log', 'w'))
fails = 0
def check(name, cond):
    global fails
    if not cond:
        print('[FAIL]', name); fails += 1
def sc(*args, stdin=None, env=None):
    e = dict(os.environ); e.update(env or {})
    return subprocess.run([t + '/scraper'] + list(args), input=stdin, capture_output=True, text=True, env=e).stdout
def job(*args):
    return sc(*args, env={'NUUBOS_JOB_ID': '1'})
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
def meta(stem):
    try: return open(ud + '/library/metadata/snes/%s.txt' % stem).read()
    except OSError: return ''
def calls():
    try: return open(run + '/scraper/calls.log').read()
    except OSError: return ''
try:
    for _ in range(50):
        if 'games=%d' % len(roms) in libcmd('STATUS'): break
        time.sleep(0.1)
    ids = {l.split('\t')[2]: l[5:21] for l in libcmd('GAMES\tsystem:snes').splitlines() if l.startswith('game=')}

    st = sc('provider', 'status')
    check('default source screenscraper, available', 'provider=screenscraper' in st and 'available=1' in st)
    check('account set', sc('account', 'set', 'me', stdin='p"w\n').startswith('OK'))
    check('account file root-only', (os.stat(state + '/scraper/screenscraper.conf').st_mode & 0o077) == 0)
    check('api key set', sc('apikey', 'set', stdin='abcdef123456\n').startswith('OK'))
    check('key shown by its end only', 'apikey=3456' in sc('account', 'status'))

    out = job('game', ids['Mario'])
    check('scraped through Skyscraper', '@result scraped' in out)
    m = meta('Mario')
    check('fields', 'title=Super Mario World' in m and 'developer=Nintendo EAD' in m and 'genre=Platform' in m
          and 'players=1-2' in m and 'release=1990-11-21' in m and 'provider=screenscraper' in m)
    check('rating 0-100', 'rating=90' in m)
    check('multi-line description', 'description=First line\\n\\nSecond: paragraph' in m)
    check('cover installed', open(ud + '/library/covers/snes/Mario.png', 'rb').read().startswith(b'\x89PNG'))
    check('screenshot installed', 'screenshot=' in m and os.path.exists(ud + '/library/screenshots/snes/Mario.png'))
    c = calls()
    check('gather + generate', "'-s', 'screenscraper'" in c and "'-f', 'pegasus'" in c and "'-p', 'snes'" in c)
    cfg = run + '/scraper/.skyscraper/config.ini'
    ini = open(cfg).read()
    check('account in tmpfs config only', 'userCreds="me:p\\"w"' in ini and (os.stat(cfg).st_mode & 0o077) == 0)
    check('key in config', '[thegamesdb]\nuserCreds="abcdef123456"' in ini)
    check('user language, The in front', 'lang="it"' in ini and 'theInFront="true"' in ini)
    check('no credentials on the command line', 'p"w' not in c and 'abcdef' not in c)
    check('scratch folder removed', not os.listdir(ud + '/library/cache/skyscraper-out'))
    d = libcmd('DETAILS\t' + ids['Mario'])
    check('libraryd shows metadata + cover', 'meta_genre=Platform' in d and '/library/covers/snes/Mario.png' in d)

    check('matched game skipped without force', 'matched' in job('game', ids['Mario']))
    check('not found', '@error not-found' in job('game', ids['Unknown']))
    check('search name stored', sc('search', ids['Unknown'], stdin='Super Mario Land\n').startswith('OK'))
    check('libraryd passes the search name', 'meta_search=Super Mario Land' in libcmd('DETAILS\t' + ids['Unknown']))
    out = job('game', ids['Unknown'])
    check('search name -> --query', "'--query', 'Super+Mario+Land'" in calls() and '@result scraped' in out
          and 'title=Super Mario Land' in meta('Unknown') and 'search=Super Mario Land' in meta('Unknown'))

    # Fill: what is there stays, what is missing comes back.
    m = meta('Mario').replace('title=Super Mario World', 'title=My Title')
    m = '\n'.join(l for l in m.splitlines() if not l.startswith(('description=', 'screenshot='))) + '\n'
    open(ud + '/library/metadata/snes/Mario.txt', 'w').write(m)
    os.unlink(ud + '/library/screenshots/snes/Mario.png')
    out = job('game', ids['Mario'], 'fill')
    m = meta('Mario')
    check('fill keeps existing data', 'title=My Title' in m)
    check('fill adds the missing', 'description=First line' in m and os.path.exists(ud + '/library/screenshots/snes/Mario.png'))
    check('complete game skipped by fill', 'complete' in job('game', ids['Mario'], 'fill'))
    out = job('game', ids['Mario'], 'force')
    check('update (force) refreshes', '@result scraped' in out and 'title=Super Mario World' in meta('Mario'))

    out = job('game', ids['Quota'])
    check('quota stops (job protocol)', '@error quota' in out)

    # libretro.
    check('switch source', sc('provider', 'set', 'libretro').startswith('OK') and 'provider=libretro' in sc('provider', 'status'))
    out = job('game', ids['Super Metroid'])
    m = meta('Super Metroid (Europe)')
    check('libretro exact dump name', '@result scraped' in out and 'provider=libretro' in m
          and os.path.exists(ud + '/library/covers/snes/Super Metroid (Europe).png'))
    check('libretro index cached', os.path.exists(ud + '/library/cache/libretro/%s.txt' % folder))
    sc('search', ids['Metroid'], stdin='Super Metroid\n')
    out = job('game', ids['Metroid'])
    check('libretro title match prefers a release (USA) over beta', 'Japan%2C%20USA' in meta('Metroid'))
    check('libretro not found', '@error not-found' in job('game', ids['Zelda']))
    out = job('bulk', 'missing')
    check('bulk missing runs', '@result' in out)

    check('reject', sc('reject', ids['Zelda']).startswith('OK') and os.path.exists(ud + '/library/metadata/snes/Zelda.rejected'))
    check('clear removes scraped media', sc('clear', ids['Mario']).startswith('OK')
          and not os.path.exists(ud + '/library/covers/snes/Mario.png')
          and not os.path.exists(ud + '/library/screenshots/snes/Mario.png') and meta('Mario') == '')
    sc('provider', 'set', 'screenscraper')
    out = job('bulk', 'all')
    check('bulk: one Skyscraper run per system, progress', '@progress' in out and "'--includefrom'" in calls())
    check('bulk stops on quota', '@error quota' in out)
    check('bulk applied games found before the stop', 'title=Super Mario World' in meta('Mario'))
    check('bulk skipped rejected game', meta('Zelda') == '')
    check('no partial files', not any(f.endswith(('.part', '.tmp')) for _, _, fs in os.walk(ud) for f in fs))
finally:
    lp.terminate(); lp.wait(); srv.shutdown()
print('[PASS] scraper' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
