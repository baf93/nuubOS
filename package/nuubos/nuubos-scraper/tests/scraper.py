#!/usr/bin/env python3
# Host test: nuubos-scraper against a fake Skyscraper (gather + Pegasus game
# list generation), a fake thumbnails.libretro.com and the real libraryd.
# Checks: Skyscraper arguments, account/key reach Skyscraper only through
# the tmpfs config (0600), results mapped into the store (fields, rating,
# multi-line description, cover, screenshot), search name -> --query, fill
# mode keeps existing data and adds the missing, update (force), quota stop,
# not found, libretro exact and title match with region preference,
# Cover Region (Skyscraper region, cache refresh and fill of covers of
# another region, libretro region first), reject/clear, no scratch or partial files left.
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
for d in (run + '/user', ud + '/roms/snes', ud + '/roms/psp/Daytona USA', ud + '/roms/psp/Jet Set Radio', share,
          state + '/users/' + user):
    os.makedirs(d, exist_ok=True)
open(share + '/systems.conf', 'w').write(open(lib + '/systems.conf').read())
open(run + '/user/active', 'w').write(user + '\n')
open(state + '/users/%s/localization.conf' % user, 'w').write('LANGUAGE=it\n')
roms = ['Mario.sfc', 'Zelda.sfc', 'Unknown.sfc', 'Quota.sfc', 'Super Metroid (Europe).sfc', 'Metroid.sfc',
        'Kirby (USA) [!] v1.1.sfc', 'Disney G-Force - 2009.sfc', 'Pups.sfc', 'Mario Paint (Japan).sfc']
for i, n in enumerate(roms):
    open(ud + '/roms/snes/' + n, 'wb').write(bytes([65 + i]) * (1000 + i))
# PSP ports in folders of their own (GitHub #29/#30): both called EBOOT.PBP.
for n in ('Daytona USA', 'Jet Set Radio'):
    open(ud + '/roms/psp/%s/EBOOT.PBP' % n, 'wb').write(b'P' * 2000)
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
files = [x for x in a if x.startswith('/') and x.endswith(('.sfc', '.PBP'))]
if opt('--includefrom'):
    files = [l.strip() for l in open(opt('--includefrom')) if l.strip()]
known = {'Mario.sfc': 'Super Mario World', 'Zelda.sfc': 'Zelda', 'Quota.sfc': None, 'Mario Paint (Japan).sfc': 'Mario Paint'}
if opt('-s'):
    for i, f in enumerate(files):
        print("#%d/%d (T1) ---- Game '%s' found! :) ----" % (i + 1, len(files), os.path.splitext(os.path.basename(f))[0]), flush=True)
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
        if b == 'Kirby (USA) [!] v1.1.sfc' and "'--query', 'Kirby'" in q:
            title = 'Kirby Super Star'
        if b == 'Disney G-Force - 2009.sfc' and "'--query', 'G-Force'" in q:
            title = 'G-Force'
        if b == 'Pups.sfc' and "'--query', '102+dalmatian'" in q:
            title = '102 Dalmatians'
        if f.endswith('Daytona USA/EBOOT.PBP') and opt('-p') == 'dreamcast' and "'--query', 'Daytona+USA'" in q:
            title = 'Daytona USA'
        if not title:
            continue
        stem = b[:-4]
        open(o + '/covers/' + stem + '.png', 'wb').write(b'\\x89PNG\\r\\n\\x1a\\n' + b'c' * 300)
        if b == 'Mario Paint (Japan).sfc':
            # A box spine (124x720), as ScreenScraper gave for Madagascar 2 DS.
            open(o + '/covers/' + stem + '.png', 'wb').write(b'\\x89PNG\\r\\n\\x1a\\n\\x00\\x00\\x00\\x0dIHDR'
                + (124).to_bytes(4, 'big') + (720).to_bytes(4, 'big') + b'c' * 300)
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
        if p.endswith('/Named_Boxarts/'):
            # Other systems' folders (the PSP ports): no thumbnails.
            self.send_response(200); self.end_headers(); self.wfile.write(b'<html></html>'); return
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
def meta(stem, system='snes'):
    try: return open(ud + '/library/metadata/%s/%s.txt' % (system, stem)).read()
    except OSError: return ''
def calls():
    try: return open(run + '/scraper/calls.log').read()
    except OSError: return ''
