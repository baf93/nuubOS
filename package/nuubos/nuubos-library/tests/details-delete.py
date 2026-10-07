#!/usr/bin/env python3
# Host test: libraryd DETAILS / HIDE / STATS_RESET / DELETE.
# Run: python3 package/nuubos/nuubos-library/tests/details-delete.py
import os, socket, subprocess, sys, tempfile, time

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', 'src')
t = tempfile.mkdtemp()
run, ud, share = t + '/run', t + '/userdata', t + '/share'
user = 'aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa'
for d in (run + '/user', ud + '/roms/psx/ff7', ud + '/roms/snes', share,
          ud + '/library/metadata/snes', ud + '/users/' + user + '/saves/snes'):
    os.makedirs(d, exist_ok=True)
open(share + '/systems.conf', 'w').write(open(src + '/systems.conf').read())
open(run + '/user/active', 'w').write(user + '\n')
def w(p, data): open(ud + '/roms/' + p, 'w').write(data)
w('snes/Mario (USA).sfc', 'x' * 100)
w('snes/Zelda.sfc', 'y' * 200)
w('psx/ff7/FF7 (Disc 1).bin', 'a' * 50)
w('psx/ff7/FF7 (Disc 2).bin', 'b' * 60)
w('psx/ff7/FF7 (Disc 1).cue', 'FILE "FF7 (Disc 1).bin" BINARY\n')
w('psx/ff7/FF7 (Disc 2).cue', 'FILE "FF7 (Disc 2).bin" BINARY\n')
w('psx/ff7/FF7.m3u', 'FF7 (Disc 1).cue\nFF7 (Disc 2).cue\n')
open(ud + '/library/metadata/snes/Mario (USA).txt', 'w').write(
    'title=Super Mario World\ndeveloper=Nintendo\nbad key=x\nplayers=1-2\n')
open(ud + '/users/' + user + '/saves/snes/Mario (USA).srm', 'w').write('save')

exe = t + '/libraryd'
subprocess.check_call(['gcc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
    '-DRUN_ROOT="%s"' % run, '-DUSERDATA_ROOT="%s"' % ud, '-DSHARE_ROOT="%s"' % share,
    src + '/libraryd.c', '-o', exe, '-lpthread'])
proc = subprocess.Popen([exe], stderr=open(t + '/log', 'w'))
fails = 0
def check(name, cond):
    global fails
    if not cond:
        print('[FAIL]', name); fails += 1

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
        txt = data.decode()
        if txt.endswith('end=1\n') or (not txt.startswith(('game=', 'user=')) and txt.endswith('\n')):
            break
    s.close()
    return data.decode()

try:
    for _ in range(50):
        st = cmd('STATUS')
        if 'scanning=0' in st and 'games=' in st and 'games=0' not in st: break
        time.sleep(0.1)
    games = cmd('GAMES\tsystem:snes')
    ids = {l.split('\t')[2]: l.split('=')[1].split('\t')[0] for l in games.splitlines() if l.startswith('game=')}
    mario, zelda = ids.get('Mario'), ids.get('Zelda')
    check('two snes games', mario and zelda)
    psx = cmd('GAMES\tsystem:psx')
    ff7 = [l.split('=')[1].split('\t')[0] for l in psx.splitlines() if l.startswith('game=')]
    check('m3u is the only psx game', len(ff7) == 1)

    cmd('SESSION_RECORD\t%s\t120' % mario)
    d = cmd('DETAILS\t' + mario)
    check('details sessions', 'sessions=1' in d)
    check('details metadata', 'meta_developer=Nintendo' in d and 'meta_players=1-2' in d)
    check('details rejects bad keys', 'bad key' not in d)
    check('details path', 'path=snes/Mario (USA).sfc' in d)

    check('hide ok', cmd('HIDE\t%s\t1' % zelda).startswith('OK'))
    check('hidden leaves system scope', zelda not in cmd('GAMES\tsystem:snes'))
    check('hidden scope lists it', zelda in cmd('GAMES\thidden'))
    check('details hidden flag', 'hidden=1' in cmd('DETAILS\t' + zelda))
    cmd('HIDE\t%s\t0' % zelda)
    check('unhide', zelda in cmd('GAMES\tsystem:snes'))

    check('stats reset', cmd('STATS_RESET\t' + mario).startswith('OK'))
    check('stats gone', 'sessions=0' in cmd('DETAILS\t' + mario))

    check('delete needs confirm', cmd('DELETE\t' + mario).startswith('ERR'))
    check('delete ok', cmd('DELETE\t%s\tCONFIRM' % mario).startswith('OK'))
    check('rom removed', not os.path.exists(ud + '/roms/snes/Mario (USA).sfc'))
    check('metadata removed', not os.path.exists(ud + '/library/metadata/snes/Mario (USA).txt'))
    check('save kept', os.path.exists(ud + '/users/' + user + '/saves/snes/Mario (USA).srm'))
    check('other rom kept', os.path.exists(ud + '/roms/snes/Zelda.sfc'))
    check('gone from catalog', 'ERR' in cmd('DETAILS\t' + mario))

    check('delete multi-disc', cmd('DELETE\t%s\tCONFIRM' % ff7[0]).startswith('OK'))
    left = os.listdir(ud + '/roms/psx/ff7')
    check('m3u deleted', 'FF7.m3u' not in left)
    check('cues deleted', not any(f.endswith('.cue') for f in left))
    check('tracks deleted', left == [])
finally:
    proc.terminate(); proc.wait()
print('[PASS] libraryd details/delete' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
