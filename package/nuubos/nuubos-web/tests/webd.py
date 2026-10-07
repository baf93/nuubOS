#!/usr/bin/env python3
# Host test: nuubos-webd session control against a fake Cog (stdout page
# reports), a fake cogctl (records the remote commands) and a fake
# nuubos-inputd (WEB ON connection, zoom HOTKEYs).
# Run: python3 package/nuubos/nuubos-web/tests/webd.py
import os, socket, subprocess, sys, tempfile, threading, time

here = os.path.dirname(os.path.abspath(__file__))
root = os.path.abspath(os.path.join(here, '..', '..', '..', '..'))
src = os.path.join(here, '..', 'src')
t = tempfile.mkdtemp()
run, users, state, cache = t + '/run', t + '/users', t + '/state', t + '/cache'
user = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
for d in (run + '/user', run + '/wayland-runtime', users + '/' + user, state + '/' + user, t + '/inc/nuubos'):
    os.makedirs(d, exist_ok=True)
open(run + '/user/active', 'w').write(user + '\n')
open(state + '/' + user + '/localization.conf', 'w').write('LANGUAGE=it\n')
open(t + '/inc/nuubos/notify.h', 'w').write(open(os.path.join(root, 'package/nuubos/nuubos-notify/src/notify.h')).read())

log = t + '/cogctl.log'
fake_cog = t + '/cog'
open(fake_cog, 'w').write('''#!/usr/bin/env python3
import os, signal, sys, time
open(os.environ["HOME"] + "/args", "w").write("\\n".join(sys.argv[1:]))
open(os.environ["HOME"] + "/env", "w").write("\\n".join("%s=%s" % kv for kv in os.environ.items()))
open(os.environ["HOME"] + "/pid", "w").write(str(os.getpid()))
if "crash.example" in sys.argv[-1]:
    os.kill(os.getpid(), signal.SIGSEGV)
signal.signal(signal.SIGTERM, lambda *a: sys.exit(0))
url = sys.argv[-1]
print("@nuubos uri=" + url, flush=True)
print("@nuubos title=Example\\tDomain", flush=True)
while True:
    time.sleep(0.1)
''')
os.chmod(fake_cog, 0o755)
fake_cogctl = t + '/cogctl'
open(fake_cogctl, 'w').write('''#!/bin/sh
echo "$*" >> %s
if [ "$2" = quit ]; then kill $(cat %s/users/%s/appdata/web/pid); fi
exit 0
''' % (log, t, user))
os.chmod(fake_cogctl, 0o755)

# Fake inputd: records commands, sends a zoom hotkey on WEB ON.
seen_input = []
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(run + '/inputd.sock'); srv.listen(4)
def inputd():
    while True:
        c, _ = srv.accept()
        f = c.makefile('rw')
        for line in f:
            seen_input.append(line.strip())
            if line.strip() == 'WEB ON':
                f.write('HOTKEY zoom_in\n'); f.flush()
        seen_input.append('CLOSED')
threading.Thread(target=inputd, daemon=True).start()

common = ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation', '-I' + t + '/inc']
subprocess.check_call(['gcc'] + common + ['-DRUN_ROOT="%s"' % run, '-DCOG_BIN="%s"' % fake_cog,
    '-DCOGCTL_BIN="%s"' % fake_cogctl, '-DUSERDATA_USERS="%s"' % users, '-DWEB_CACHE_ROOT="%s"' % cache,
    '-DSTATE_USERS="%s"' % state, '-DNUUBOS_NOTIFY_SOCKET="%s/notifyd.sock"' % run,
    src + '/webd.c', '-o', t + '/webd'])
subprocess.check_call(['gcc'] + common + ['-DRUN_ROOT="%s"' % run, src + '/webctl.c', '-o', t + '/webctl'])

fails = 0
def check(name, cond):
    global fails
    if not cond:
        print('[FAIL]', name); fails += 1
def wc(*a):
    return subprocess.run([t + '/webctl'] + list(a), capture_output=True, text=True).stdout
