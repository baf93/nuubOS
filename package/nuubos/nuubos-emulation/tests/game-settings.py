#!/usr/bin/env python3
# Host test: nuubos-emud per-game overrides (EPIC-018), user-key restore,
# screenshots (EPIC-021) and achievements session keys (EPIC-017), driving
# the real libraryd and emud with a fake RetroArch that, like the real one,
# saves its whole configuration on exit.
# Run: python3 package/nuubos/nuubos-emulation/tests/game-settings.py
import os, socket, subprocess, sys, tempfile, time, threading

here = os.path.dirname(os.path.abspath(__file__))
pkg = os.path.join(here, '..', 'src')
lib = os.path.join(here, '..', '..', 'nuubos-library', 'src')
notify_h = os.path.join(here, '..', '..', 'nuubos-notify', 'src', 'notify.h')
t = tempfile.mkdtemp()
run, ud, share, state = t + '/run', t + '/userdata', t + '/share', t + '/state/users'
emu_share, cores = t + '/emushare', t + '/cores'
user = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
for d in (run + '/user', ud + '/roms/snes', share, emu_share + '/shaders', cores,
          state + '/' + user + '/secrets', t + '/inc/nuubos'):
    os.makedirs(d, exist_ok=True)
open(share + '/systems.conf', 'w').write(open(lib + '/systems.conf').read())
open(run + '/user/active', 'w').write(user + '\n')
open(ud + '/roms/snes/Mario.sfc', 'w').write('x' * 64)
open(emu_share + '/cores.conf', 'w').write('snes|snes9x,bsnes,missing\n')
for c in ('snes9x', 'bsnes'):
    open(cores + '/%s_libretro.so' % c, 'w').write('')
open(emu_share + '/retroarch.cfg', 'w').write(
    'aspect_ratio_index = "22"\nvideo_smooth = "false"\nvideo_shader_enable = "true"\n'
    'cheevos_enable = "false"\nmenu_driver = "ozone"\n')
open(emu_share + '/retroarch-core-options.cfg', 'w').write('')
open(emu_share + '/shaders/global.glslp', 'w').write('')
open(t + '/inc/nuubos/notify.h', 'w').write(open(notify_h).read())

fake = t + '/retroarch'
open(fake, 'w').write(r'''#!/bin/sh
cfg=""; app=""; core=""
while [ $# -gt 0 ]; do
  case "$1" in --config) cfg="$2"; shift;; --appendconfig) app="$2"; shift;; -L) core="$2"; shift;; esac
  shift
done
echo "$core" > "%s/launched-core"
cp "$app" "%s/last-session.cfg"
shots=$(sed -n 's/^screenshot_directory = "\(.*\)"/\1/p' "$app")
while read -r line; do
  case "$line" in
    SCREENSHOT) printf 'PNG' > "$shots/Mario-001.png" ;;
    QUIT) break ;;
  esac
done
# config_save_on_exit: the appended keys end up in the user file.
awk -F' = ' 'NR==FNR{v[$1]=$0; next} ($1 in v){print v[$1]; delete v[$1]; next} {print} END{for(k in v) print v[k]}' "$app" "$cfg" > "$cfg.new" && mv "$cfg.new" "$cfg"
exit 0
''' % (t, t))
os.chmod(fake, 0o755)

common = ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation',
          '-I' + t + '/inc', '-DRUN_ROOT="%s"' % run, '-DNUUBOS_NOTIFY_SOCKET="%s/notifyd.sock"' % run]
subprocess.check_call(['gcc'] + common + ['-DUSERDATA_ROOT="%s"' % ud, '-DSHARE_ROOT="%s"' % share,
                       lib + '/libraryd.c', '-o', t + '/libraryd', '-lpthread'])
subprocess.check_call(['gcc'] + common + ['-DUSERDATA="%s"' % ud, '-DSTATE_USERS="%s"' % state,
                       '-DSHARE_DIR="%s"' % emu_share, '-DBUNDLED_CORES="%s"' % cores,
                       '-DRETROARCH_BIN="%s"' % fake, pkg + '/emud.c', '-o', t + '/emud'])

notes = []
ns = socket.socket(socket.AF_UNIX); ns.bind(run + '/notifyd.sock'); ns.listen(16)
def notifyd():
    while True:
        try:
            c, _ = ns.accept()
        except OSError:
            return
        notes.append(c.recv(4096).decode()); c.close()
