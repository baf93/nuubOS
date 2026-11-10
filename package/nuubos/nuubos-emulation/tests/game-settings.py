#!/usr/bin/env python3
# Host test: nuubos-emud per-game overrides (EPIC-018), user-key restore,
# screenshots (EPIC-021), achievements session keys (EPIC-017), the user's
# emulation settings, slot rotation, resume/start modes and the session
# shader (pixel perfect, CRT, bezels), downloaded emulators, cheats and
# netplay (Host Game relaunch, LAN discovery), driving
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
for d in (run + '/user', ud + '/roms/snes', ud + '/roms/psx', share, emu_share + '/shaders', cores,
          state + '/' + user + '/secrets', t + '/inc/nuubos'):
    os.makedirs(d, exist_ok=True)
open(share + '/systems.conf', 'w').write(open(lib + '/systems.conf').read())
open(run + '/user/active', 'w').write(user + '\n')
open(ud + '/roms/snes/Mario.sfc', 'w').write('x' * 64)
open(ud + '/roms/psx/Crash.chd', 'w').write('x' * 64)
open(emu_share + '/cores.conf', 'w').write('snes|snes9x,bsnes,missing\npsx|pcsx_rearmed\nn64|parallel_n64_nuubos\ndreamcast|flycast_nuubos\n')
for c in ('snes9x', 'bsnes', 'pcsx_rearmed', 'parallel_n64_nuubos', 'flycast_nuubos'):
    open(cores + '/%s_libretro.so' % c, 'w').write('')
open(emu_share + '/retroarch.cfg', 'w').write(
    'aspect_ratio_index = "22"\nvideo_smooth = "false"\nvideo_shader_enable = "true"\n'
    'cheevos_enable = "false"\nmenu_driver = "ozone"\n')
open(emu_share + '/retroarch-core-options.cfg', 'w').write('')
open(emu_share + '/shaders/global.glslp', 'w').write('')
open(emu_share + '/display.conf', 'w').write('snes|crt\n')
open(emu_share + '/libretro.conf', 'w').write('snes|Nintendo - Super Nintendo Entertainment System\n')
# Core-info: snes9x and a downloaded core for SNES, another for GBA only.
info = t + '/info'
os.makedirs(info)
for c, db, name in (('snes9x', 'Nintendo - Super Nintendo Entertainment System', 'Snes9x'),
                    ('mesen-s', 'Nintendo - Super Nintendo Entertainment System|Nintendo - Game Boy', 'Mesen-S'),
                    ('gpsp', 'Nintendo - Game Boy Advance', 'gpSP'),
                    ('needsgl', 'Nintendo - Super Nintendo Entertainment System', 'Needs GL'),
                    ('listed', 'Nintendo - Super Nintendo Entertainment System', 'Listed')):
    open('%s/%s_libretro.info' % (info, c), 'w').write('corename = "%s"\ndatabase = "%s"\n' % (name, db))
# Downloaded cores are real shared objects (emud reads their libraries,
# core-check.h, GitHub #22): needsgl needs a library the system lacks,
# listed is in unsupported-cores.conf.
os.makedirs(ud + '/retroarch/cores')
open(t + '/core.c', 'w').write('int retro_api_version(void) { return 1; }\n')
open(t + '/libnuubosmissing.c', 'w').write('int nuubos_missing(void) { return 0; }\n')
subprocess.check_call(['gcc', '-shared', '-fPIC', t + '/libnuubosmissing.c', '-o', t + '/libnuubosmissing.so'])
for c in ('mesen-s', 'gpsp', 'listed'):
    subprocess.check_call(['gcc', '-shared', '-fPIC', t + '/core.c', '-o', ud + '/retroarch/cores/%s_libretro.so' % c])
subprocess.check_call(['gcc', '-shared', '-fPIC', t + '/core.c', '-L' + t, '-Wl,--no-as-needed', '-lnuubosmissing',
                       '-o', ud + '/retroarch/cores/needsgl_libretro.so'])
open(emu_share + '/unsupported-cores.conf', 'w').write('# comment\nlisted  # does not run\n')
# Cheats: the user's own file, the collection's of the same title.
coll = ud + '/cheats/libretro/Nintendo - Super Nintendo Entertainment System'
os.makedirs(ud + '/cheats/snes')
os.makedirs(ud + '/cheats/libretro')
cht = 'cheats = 3\n\ncheat0_desc = "Lives"\ncheat0_code = "7E0DBE:09"\ncheat0_enable = true\n\ncheat1_desc = "Coins"\ncheat1_code = "7E0DBF:63"\ncheat1_enable = false\n\ncheat2_code = "7E0019:02"\n'
# The collection: an archive and its index (nuubos-libretroctl); emud
# extracts the game's files.
import zipfile
with zipfile.ZipFile(ud + '/cheats/libretro/cheats.zip', 'w') as z:
    for n in ('Mario (USA).cht', 'Mario (USA) (Game Genie).cht', 'Mario (Demo).cht', 'Zelda (USA).cht'):
        z.writestr('Nintendo - Super Nintendo Entertainment System/' + n, cht)
    z.writestr('Nintendo - Game Boy/Mario (USA).cht', cht)
open(ud + '/cheats/libretro/index.txt', 'w').write(''.join(
    'Nintendo - Super Nintendo Entertainment System/%s\n' % n
    for n in ('Mario (USA).cht', 'Mario (USA) (Game Genie).cht', 'Mario (Demo).cht', 'Zelda (USA).cht'))
    + 'Nintendo - Game Boy/Mario (USA).cht\n')
open(ud + '/cheats/snes/Mario.cht', 'w').write(cht)
open(t + '/inc/nuubos/notify.h', 'w').write(open(notify_h).read())