def status():
    out = wc('status')
    d = {}
    for l in out.splitlines():
        if '=' in l:
            k, v = l.split('=', 1)
            d.setdefault(k, []).append(v)
    return {k: (v[0] if len(v) == 1 else v) for k, v in d.items()}
def wait(cond, secs=5):
    end = time.time() + secs
    while time.time() < end:
        if cond(): return True
        time.sleep(0.05)
    return False
ctl = lambda: open(log).read() if os.path.exists(log) else ''
web = users + '/' + user + '/appdata/web'

d = subprocess.Popen([t + '/webd'], stderr=open(t + '/webd.log', 'w'))
try:
    wait(lambda: os.path.exists(run + '/webd.sock'))
    check('idle at start', status().get('state') == 'idle')
    check('commands need a session', wc('back').startswith('ERR no-session'))
    check('open a host name', wc('open', 'example.org').startswith('OK'))
    check('running', wait(lambda: status().get('state') == 'running'))
    wait(lambda: os.path.exists(web + '/env'))
    args = open(web + '/args').read().split('\n')
    check('https added', args[-1] == 'https://example.org')
    check('spatial navigation + sqlite cookies', '--enable-spatial-navigation=true' in args and '--cookie-jar=sqlite' in args)
    env = open(web + '/env').read()
    check('per-user data and cache', 'XDG_DATA_HOME=%s/data' % web in env and 'XDG_CACHE_HOME=%s/%s' % (cache, user) in env)
    check('user language', 'LANGUAGE=it' in env and 'COG_PLATFORM_WL_VIEW_FULLSCREEN=1' in env)
    check('inputd web mode', wait(lambda: 'WEB ON' in seen_input))
    check('zoom hotkey -> cogctl', wait(lambda: '--system zoom-in' in ctl()))
    check('page reported', wait(lambda: status().get('uri') == 'https://example.org' and status().get('title') == 'Example Domain'))
    check('history recorded', wait(lambda: status().get('history') == 'https://example.org\tExample Domain'))
    check('open in running session', wc('open', 'two words').startswith('OK') and
          '--system open https://duckduckgo.com/html/?q=two+words' in ctl())
    check('back/reload', wc('back').startswith('OK') and wc('reload').startswith('OK') and
          '--system previous' in ctl() and '--system reload' in ctl())
    check('bookmark the page', wc('bookmark-add').startswith('OK') and
          status().get('bookmark') == 'https://example.org\tExample Domain')
    check('bookmark by address', wc('bookmark-add', 'libretro.com', 'Libretro').startswith('OK'))
    check('newest bookmark first', status().get('bookmark', [''])[0] == 'https://libretro.com\tLibretro')
    check('clear data refused while running', wc('clear-data').startswith('ERR busy'))
    check('sleep keeps the session', wc('pre-power', 'sleep').startswith('OK') and status().get('state') == 'running')
    check('poweroff closes it', wc('pre-power', 'poweroff').startswith('OK') and status().get('state') == 'idle')
    check('inputd released', wait(lambda: 'CLOSED' in seen_input))
    check('remove bookmark', wc('bookmark-remove', 'https://libretro.com').startswith('OK') and
          status().get('bookmark') == 'https://example.org\tExample Domain')
    os.makedirs(web + '/data/cog', exist_ok=True); os.makedirs(cache + '/' + user, exist_ok=True)
    check('clear data', wc('clear-data').startswith('OK') and not os.path.exists(web + '/data/cog')
          and not os.path.exists(cache + '/' + user) and 'history' not in status()
          and 'bookmark' in status())
    check('crash reported', wc('open', 'crash.example').startswith('OK') and
          wait(lambda: status().get('state') == 'idle') and 'cog exited' in open(t + '/webd.log').read())
    os.unlink(run + '/user/active')
    check('no user', wc('open', 'example.org').startswith('ERR no-user'))
finally:
    d.terminate(); d.wait()
print('[PASS] webd' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