threading.Thread(target=notifyd, daemon=True).start()

procs = [subprocess.Popen([t + '/libraryd'], stderr=open(t + '/lib.log', 'w')),
         subprocess.Popen([t + '/emud'], stderr=open(t + '/emud.log', 'w'))]
fails = 0
def check(name, cond):
    global fails
    if not cond:
        print('[FAIL]', name); fails += 1

def cmd(sock, line, until=None):
    s = socket.socket(socket.AF_UNIX)
    for _ in range(50):
        try:
            s.connect(run + '/' + sock); break
        except OSError:
            time.sleep(0.1)
    s.sendall((line + '\n').encode()); s.settimeout(2); data = b''
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk: break
        data += chunk
        if data.endswith(b'end=1\n') or (until is None and data.endswith(b'\n') and not data.startswith((b'core=', b'state=', b'game=', b'user='))):
            break
    s.close(); return data.decode()

def wait(cond, secs=5):
    end = time.time() + secs
    while time.time() < end:
        if cond(): return True
        time.sleep(0.05)
    return False

try:
    wait(lambda: 'games=1' in cmd('libraryd.sock', 'STATUS'))
    game = [l for l in cmd('libraryd.sock', 'GAMES\tsystem:snes').splitlines() if l.startswith('game=')][0].split('=')[1].split('\t')[0]
    gs = cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game)
    check('defaults', 'core=snes9x\n' in gs and 'core_override=\n' in gs and 'cores=snes9x,bsnes\n' in gs)
    check('reject unknown core', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tcore\tmissing' % game).startswith('ERR'))
    check('reject bad aspect', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\t5:4' % game).startswith('ERR'))
    check('set core', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tcore\tbsnes' % game).startswith('OK'))
    check('set aspect', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\t4:3' % game).startswith('OK'))
    check('set filter', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tfilter\tpixel' % game).startswith('OK'))
    conf = open('%s/users/%s/appdata/nuubos-emulation/games/%s.conf' % (ud, user, game)).read()
    check('only overrides stored', conf == 'core=bsnes\naspect=4:3\nfilter=pixel\n')

    open(state + '/' + user + '/secrets/retroachievements.conf', 'w').write(
        'USERNAME=alice\nTOKEN=tok123\nENABLED=1\nHARDCORE=1\n')
    check('launch', cmd('emud.sock', 'LAUNCH\t' + game).startswith('OK'))
    check('core override used', wait(lambda: os.path.exists(t + '/launched-core')) and
          open(t + '/launched-core').read().strip().endswith('bsnes_libretro.so'))
    sess = open(t + '/last-session.cfg').read()
    check('aspect in session', 'aspect_ratio_index = "0"' in sess and 'video_smooth = "false"' in sess and 'video_shader_enable = "false"' in sess)
    check('achievements in session', 'cheevos_token = "tok123"' in sess and 'cheevos_hardcore_mode_enable = "true"' in sess)

    check('screenshot accepted', cmd('emud.sock', 'SCREENSHOT').startswith('OK'))
    check('screenshot confirmed', wait(lambda: any('game.screenshot.saved' in n for n in notes)))
    check('quit', cmd('emud.sock', 'QUIT').startswith('OK'))
    check('idle again', wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS')))
    ucfg = open('%s/users/%s/appdata/retroarch/retroarch.cfg' % (ud, user)).read()
    check('user aspect restored', 'aspect_ratio_index = "22"' in ucfg)
    check('user shader restored', 'video_shader_enable = "true"' in ucfg)
    check('token not left in USERDATA', 'tok123' not in ucfg)
    check('absent keys removed', 'cheevos_hardcore_mode_enable' not in ucfg and 'video_scale_integer' not in ucfg)
    check('unrelated keys kept', 'menu_driver = "ozone"' in ucfg)

    check('reset', cmd('emud.sock', 'GAME_RESET\t' + game).startswith('OK'))
    check('reset removes file', not os.path.exists('%s/users/%s/appdata/nuubos-emulation/games/%s.conf' % (ud, user, game)))
    check('inherited again', 'core=snes9x\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game))
finally:
    for p in procs: p.terminate(); p.wait()
    ns.close()
if fails:
    print(open(t + '/emud.log').read())
print('[PASS] emud game settings' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