fake = t + '/retroarch'
open(fake, 'w').write(r'''#!/bin/sh
echo "$*" > "%s/args"
cfg=""; app=""; core=""
for a in "$@"; do base=$a; done
base=${base##*/}; base=${base%%.*}
while [ $# -gt 0 ]; do
  case "$1" in --config) cfg="$2"; shift;; --appendconfig) app="$2"; shift;; -L) core="$2"; shift;; esac
  shift
done
echo "$core" > "%s/launched-core"
echo "${NUUBOS_RA_RENDER_HEIGHT:-}" > "%s/render-height"
echo "${NUUBOS_RA_BEZEL:-}" > "%s/bezel"
echo "${NUUBOS_RA_PIXEL_CROP:-}" > "%s/pixel-crop"
cp "$app" "%s/last-session.cfg"
grep -q -- '--host' "%s/args" && { echo "NETPLAY hosting"; echo "NETPLAY joined bob"; }
shots=$(sed -n 's/^screenshot_directory = "\(.*\)"/\1/p' "$app")
states=$(sed -n 's/^savestate_directory = "\(.*\)"/\1/p' "$app")
while read -r line; do
  case "$line" in
    SCREENSHOT) printf 'PNG' > "$shots/Mario-001.png" ;;
    CHEATS_LOAD*) echo "$line" >> "%s/cheats-loaded"; echo "CHEATS_LOAD OK" ;;
    NETPLAY_STOP) echo "NETPLAY_STOP OK"; echo "NETPLAY off" ;;
    SAVE_STATE_SLOT*) n=${line#SAVE_STATE_SLOT }; f="$states/$base.state$n"
      [ "$n" = 0 ] && f="$states/$base.state"; [ "$n" = -1 ] && f="$states/$base.state.auto"
      sleep 0.05; printf S > "$f"; echo "SAVE_STATE_SLOT $n OK" ;;
    QUIT) grep -q 'savestate_auto_save = "true"' "$app" && printf A > "$states/$base.state.auto"; break ;;
  esac
done
# config_save_on_exit: the appended keys end up in the user file.
awk -F' = ' 'NR==FNR{v[$1]=$0; next} ($1 in v){print v[$1]; delete v[$1]; next} {print} END{for(k in v) print v[k]}' "$app" "$cfg" > "$cfg.new" && mv "$cfg.new" "$cfg"
exit 0
''' % (t, t, t, t, t, t, t, t))
os.chmod(fake, 0o755)

common = ['-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-Wno-format-truncation',
          '-I' + t + '/inc', '-DRUN_ROOT="%s"' % run, '-DNUUBOS_NOTIFY_SOCKET="%s/notifyd.sock"' % run]
subprocess.check_call(['gcc'] + common + ['-DUSERDATA_ROOT="%s"' % ud, '-DSHARE_ROOT="%s"' % share,
                       lib + '/libraryd.c', '-o', t + '/libraryd', '-lpthread'])
subprocess.check_call(['gcc'] + common + ['-DUSERDATA="%s"' % ud, '-DSTATE_USERS="%s"' % state,
                       '-DSHARE_DIR="%s"' % emu_share, '-DBUNDLED_CORES="%s"' % cores,
                       '-DCORE_INFO_DIR="%s"' % info,
                       '-DRETROARCH_BIN="%s"' % fake, '-DHDMI_ENABLED="%s/hdmi-enabled"' % t, pkg + '/emud.c', '-o', t + '/emud'])

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
        if data.endswith(b'end=1\n') or (until is None and data.endswith(b'\n') and not data.startswith((b'core=', b'state=', b'game=', b'user=', b'slots=', b'boot_screens=', b'available=', b'host='))):
            break
    s.close(); return data.decode()

def wait(cond, secs=5):
    end = time.time() + secs
    while time.time() < end:
        if cond(): return True
        time.sleep(0.05)
    return False