try:
    for _ in range(50):
        if 'games=%d' % (len(roms) + 2) in libcmd('STATUS'): break
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
    check('no screenshot', 'screenshot=' not in m and not os.path.exists(ud + '/library/screenshots'))
    c = calls()
    check('gather + generate', "'-s', 'screenscraper'" in c and "'-f', 'pegasus'" in c and "'-p', 'snes'" in c)
    check('screenshots not cached', 'noscreenshots' in c)
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
    kirby = next(v for k, v in ids.items() if k.startswith('Kirby'))
    out = job('game', kirby)
    check('missed file name retried by a cleaned name', "'--query', 'Kirby'" in calls()
          and '@result scraped' in out and 'title=Kirby Super Star' in meta('Kirby (USA) [!] v1.1')
          and 'search=' not in meta('Kirby (USA) [!] v1.1').replace('search=\n', ''))
    gforce = next(v for k, v in ids.items() if k.startswith('Disney G-Force'))
    out = job('game', gforce)
    check('retried without the year and the brand word', "'--query', 'Disney+G-Force'" in calls()
          and "'--query', 'G-Force'" in calls() and 'title=G-Force' in meta('Disney G-Force - 2009'))
    pups = next(v for k, v in ids.items() if k.startswith('Pups'))
    sc('search', pups, stdin="Disney's 102 dalmatian\n")
    out = job('game', pups)
    check('a failed search name is retried without the brand word', "'--query', \"Disney's+102+dalmatian\"" in calls()
          and "'--query', '102+dalmatian'" in calls() and 'title=102 Dalmatians' in meta('Pups'))
    paint = next(v for k, v in ids.items() if k.startswith('Mario Paint'))
    out = job('game', paint)
    cov = ud + '/library/covers/snes/Mario Paint (Japan).png'
    check('a box spine is not a cover: libretro cover instead', '@result scraped' in out and os.path.exists(cov)
          and open(cov, 'rb').read() == PNG and 'title=Mario Paint' in meta('Mario Paint (Japan)'))
    check('search name stored', sc('search', ids['Unknown'], stdin='Super Mario Land\n').startswith('OK'))
    check('libraryd passes the search name', 'meta_search=Super Mario Land' in libcmd('DETAILS\t' + ids['Unknown']))
    out = job('game', ids['Unknown'])
    check('search name -> --query', "'--query', 'Super+Mario+Land'" in calls() and '@result scraped' in out
          and 'title=Super Mario Land' in meta('Unknown') and 'search=Super Mario Land' in meta('Unknown'))

    # Fill: what is there stays, what is missing comes back.
    m = meta('Mario').replace('title=Super Mario World', 'title=My Title')
    m = '\n'.join(l for l in m.splitlines() if not l.startswith('description=')) + '\n'
    open(ud + '/library/metadata/snes/Mario.txt', 'w').write(m)
    out = job('game', ids['Mario'], 'fill')
    m = meta('Mario')
    check('fill keeps existing data', 'title=My Title' in m)
    check('fill adds the missing', 'description=First line' in m)
    check('complete game skipped by fill', 'complete' in job('game', ids['Mario'], 'fill'))
    out = job('game', ids['Mario'], 'force')
    check('update (force) refreshes', '@result scraped' in out and 'title=Super Mario World' in meta('Mario'))

    # Cover Region (GitHub #10).
    check('region set keeps the source', sc('region', 'set', 'eu').startswith('OK')
          and 'region=eu' in sc('provider', 'status') and 'provider=screenscraper' in sc('provider', 'status'))
    check('invalid region refused', sc('region', 'set', 'xx') == '' and 'region=eu' in sc('provider', 'status'))
    os.remove(run + '/scraper/calls.log')
    out = job('game', ids['Mario'], 'fill')
    check('cover of another region: fill asks the source again', '@result scraped' in out
          and "'--cache', 'refresh'" in calls() and 'cover_region=eu' in meta('Mario')
          and 'title=Super Mario World' in meta('Mario'))
    check('region in the Skyscraper config', 'region="eu"' in open(cfg).read())
    check('cover of the chosen region: complete', 'complete' in job('game', ids['Mario'], 'fill'))
    sc('region', 'set', 'auto')
    os.remove(run + '/scraper/calls.log')
    check('auto never asks again', 'complete' in job('game', ids['Mario'], 'fill'))
    job('game', ids['Mario'], 'force')
    check('auto: no region, no refresh', 'region=' not in open(cfg).read() and "'refresh'" not in calls())

    # Search System and a game's own Cover Region (GitHub #29), on PSP ports
    # named after their folders (GitHub #30).
    psp = {l.split('\t')[2]: l[5:21] for l in libcmd('GAMES\tsystem:psp').splitlines() if l.startswith('game=')}
    daytona = psp.get('Daytona USA')
    check('PSP ports named after their folders', sorted(psp) == ['Daytona USA', 'Jet Set Radio'])
    st = sc('provider', 'status')
    check('systems the source can search', any(l.startswith('systems=') and ',dreamcast,' in ',' + l[8:] + ','
          and ',psp,' in ',' + l[8:] + ',' for l in st.splitlines()))
    check('unknown search system refused', sc('search-system', daytona, 'nosuch').startswith('ERR'))
    check('search system stored', sc('search-system', daytona, 'dreamcast').startswith('OK')
          and 'meta_search_system=dreamcast' in libcmd('DETAILS\t' + daytona))
    sc('search', daytona, stdin='Daytona USA\n')
    os.remove(run + '/scraper/calls.log')
    out = job('game', daytona)
    m = meta('Daytona USA', 'psp')
    check('looked up as a Dreamcast game, stored as the PSP folder game', '@result scraped' in out
          and "'-p', 'dreamcast'" in calls() and 'title=Daytona USA' in m and 'search_system=dreamcast' in m
          and os.path.exists(ud + '/library/covers/psp/Daytona USA.png'))
    os.remove(run + '/scraper/calls.log')
    job('bulk', 'system:psp')
    c = calls()
    check('in a batch the game with its own system runs alone', "'-p', 'psp'" in c and "'--includefrom'" not in c)
    check('reject keeps the search system', sc('reject', daytona).startswith('OK')
          and 'search_system=dreamcast' in meta('Daytona USA', 'psp'))
    check('default clears it', sc('search-system', daytona, 'default').startswith('OK')
          and 'search_system=dreamcast' not in meta('Daytona USA', 'psp'))
    check('region of a game stored', sc('search-region', ids['Mario'], 'jp').startswith('OK')
          and 'search_region=jp' in meta('Mario'))
    job('game', ids['Mario'], 'force')
    check('its region in the Skyscraper config', 'region="jp"' in open(cfg).read() and 'cover_region=jp' in meta('Mario'))
    check('invalid region of a game refused', sc('search-region', ids['Mario'], 'xx').startswith('ERR'))
    sc('search-region', ids['Mario'], 'default')
    job('game', ids['Mario'], 'force')
    check('region of the game cleared', 'search_region=' not in meta('Mario') and 'region=' not in open(cfg).read())

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
    sc('region', 'set', 'eu')
    job('game', ids['Metroid'], 'force')
    check('libretro: the chosen region first', 'Super%20Metroid%20%28Europe%29' in meta('Metroid')
          and 'cover_region=eu' in meta('Metroid'))
    sc('region', 'set', 'auto')
    out = job('bulk', 'missing')
    check('bulk missing runs', '@result' in out)

    check('reject', sc('reject', ids['Zelda']).startswith('OK') and os.path.exists(ud + '/library/metadata/snes/Zelda.rejected'))
    check('clear removes scraped media', sc('clear', ids['Mario']).startswith('OK')
          and not os.path.exists(ud + '/library/covers/snes/Mario.png') and meta('Mario') == '')
    sc('provider', 'set', 'screenscraper')
    out = job('bulk', 'all')
    check('bulk: one Skyscraper run per system, progress', '@progress' in out and "'--includefrom'" in calls())
    pct = [int(l.split()[1]) for l in out.splitlines() if l.startswith('@progress ')]
    check('bulk progress never goes backwards', pct == sorted(pct))
    check('bulk progress names the game just done', any(l.startswith('@progress ') and l.endswith(' Mario') for l in out.splitlines()))
    check('bulk stops on quota', '@error quota' in out)
    check('bulk applied games found before the stop', 'title=Super Mario World' in meta('Mario'))
    check('bulk skipped rejected game', meta('Zelda') == '')
    check('no partial files', not any(f.endswith(('.part', '.tmp')) for _, _, fs in os.walk(ud) for f in fs))
finally:
    lp.terminate(); lp.wait(); srv.shutdown()
print('[PASS] scraper' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
