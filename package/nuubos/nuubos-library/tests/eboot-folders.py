#!/usr/bin/env python3
# Host test: a game in a folder of its own ("<Folder>/EBOOT.PBP", GitHub
# #30) is named after the folder: title, identity, covers, metadata and the
# DETAILS name= the scraper uses; an EBOOT.PBP right in a system's folder
# keeps its file name. Delete Game removes such a game's whole folder.
# Run: python3 package/nuubos/nuubos-library/tests/eboot-folders.py
import os, socket, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', 'src')
t = tempfile.mkdtemp()
run, ud, share = t + '/run', t + '/userdata', t + '/share'
user = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
for d in (run + '/user', ud + '/roms/psp/Jet Set Radio', ud + '/roms/psp/PSP/GAME/SonicR', share,
          ud + '/library/covers/psp', ud + '/users/' + user + '/library'):
    os.makedirs(d, exist_ok=True)
open(share + '/systems.conf', 'w').write(open(src + '/systems.conf').read())
open(run + '/user/active', 'w').write(user + '\n')
# The same size on purpose: the identity must not be name + size alone.
for f in ('psp/Jet Set Radio/EBOOT.PBP', 'psp/PSP/GAME/SonicR/eboot.pbp', 'psp/EBOOT.PBP'):
    open(ud + '/roms/' + f, 'w').write('x' * 64)
# The port's own data beside it, and a loose game next to the system folder EBOOT.
os.makedirs(ud + '/roms/psp/Jet Set Radio/data/cache')
open(ud + '/roms/psp/Jet Set Radio/data/cache/x.bin', 'w').write('d')
open(ud + '/roms/psp/Other.iso', 'w').write('i' * 32)
open(ud + '/library/covers/psp/Jet Set Radio.png', 'w').write('png')

exe = t + '/libraryd'
subprocess.check_call(['gcc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
    '-DRUN_ROOT="%s"' % run, '-DUSERDATA_ROOT="%s"' % ud, '-DSHARE_ROOT="%s"' % share, '-DSTATE_ROOT="%s"' % (t + '/state'),
    src + '/libraryd.c', '-o', exe, '-lpthread'])

def cmd(line):
    s = socket.socket(socket.AF_UNIX)
    for _ in range(50):
        try:
            s.connect(run + '/libraryd.sock'); break
        except OSError:
            time.sleep(0.1)
    s.sendall((line + '\n').encode())
    data = b''
    s.settimeout(2)
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk: break
        data += chunk
        if data.decode().endswith('end=1\n'):
            break
    s.close()
    return data.decode()

fails = 0
def check(name, cond):
    global fails
    if not cond:
        print('[FAIL]', name); fails += 1

open(run + '/ui-ready', 'w').write('')
proc = subprocess.Popen([exe], stderr=open(t + '/log', 'w'))
try:
    for _ in range(100):
        if 'scan: ' in open(t + '/log').read() and 'scanning=0' in cmd('STATUS'):
            break
        time.sleep(0.1)
    games = {}
    for l in cmd('GAMES\tsystem:psp').splitlines():
        if l.startswith('game='):
            f = l[5:].split('\t')
            games[f[2]] = f
    check('games titled by their folders: %s' % sorted(games), sorted(games) == ['EBOOT', 'Jet Set Radio', 'Other', 'SonicR'])
    check('distinct identities', len({g[0] for g in games.values()}) == 4)
    jsr = games.get('Jet Set Radio')
    check('cover named after the folder', jsr and jsr[3].endswith('/covers/psp/Jet Set Radio.png'))
    names = {}
    for title, g in games.items():
        d = cmd('DETAILS\t' + g[0])
        names[title] = next((l[5:] for l in d.splitlines() if l.startswith('name=')), None)
        if title in ('Jet Set Radio', 'EBOOT'):
            check('folder=1 only for a folder game: ' + title, ('folder=1' in d.splitlines()) == (title == 'Jet Set Radio'))
    sysl = cmd('SYSTEMS')
    check('SYSTEMS lists systems without games', 'system=dreamcast\tDreamcast' in sysl and sysl.endswith('end=1\n'))
    check('DETAILS name=: %s' % names, names == {'Jet Set Radio': 'Jet Set Radio', 'SonicR': 'SonicR', 'EBOOT': 'EBOOT', 'Other': 'Other'})
    check('delete a folder game', cmd('DELETE\t%s\tCONFIRM' % games['Jet Set Radio'][0]).startswith('OK'))
    check('its whole folder is gone', not os.path.exists(ud + '/roms/psp/Jet Set Radio'))
    check('delete the EBOOT in the system folder', cmd('DELETE\t%s\tCONFIRM' % games['EBOOT'][0]).startswith('OK'))
    check('only its file goes', not os.path.exists(ud + '/roms/psp/EBOOT.PBP') and os.path.exists(ud + '/roms/psp/Other.iso')
          and os.path.exists(ud + '/roms/psp/PSP/GAME/SonicR/eboot.pbp'))
finally:
    proc.terminate()
    proc.wait()
if fails:
    print('[FAIL] %d checks' % fails)
    print(open(t + '/log').read())
    sys.exit(1)
print('[PASS] library EBOOT folders')