try:
    wait(lambda: 'games=2' in cmd('libraryd.sock', 'STATUS'))
    game = [l for l in cmd('libraryd.sock', 'GAMES\tsystem:snes').splitlines() if l.startswith('game=')][0].split('=')[1].split('\t')[0]
    gs = cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game)
    check('defaults', 'core=snes9x\n' in gs and 'core_override=\n' in gs and 'cores=snes9x,bsnes,mesen-s\n' in gs)
    check('match file name', cmd('libraryd.sock', 'MATCH\tmario.SFC') == 'OK %s\tsnes\n' % game)
    check('match without extension', cmd('libraryd.sock', 'MATCH\tMario.zip') == 'OK %s\tsnes\n' % game)
    check('match none', cmd('libraryd.sock', 'MATCH\tZelda.sfc').startswith('ERR'))
    check('reject unknown core', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tcore\tmissing' % game).startswith('ERR'))
    check('reject core that cannot run', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tcore\tneedsgl' % game).startswith('ERR')
          and cmd('emud.sock', 'GAME_SET\t%s\tsnes\tcore\tlisted' % game).startswith('ERR'))
    check('reject bad aspect', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\t5:4' % game).startswith('ERR'))
    # Scaling a policy independent of aspect (shape), no
    # longer one of the aspect values ("integer" used to be).
    check('reject integer as aspect', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\tinteger' % game).startswith('ERR'))
    check('reject bad scaling', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tscaling\tsmart' % game).startswith('ERR'))
    check('set core', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tcore\tbsnes' % game).startswith('OK'))
    check('set aspect', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\t4:3' % game).startswith('OK'))
    check('set filter', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tfilter\tpixel' % game).startswith('OK'))
    # Emulation accuracy: a melonDS-nuubOS setting (stored for any game).
    check('accuracy not for snes9x', 'accuracy_supported=0\n' in gs and 'accuracy_default=fast\n' in gs)
    check('reject bad accuracy', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taccuracy\tturbo' % game).startswith('ERR'))
    check('set accuracy', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taccuracy\taccurate' % game).startswith('OK'))
    check('accuracy listed', 'accuracy=accurate\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game))
    check('clear accuracy', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taccuracy\t' % game).startswith('OK'))
    # Flycast: translucent sorting is its accuracy, per strip (Fast) by default.
    check('accuracy for flycast', 'accuracy_supported=1\naccuracy=\naccuracy_default=fast\n'
          in cmd('emud.sock', 'GAME_SETTINGS\t%s\tdreamcast' % game))
    # Nintendo 64 accessory (2026-10-21): Rumble Pak unless the game keeps
    # Controller Pak saves; only n64 games offer it.
    check('pak not for snes', 'pak_supported=0\n' in gs)
    check('pak offered for n64', 'pak_supported=1\npak=\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tn64' % game))
    check('reject bad pak', cmd('emud.sock', 'GAME_SET\t%s\tn64\tpak\ttransfer' % game).startswith('ERR'))
    check('set controller pak', cmd('emud.sock', 'GAME_SET\t%s\tn64\tpak\tmemory' % game).startswith('OK')
          and 'pak=memory\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tn64' % game))
    check('rumble is the default (stored empty)', cmd('emud.sock', 'GAME_SET\t%s\tn64\tpak\trumble' % game).startswith('OK')
          and 'pak=\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tn64' % game))
    # 3D render resolution (2026-10-27): the cores with a 1x/2x option.
    check('resolution not for snes', 'resolution_supported=0\n' in gs)
    check('resolution auto for psx', 'resolution_supported=1\nresolution=\nresolution_default=auto\n'
          in cmd('emud.sock', 'GAME_SETTINGS\t%s\tpsx' % game))
    check('resolution native default for n64', 'resolution_supported=1\nresolution=\nresolution_default=1x\n'
          in cmd('emud.sock', 'GAME_SETTINGS\t%s\tn64' % game))
    check('reject bad resolution', cmd('emud.sock', 'GAME_SET\t%s\tpsx\tresolution\t4x' % game).startswith('ERR'))
    check('set resolution', cmd('emud.sock', 'GAME_SET\t%s\tpsx\tresolution\t2x' % game).startswith('OK')
          and 'resolution=2x\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tpsx' % game))
    check('clear resolution', cmd('emud.sock', 'GAME_SET\t%s\tpsx\tresolution\t' % game).startswith('OK'))
    conf = open('%s/users/%s/appdata/nuubos-emulation/games/%s.conf' % (ud, user, game)).read()
    check('only overrides stored', conf == 'core=bsnes\naspect=4:3\nfilter=pixel\n')

    open(state + '/' + user + '/secrets/retroachievements.conf', 'w').write(
        'USERNAME=alice\nTOKEN=tok123\nENABLED=1\nHARDCORE=1\n')
    open(t + '/hdmi-enabled', 'w').write('enabled\n')
    # One-time core option updates: a key still at its old default moves to
    # the new one, a value the user chose is kept, each id runs once.
    open(emu_share + '/core-option-updates.conf', 'w').write(
        '# comment\nu1|opt_a|disabled|2\nu2|opt_b|disabled|enabled\n')
    radir = '%s/users/%s/appdata/retroarch' % (ud, user)
    os.makedirs(radir, exist_ok=True)
    open(radir + '/retroarch-core-options.cfg', 'w').write(
        'opt_a = "disabled"\nopt_b = "3"\nopt_c = "x"\n')
    check('launch', cmd('emud.sock', 'LAUNCH\t' + game).startswith('OK'))
    opts = open(radir + '/retroarch-core-options.cfg').read()
    check('core option update applied', 'opt_a = "2"\n' in opts)
    check('user core option kept', 'opt_b = "3"\n' in opts and 'opt_c = "x"\n' in opts)
    check('core option updates recorded', open(radir + '/core-option-updates').read() == 'u1\nu2\n')
    check('core override used', wait(lambda: os.path.exists(t + '/launched-core')) and
          open(t + '/launched-core').read().strip().endswith('bsnes_libretro.so'))
    sess = open(t + '/last-session.cfg').read()
    check('aspect in session', 'aspect_ratio_index = "0"' in sess and 'video_smooth = "false"' in sess and 'video_shader_enable = "false"' in sess)
    check('achievements in session', 'cheevos_token = "tok123"' in sess and 'cheevos_hardcore_mode_enable = "true"' in sess)
    # Hardcore: no save states, no fast-forward, no state at start or quit.
    check('hardcore no states', 'savestate_auto_save = "false"' in sess and 'savestate_auto_load = "false"' in sess
          and '--entryslot' not in open(t + '/args').read())
    check('hardcore in status', 'hardcore=1\n' in cmd('emud.sock', 'STATUS'))
    seen = len(notes)
    for line in ('SAVE_STATE', 'LOAD_STATE', 'SLOT\t+1', 'FAST_FORWARD'):
        check('hardcore refuses ' + line, cmd('emud.sock', line) == 'ERR hardcore\n')
    check('hardcore notified', wait(lambda: sum('game.hardcore' in n for n in notes[seen:]) == 4))
    check('hardcore STATES', 'hardcore=1\n' in cmd('emud.sock', 'STATES') and 'state=' not in cmd('emud.sock', 'STATES'))

    check('HDMI render height', open(t + '/render-height').read() == '720\n')
    check('screenshot accepted', cmd('emud.sock', 'SCREENSHOT').startswith('OK'))
    check('screenshot confirmed', wait(lambda: any('game.screenshot.saved' in n for n in notes)))
    check('quit', cmd('emud.sock', 'QUIT').startswith('OK'))
    check('idle again', wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS')))
    opts = open(radir + '/retroarch-core-options.cfg').read()
    open(radir + '/retroarch-core-options.cfg', 'w').write(opts.replace('opt_a = "2"', 'opt_a = "disabled"'))
    hsdir = '%s/users/%s/states/snes/' % (ud, user)
    check('hardcore quit saves no state', not os.listdir(hsdir))
    check('hardcore STATES by game', 'hardcore=1\n' in cmd('emud.sock', 'STATES\t' + game))
    check('hardcore refuses slot launch', cmd('emud.sock', 'LAUNCH\t%s\tslot:0' % game) == 'ERR hardcore\n')
    open(t + '/hdmi-enabled', 'w').write('disabled\n')
    ucfg = open('%s/users/%s/appdata/retroarch/retroarch.cfg' % (ud, user)).read()
    check('user aspect restored', 'aspect_ratio_index = "22"' in ucfg)
    check('user shader restored', 'video_shader_enable = "true"' in ucfg)
    check('token not left in USERDATA', 'tok123' not in ucfg)
    check('absent keys removed', 'cheevos_hardcore_mode_enable' not in ucfg and 'video_scale_integer' not in ucfg)
    check('unrelated keys kept', 'menu_driver = "ozone"' in ucfg)
    open(state + '/' + user + '/secrets/retroachievements.conf', 'w').write(
        'USERNAME=alice\nTOKEN=tok123\nENABLED=1\nHARDCORE=0\n')

    # PS1 resolution Auto: 1x on the panel, 2x on HDMI, a game's choice
    # wins; the user's core option comes back after the session.
    psx = [l for l in cmd('libraryd.sock', 'GAMES\tsystem:psx').splitlines() if l.startswith('game=')][0].split('=')[1].split('\t')[0]
    copts = radir + '/retroarch-core-options.cfg'
    def psx_session(hdmi):
        open(t + '/hdmi-enabled', 'w').write('enabled\n' if hdmi else 'disabled\n')
        ok = cmd('emud.sock', 'LAUNCH\t' + psx).startswith('OK')
        ok = ok and wait(lambda: 'state=running' in cmd('emud.sock', 'STATUS'))
        m = [l for l in open(copts).read().splitlines() if l.startswith('pcsx_rearmed_neon_enhancement_enable ')]
        cmd('emud.sock', 'QUIT')
        ok = ok and wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
        return ok and m[0].split('"')[1] if m else None
    check('psx auto 1x on the panel', psx_session(False) == 'disabled')
    check('psx auto 2x on HDMI', psx_session(True) == 'enabled')
    check('psx user option restored', 'pcsx_rearmed_neon_enhancement_enable = "enabled"\n' not in open(copts).read())
    cmd('emud.sock', 'GAME_SET\t%s\tpsx\tresolution\t1x' % psx)
    check('psx game choice wins on HDMI', psx_session(True) == 'disabled')
    cmd('emud.sock', 'GAME_RESET\t' + psx)
    open(t + '/hdmi-enabled', 'w').write('disabled\n')

    # Frameskip (GitHub #23): Off / Auto / Max per game and per system,
    # written for the session only; the user's core options come back.
    def psx_frameskip():
        ok = cmd('emud.sock', 'LAUNCH\t' + psx).startswith('OK')
        ok = ok and wait(lambda: 'state=running' in cmd('emud.sock', 'STATUS'))
        opts = dict(l.split(' = ', 1) for l in open(copts).read().splitlines() if ' = ' in l)
        cmd('emud.sock', 'QUIT')
        ok = ok and wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
        return ok and (opts.get('pcsx_rearmed_frameskip_type'), opts.get('pcsx_rearmed_frameskip_interval'))
    check('frameskip not for snes', 'frameskip_supported=0\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game))
    check('frameskip psx default off', 'frameskip_supported=1\nframeskip=\nframeskip_default=off\n'
          in cmd('emud.sock', 'GAME_SETTINGS\t%s\tpsx' % psx))
    check('reject bad frameskip', cmd('emud.sock', 'GAME_SET\t%s\tpsx\tframeskip\t5' % psx).startswith('ERR'))
    check('no frameskip choice keeps the user options', psx_frameskip() == (None, None))
    cmd('emud.sock', 'GAME_SET\t%s\tpsx\tframeskip\tmax' % psx)
    check('frameskip max in the session', psx_frameskip() == ('"auto"', '"3"'))
    check('frameskip user options restored', 'pcsx_rearmed_frameskip_type = "disabled"' in open(copts).read()
          and 'frameskip_default=off\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tpsx' % psx))
    cmd('emud.sock', 'GAME_SET\t%s\tpsx\tframeskip\tauto' % psx)
    check('frameskip auto in the session', psx_frameskip() == ('"auto"', '"1"'))
    cmd('emud.sock', 'GAME_RESET\t' + psx)
    cmd('emud.sock', 'SYSTEM_SET\tpsx\tframeskip\tmax')
    check('frameskip follows the system', 'system_frameskip=max\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tpsx' % psx)
          and psx_frameskip() == ('"auto"', '"3"'))
    cmd('emud.sock', 'SYSTEM_RESET\tpsx')

    # System Settings (GitHub #12): a game's unset key follows its system.
    sysdir = '%s/users/%s/appdata/nuubos-emulation/systems' % (ud, user)
    ss = cmd('emud.sock', 'SYSTEM_SETTINGS\tpsx')
    check('system defaults', 'core=pcsx_rearmed\n' in ss and 'resolution=\n' in ss and 'games=0\n' in ss)
    check('system rejects bad value', cmd('emud.sock', 'SYSTEM_SET\tpsx\tresolution\t4x').startswith('ERR'))
    check('system rejects bad name', cmd('emud.sock', 'SYSTEM_SETTINGS\t../x').startswith('ERR'))
    check('system set', cmd('emud.sock', 'SYSTEM_SET\tpsx\tresolution\t2x').startswith('OK')
          and cmd('emud.sock', 'SYSTEM_SET\tpsx\taspect\t16:9').startswith('OK')
          and open(sysdir + '/psx.conf').read() == 'aspect=16:9\nresolution=2x\n')
    gs = cmd('emud.sock', 'GAME_SETTINGS\t%s\tpsx' % psx)
    check('game lists its system values', 'resolution=\n' in gs and 'system_resolution=2x\n' in gs
          and 'system_aspect=16:9\n' in gs and 'aspect=\n' in gs)
    check('game follows its system', psx_session(False) == 'enabled'
          and 'aspect_ratio_index = "1"' in open(t + '/last-session.cfg').read())
    cmd('emud.sock', 'GAME_SET\t%s\tpsx\tresolution\t1x' % psx)
    check('game choice wins over the system', psx_session(False) == 'disabled')
    check('system counts the games with their own', 'games=1\n' in cmd('emud.sock', 'SYSTEM_SETTINGS\tpsx'))
    cmd('emud.sock', 'GAME_SET\t%s\tpsx\tcpu_level\t2' % psx)
    open('%s/users/%s/appdata/nuubos-emulation/games/%s.conf' % (ud, user, psx), 'a').write('nds_layout=hybrid\n')
    check('system resets its games', cmd('emud.sock', 'SYSTEM_RESET_GAMES\tpsx') == 'OK 1\n'
          and 'games=0\n' in cmd('emud.sock', 'SYSTEM_SETTINGS\tpsx')
          and open('%s/users/%s/appdata/nuubos-emulation/games/%s.conf' % (ud, user, psx)).read() == 'nds_layout=hybrid\n')
    snes_ss = cmd('emud.sock', 'SYSTEM_SET\tsnes\tcore\tbsnes')
    check('system emulator is the game default', snes_ss.startswith('OK')
          and 'core_default=bsnes\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game))
    check('other system untouched', 'system_core=\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tn64' % game))
    check('system reset', cmd('emud.sock', 'SYSTEM_RESET\tpsx').startswith('OK')
          and cmd('emud.sock', 'SYSTEM_RESET\tsnes').startswith('OK') and os.listdir(sysdir) == [])
    cmd('emud.sock', 'GAME_RESET\t' + psx)
    open(t + '/hdmi-enabled', 'w').write('disabled\n')

    # Controls (user request 2026-10-29): per system, per game, sent to
    # inputd as REMAP after the hotkeys.
    input_lines = []
    isrv = socket.socket(socket.AF_UNIX)
    isrv.bind(run + '/inputd.sock')
    isrv.listen(8)
    def input_conn(conn):
        f = conn.makefile('rw')
        for line in f:
            input_lines.append(line.strip())
            if line.startswith('GAMEPADS ON'):
                f.write('OK\n'); f.flush()
            elif line.startswith('BIND GET'):
                f.write('304\n'); f.flush()
    def fake_inputd():
        while True:
            try:
                conn, _ = isrv.accept()
            except OSError:
                return
            threading.Thread(target=input_conn, args=(conn,), daemon=True).start()
    threading.Thread(target=fake_inputd, daemon=True).start()
    def remap_sent():
        del input_lines[:]
        psx_session(False)
        return [l for l in input_lines if l.startswith('REMAP')]
    gs = cmd('emud.sock', 'GAME_SETTINGS\t%s\tpsx' % psx)
    check('controls: system pad listed', 'pad=menu_confirm:#1459,menu_back:#1460,' in gs and 'remap=\n' in gs)
    check('controls: generic pad', 'pad=menu_back:A,menu_confirm:B,face_north:X,' in cmd('emud.sock', 'SYSTEM_SETTINGS\tcps2'))
    check('controls: none by default', remap_sent() == ['REMAP'])
    check('controls: bad values refused', cmd('emud.sock', 'SYSTEM_SET\tpsx\tremap_l1\tquick_menu').startswith('ERR')
          and cmd('emud.sock', 'SYSTEM_SET\tpsx\tremap_x\tl1').startswith('ERR'))
    check('controls: system set', cmd('emud.sock', 'SYSTEM_SET\tpsx\tremap_face_west\tmenu_confirm').startswith('OK')
          and cmd('emud.sock', 'SYSTEM_SET\tpsx\tremap_l3\tnone').startswith('OK')
          and open(sysdir + '/psx.conf').read() == 'remap_face_west=menu_confirm\nremap_l3=none\n')
    gs = cmd('emud.sock', 'GAME_SETTINGS\t%s\tpsx' % psx)
    check('controls: game lists the system', 'system_remap=face_west:menu_confirm,l3:none\n' in gs and 'remap=\n' in gs)
    check('controls: game follows its system', remap_sent() == ['REMAP face_west:menu_confirm,l3:none'])
    cmd('emud.sock', 'GAME_SET\t%s\tpsx\tremap_face_west\tface_west' % psx)
    cmd('emud.sock', 'GAME_SET\t%s\tpsx\tremap_face_north\tmenu_back' % psx)
    check('controls: game wins', remap_sent() == ['REMAP face_north:menu_back,face_west:face_west,l3:none'])
    check('controls: system counts the game', 'games=1\n' in cmd('emud.sock', 'SYSTEM_SETTINGS\tpsx'))
    check('controls: reset all', cmd('emud.sock', 'GAME_SET\t%s\tpsx\tremap\t' % psx).startswith('OK')
          and remap_sent() == ['REMAP face_west:menu_confirm,l3:none'])
    cmd('emud.sock', 'SYSTEM_RESET\tpsx')
    isrv.close()
    os.unlink(run + '/inputd.sock')

    check('reset', cmd('emud.sock', 'GAME_RESET\t' + game).startswith('OK'))
    check('reset removes file', not os.path.exists('%s/users/%s/appdata/nuubos-emulation/games/%s.conf' % (ud, user, game)))
    check('inherited again', 'core=snes9x\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game))

    # User emulation settings.
    st0 = cmd('emud.sock', 'SETTINGS')
    check('settings defaults', 'slots=10\n' in st0 and 'slot_rotation=1\n' in st0 and 'crt=1\n' in st0
          and 'hotkey_save_state=r1\n' in st0 and 'hotkey_quit=settings\n' in st0
          and 'position=center\n' in st0)
    check('reject position', cmd('emud.sock', 'SET\tposition\tleft').startswith('ERR'))
    check('reject slots', cmd('emud.sock', 'SET\tslots\t0').startswith('ERR'))
    check('reject hotkey', cmd('emud.sock', 'SET\thotkey_quit\tquick_menu').startswith('ERR'))
    check('reject overlay item', cmd('emud.sock', 'SET\toverlay_items\tfps,disk').startswith('ERR'))
    check('set slots', cmd('emud.sock', 'SET\tslots\t3').startswith('OK'))
    check('set hotkey', cmd('emud.sock', 'SET\thotkey_quit\tl3').startswith('OK'))
    check('settings stored', 'hotkey_quit=l3\n' in cmd('emud.sock', 'SETTINGS'))
    check('crt filter default', 'filter_default=crt\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game))
    check('set crt off', cmd('emud.sock', 'SET\tcrt\t0').startswith('OK'))
    check('sharp filter default', 'filter_default=sharp\n' in cmd('emud.sock', 'GAME_SETTINGS\t%s\tsnes' % game))

    # Pixel perfect, start from the beginning, rotation.
    def launch(mode=''):
        for f in ('/args', '/last-session.cfg'):
            if os.path.exists(t + f): os.remove(t + f)
        ok = cmd('emud.sock', 'LAUNCH\t' + game + ('\t' + mode if mode else '')).startswith('OK')
        return ok and wait(lambda: os.path.exists(t + '/last-session.cfg') and os.path.exists(t + '/args'))
    def session():
        return open(t + '/last-session.cfg').read()
    def args():
        return open(t + '/args').read()
    def idle_state():
        return wait(lambda: 'state_busy=0' in cmd('emud.sock', 'STATUS'))
    sdir = '%s/users/%s/states/snes/' % (ud, user)
    # Start from no states.
    for f in os.listdir(sdir): os.remove(sdir + f)
    # Bezels are on by default (2026-10-15); the presentation checks below
    # are the plain picture.
    check('bezels off', cmd('emud.sock', 'SET\tbezels\t0').startswith('OK'))
    check('launch new', launch('new'))
    check('panel renders native', wait(lambda: open(t + '/render-height').read() == '\n'))
    sess = session()
    check('integer scaling, centred', 'aspect_ratio_index = "22"' in sess and 'video_scale_integer = "true"' in sess
          and 'video_viewport_bias_y = "0.500000"' in sess and 'video_shader_enable = "true"' in sess
          and open(t + '/pixel-crop').read() == '\n')
    # Slot rotation (default): emud saves the next slot at quit, not RetroArch.
    check('no auto save with rotation, no auto load', 'savestate_auto_save = "false"' in sess and 'savestate_auto_load = "false"' in sess)
    check('RetroArch notifications off', 'notification_show_save_state = "false"' in sess
          and 'notification_show_screenshot_flash = "2"' in sess and 'menu_show_load_content_animation = "false"' in sess)
    check('no entry slot, sharp shader', '--entryslot' not in args()
          and '--set-shader %s/shaders/global.glslp -L' % emu_share in args())
    slots = []
    for _ in range(4):
        check('save', cmd('emud.sock', 'SAVE_STATE').startswith('OK'))
        idle_state()
        slots.append([l for l in cmd('emud.sock', 'STATUS').splitlines() if l.startswith('slot=')][0])
    check('rotation wraps', slots == ['slot=0', 'slot=1', 'slot=2', 'slot=0'])
    sts = cmd('emud.sock', 'STATES')
    check('states listed', 'slots=3\n' in sts and all('state=%d\t' % n in sts for n in (0, 1, 2)))
    before = os.path.getmtime(sdir + 'Mario.state1')
    seen = len(notes)
    time.sleep(1.1)  # resume compares state mtimes in seconds
    check('quit accepted', cmd('emud.sock', 'QUIT').startswith('OK'))
    check('idle', wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS')))
    check('quit saved the next slot', os.path.getmtime(sdir + 'Mario.state1') > before
          and not os.path.exists(sdir + 'Mario.state.auto'))
    check('quit save quiet', not any('game.state.saved' in n for n in notes[seen:]))

    # Sleep with rotation: the next slot, the game keeps running.
    check('launch resume', launch())
    check('resume from the quit slot', '--entryslot 1 ' in args())
    before = os.path.getmtime(sdir + 'Mario.state2')
    time.sleep(1.1)
    check('pre-power sleep', cmd('emud.sock', 'PRE_POWER\tsleep').startswith('OK'))
    check('sleep saved the next slot', os.path.getmtime(sdir + 'Mario.state2') > before
          and 'state=running' in cmd('emud.sock', 'STATUS'))
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))

    # Without rotation RetroArch keeps the automatic state.
    check('rotation off', cmd('emud.sock', 'SET\tslot_rotation\t0').startswith('OK'))
    check('launch, rotation off', launch())
    check('auto save without rotation', 'savestate_auto_save = "true"' in session())
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    check('auto state written', os.path.exists(sdir + 'Mario.state.auto'))
    check('states by game', 'state=-1\t' in cmd('emud.sock', 'STATES\t' + game))
    check('launch resume auto', launch())
    check('resume from auto', 'savestate_auto_load = "true"' in session() and '--entryslot' not in args())
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    cmd('emud.sock', 'SET\tslot_rotation\t1')
    future = time.time() + 100
    os.utime(sdir + 'Mario.state1', (future, future))
    check('launch resume slot', launch())
    check('resume from slot', '--entryslot 1 ' in args() and 'savestate_auto_load = "false"' in session()
          and 'slot=1\n' in cmd('emud.sock', 'STATUS'))
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    check('reject missing state', cmd('emud.sock', 'LAUNCH\t%s\tslot:-2' % game).startswith('ERR'))

    # Picture Position (GitHub #13) and per-game Square Pixels (#15).
    check('set position top', cmd('emud.sock', 'SET\tposition\ttop').startswith('OK'))
    launch('new')
    check('picture at the top', 'video_viewport_bias_y = "0.000000"' in session()
          and 'video_viewport_bias_portrait_y = "0.000000"' in session())
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    check('set position bottom', cmd('emud.sock', 'SET\tposition\tbottom').startswith('OK')
          and 'position=bottom\n' in cmd('emud.sock', 'SETTINGS'))
    check('set square pixels', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\tsquare' % game).startswith('OK'))
    launch('new')
    sess = session()
    check('square pixels', 'aspect_ratio_index = "21"' in sess and 'video_scale_integer = "true"' in sess
          and 'video_viewport_bias_y = "1.000000"' in sess and open(t + '/pixel-crop').read() == '8\n'
          and open(t + '/bezel').read() == '\n')
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\t' % game)
    # Scaling policy, independent of aspect (shape): combines freely
    # (2026-10-30), e.g. a shape ("4:3") was never expressible with true
    # Integer Scaling before this split, since "aspect" alone used to pick
    # both shape and policy as a single choice.
    check('set 4:3 + integer', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\t4:3' % game).startswith('OK')
          and cmd('emud.sock', 'GAME_SET\t%s\tsnes\tscaling\tinteger' % game).startswith('OK'))
    launch('new')
    check('4:3 shape with integer policy', 'aspect_ratio_index = "0"' in session()
          and 'video_scale_integer = "true"' in session() and 'video_scale_integer_scaling = "0"' in session()
          and 'video_crop_overscan = "true"' in session())
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    check('set core shape + overscale', cmd('emud.sock', 'GAME_SET\t%s\tsnes\taspect\t' % game).startswith('OK')
          and cmd('emud.sock', 'GAME_SET\t%s\tsnes\tscaling\toverscale' % game).startswith('OK'))
    launch('new')
    # Overscale already crops a little to reach its bigger clean factor
    # RetroArch's own global Crop Overscan would needlessly
    # crop more on top of that, so it is turned off only here.
    check('core shape with overscale policy', 'aspect_ratio_index = "22"' in session()
          and 'video_scale_integer = "true"' in session() and 'video_scale_integer_scaling = "1"' in session()
          and 'video_crop_overscan = "false"' in session())
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    check('set scaling off', cmd('emud.sock', 'GAME_SET\t%s\tsnes\tscaling\toff' % game).startswith('OK'))
    launch('new')
    check('scaling off', 'video_scale_integer = "false"' in session())
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    cmd('emud.sock', 'GAME_SET\t%s\tsnes\tscaling\t' % game)
    cmd('emud.sock', 'SET\tposition\tcenter')

    # CRT look and bezels.
    check('set crt', cmd('emud.sock', 'SET\tcrt\t1').startswith('OK'))
    launch('new')
    glslp = open(run + '/emulation/session.glslp').read()
    check('crt shader', 'crt.glsl' in glslp and '--set-shader %s/emulation/session.glslp' % run in args())
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    check('set bezels', cmd('emud.sock', 'SET\tbezels\t1').startswith('OK'))
    launch('new')
    glslp = open(run + '/emulation/session.glslp').read()
    # RetroArch draws the bezel (patch 0010); the picture keeps its shader.
    bz = open(t + '/bezel').read().split()
    check('bezel', 'crt.glsl' in glslp and 'aspect_ratio_index = "22"' in session()
          and len(bz) == 15 and bz[3] == '0' and bz[4] == '-' and bz[5].endswith('/bezel-logo.png'))
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    # Cheats.
    cmd('emud.sock', 'SET\tbezels\t0')
    check('cheats need a game', cmd('emud.sock', 'CHEATS').startswith('ERR no-session'))
    if os.path.exists(t + '/cheats-loaded'):
        os.remove(t + '/cheats-loaded')
    launch('new')
    # RetroArch's own saved cheats of the game are cleared without the user's.
    check('no cheats cleared at launch', wait(lambda: os.path.exists(t + '/cheats-loaded'))
          and open(t + '/cheats-loaded').read() == 'CHEATS_LOAD\n')
    ch = cmd('emud.sock', 'CHEATS')
    lists = [l.split('=', 1)[1].split('\t')[1] for l in ch.splitlines() if l.startswith('list=')]
    check('cheat lists ranked', lists == ['Mario', 'Mario (USA)', 'Mario (USA) (Game Genie)', 'Mario (Demo)'])
    check('game lists extracted', os.path.exists(coll + '/Mario (USA) (Game Genie).cht')
          and not os.path.exists(coll + '/Zelda (USA).cht')
          and not os.path.exists(ud + '/cheats/libretro/Nintendo - Game Boy'))
    check('cheats of the best list', 'available=1\n' in ch and 'file=%s/cheats/snes/Mario.cht\n' % ud in ch
          and 'cheat=0\t0\tLives\n' in ch and 'cheat=1\t0\tCoins\n' in ch and 'cheat=2\t0\t\n' in ch)
    check('cheat on', cmd('emud.sock', 'CHEAT_SET\t1\t1') == 'OK\n')
    sc = open(run + '/emulation/session.cht').read()
    check('session cheats', 'cheat1_enable = "true"' in sc and 'cheat0_enable' not in sc and 'cheat0_code' in sc)
    check('cheats loaded', wait(lambda: os.path.exists(t + '/cheats-loaded') and
          'CHEATS_LOAD %s/emulation/session.cht' % run in open(t + '/cheats-loaded').read()))
    check('cheats count', 'cheats_on=1\n' in cmd('emud.sock', 'STATUS'))
    check('cheat shown on', 'cheat=1\t1\tCoins\n' in cmd('emud.sock', 'CHEATS'))
    conf = open('%s/users/%s/appdata/nuubos-emulation/games/%s.conf' % (ud, user, game)).read()
    check('cheats stored', 'cheat_file=%s/cheats/snes/Mario.cht\n' % ud in conf and 'cheats=1\n' in conf)
    check('reject other list', cmd('emud.sock', 'CHEAT_FILE\t%s/Zelda (USA).cht' % coll) == 'ERR list\n')
    check('pick list', cmd('emud.sock', 'CHEAT_FILE\t%s/Mario (USA).cht' % coll) == 'OK\n')
    check('picked list clears', 'cheats_on=0\n' in cmd('emud.sock', 'STATUS')
          and 'file=%s/Mario (USA).cht\n' % coll in cmd('emud.sock', 'CHEATS'))
    check('cheat on again', cmd('emud.sock', 'CHEAT_SET\t0\t1') == 'OK\n')
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    os.remove(t + '/cheats-loaded')
    # A new collection removed the extracted files: the chosen one comes back.
    os.remove(coll + '/Mario (USA).cht')
    launch('new')
    check('cheats at launch', wait(lambda: os.path.exists(t + '/cheats-loaded'))
          and 'cheats_on=1\n' in cmd('emud.sock', 'STATUS'))
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    open(state + '/' + user + '/secrets/retroachievements.conf', 'w').write(
        'USERNAME=alice\nTOKEN=tok123\nENABLED=1\nHARDCORE=1\n')
    os.remove(t + '/cheats-loaded')
    launch('new')
    check('no cheats in hardcore', 'available=0\nreason=hardcore\n' in cmd('emud.sock', 'CHEATS')
          and cmd('emud.sock', 'CHEAT_SET\t1\t1') == 'ERR hardcore\n'
          and wait(lambda: os.path.exists(t + '/cheats-loaded'))
          and open(t + '/cheats-loaded').read() == 'CHEATS_LOAD\n')
    check('no netplay in hardcore', cmd('emud.sock', 'NETPLAY_HOST') == 'ERR hardcore\n')
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    open(state + '/' + user + '/secrets/retroachievements.conf', 'w').write(
        'USERNAME=alice\nTOKEN=tok123\nENABLED=1\nHARDCORE=0\n')

    # Netplay: Host Game closes the game and starts it again as a host.
    os.makedirs(state + '/' + user, exist_ok=True)
    open(state + '/' + user + '/profile.conf', 'w').write('USER_NAME=Alice\n')
    launch('new')
    sess = session()
    check('netplay session keys', 'netplay_nickname = "Alice"' in sess and 'netplay_public_announce = "false"' in sess
          and 'netplay_ip_port = "55435"' in sess)
    check('netplay off', 'netplay=off\n' in cmd('emud.sock', 'STATUS'))
    os.remove(t + '/args')
    check('host game', cmd('emud.sock', 'NETPLAY_HOST') == 'OK\n')
    check('relaunched as host', wait(lambda: os.path.exists(t + '/args') and '--host' in args()))
    check('hosting with a player', wait(lambda: 'netplay=host\nnetplay_peers=1\nnetplay_peer=bob\n' in cmd('emud.sock', 'STATUS')))
    check('resumed from the saved progress', '--entryslot' in args())
    check('no cheats with netplay', 'reason=netplay' in cmd('emud.sock', 'CHEATS'))
    check('host twice refused', cmd('emud.sock', 'NETPLAY_HOST') == 'ERR busy\n')
    check('stop hosting', cmd('emud.sock', 'NETPLAY_STOP') == 'OK\n')
    check('netplay off again', wait(lambda: 'netplay=off\n' in cmd('emud.sock', 'STATUS')))
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))
    check('reject bad join', cmd('emud.sock', 'LAUNCH\t%s\tnew\tconnect:1.2.3:55435:snes9x' % game) == 'ERR net\n')
    check('reject join core', cmd('emud.sock', 'LAUNCH\t%s\tnew\tconnect:192.168.1.9:55435:gpsp' % game) == 'ERR no-core\n')
    os.remove(t + '/args')
    check('join', cmd('emud.sock', 'LAUNCH\t%s\tresume\tconnect:192.168.1.9:55436:mesen-s' % game) == 'OK\n')
    check('join args', wait(lambda: os.path.exists(t + '/args')) and '--connect 192.168.1.9 --port 55436' in args()
          and '--entryslot' not in args() and 'mesen-s_libretro.so' in open(t + '/launched-core').read())
    cmd('emud.sock', 'QUIT'); wait(lambda: 'state=idle' in cmd('emud.sock', 'STATUS'))

    # LAN discovery: a fake RetroArch host answering the broadcast.
    import struct
    host = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    host.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        host.bind(('', 55435))
    except OSError:
        host = None
    if host:
        asked = []
        def answer():
            host.settimeout(3)
            try:
                q, addr = host.recvfrom(64)
            except OSError:
                return
            asked.append(addr)
            if q == struct.pack('!I', 0x52414E51):
                pkt = struct.pack('!IiiI', 0x52414E53, 0, 55435, 0) + b'bob'.ljust(32, b'\0') \
                    + b'Linux'.ljust(32, b'\0') + b'Mesen-S'.ljust(32, b'\0') + b'1'.ljust(32, b'\0') \
                    + b'1.22.2'.ljust(32, b'\0') + b'Mario.sfc'.ljust(256, b'\0') + b'N/A'.ljust(256, b'\0')
                host.sendto(pkt, addr)
        threading.Thread(target=answer, daemon=True).start()
        hosts = cmd('emud.sock', 'NETPLAY_HOSTS')
        if hosts.startswith('ERR network') or not asked:
            print('[SKIP] broadcasts do not come back on this machine: discovery not tested')
        else:
            check('host discovered', '\t55435\tbob\tMesen-S\tMario.sfc\t%s\tsnes\tmesen-s\t0\n' % game in hosts)
        host.close()
finally:
    for p in procs: p.terminate(); p.wait()
    ns.close()
check('core option update runs once', 'opt_a = "disabled"\n' in open(radir + '/retroarch-core-options.cfg').read())
if fails:
    print(open(t + '/emud.log').read())
print('[PASS] emud game settings' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
