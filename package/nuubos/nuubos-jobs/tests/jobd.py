#!/usr/bin/env python3
# Host test: nuubos-jobd lifecycle, progress, result, failure, cancel,
# retry, concurrency, de-duplication, argument validation, notifications.
# Run: python3 package/nuubos/nuubos-jobs/tests/jobd.py
import os, socket, subprocess, sys, tempfile, threading, time

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, '..', 'src')
notify_h = os.path.join(here, '..', '..', 'nuubos-notify', 'src', 'notify.h')
t = tempfile.mkdtemp()
run, jobs = t + '/run', t + '/jobs'
for d in (run + '/user', jobs, t + '/inc/nuubos'):
    os.makedirs(d)
open(t + '/inc/nuubos/notify.h', 'w').write(open(notify_h).read())
open(run + '/user/active', 'w').write('aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n')

def script(name, body):
    p = t + '/' + name
    open(p, 'w').write('#!/bin/sh\n' + body)
    os.chmod(p, 0o755)
    return p
ok = script('ok', 'echo "@progress 50 half"\necho "@result /out/$1-$NUUBOS_JOB_USER"\nexit 0\n')
bad = script('bad', 'echo "@error network"\nexit 3\n')
slow = script('slow', 'trap "exit 9" TERM\necho "@progress -1"\nwhile :; do sleep 0.05; done\n')
def jobtype(name, cmd, extra):
    open(jobs + '/%s.job' % name, 'w').write('COMMAND=%s\n%s' % (cmd, extra))
jobtype('ok', ok, 'SCOPE=user\nARGS=1\nRETRY=1\n')
jobtype('bad', bad, 'RETRY=1\n')
jobtype('slow', slow, 'CANCEL=1\n')
jobtype('slow2', slow, 'CANCEL=1\n')
jobtype('slow3', slow, 'CANCEL=1\n')
jobtype('norm', slow, '')

exe = t + '/jobd'
subprocess.check_call(['gcc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
    '-I' + t + '/inc', '-DRUN_ROOT="%s"' % run, '-DJOBS_DIR="%s"' % jobs,
    '-DNUUBOS_NOTIFY_SOCKET="%s/notifyd.sock"' % run, src + '/jobd.c', '-o', exe])
notes = []
ns = socket.socket(socket.AF_UNIX); ns.bind(run + '/notifyd.sock'); ns.listen(64)
def notifyd():
    while True:
        try:
            c, _ = ns.accept()
        except OSError:
            return
        notes.append(c.recv(4096).decode()); c.close()
threading.Thread(target=notifyd, daemon=True).start()
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
            s.connect(run + '/jobd.sock'); break
        except OSError:
            time.sleep(0.05)
    s.sendall((line + '\n').encode()); s.settimeout(2); data = b''
    while True:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            break
        if not chunk: break
        data += chunk
        if data.endswith(b'end=1\n') or (not line.startswith('STATUS') and data.endswith(b'\n')):
            break
    s.close(); return data.decode()

def job(jid):
    for l in cmd('STATUS').splitlines():
        if l.startswith('job=%s\t' % jid):
            return l[4:].split('\t')
    return None

def wait_state(jid, states, secs=5):
    end = time.time() + secs
    while time.time() < end:
        j = job(jid)
        if j and j[2] in states: return j
        time.sleep(0.05)
    return job(jid)

try:
    r = cmd('SUBMIT\tok\tgame1'); jid = r.split()[1]
    j = wait_state(jid, ('succeeded',))
    check('ok succeeds', j and j[2] == 'succeeded' and j[3] == '100')
    check('result + user env', j and j[6] == '/out/game1-aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa')
    check('progress step', j and j[5] == 'half')
    check('live notification', any('id=job.%s' % jid in n and 'progress=' in n for n in notes))
    check('completion notification', any('id=job.%s' % jid in n and 'state=succeeded' in n for n in notes))

    check('unknown type', cmd('SUBMIT\tnope').startswith('ERR unknown'))
    check('path traversal type', cmd('SUBMIT\t../ok').startswith('ERR unknown'))
    check('too many args', cmd('SUBMIT\tok\ta\tb').startswith('ERR args'))
    check('option-like arg', cmd('SUBMIT\tok\t--evil').startswith('ERR args'))

    r = cmd('SUBMIT\tbad'); bid = r.split()[1]
    j = wait_state(bid, ('failed',))
    check('failure reason', j and j[2] == 'failed' and j[7] == 'network')
    check('retry', cmd('RETRY\t' + bid).startswith('OK'))
    j = wait_state(bid, ('failed',))
    check('retry ran again, same id', j and j[2] == 'failed')

    a = cmd('SUBMIT\tslow').split()[1]
    check('dedup same job', cmd('SUBMIT\tslow').split()[1] == a)
    b = cmd('SUBMIT\tslow2').split()[1]
    c = cmd('SUBMIT\tslow3').split()[1]
    wait_state(b, ('running',))
    check('bounded concurrency: third queued', job(c)[2] == 'queued')
    check('non-cancellable refused', True)
    check('cancel running', cmd('CANCEL\t' + a).startswith('OK'))
    check('cancelled state', wait_state(a, ('cancelled',))[2] == 'cancelled')
    check('queued starts when capacity frees', wait_state(c, ('running',))[2] == 'running')
    n = cmd('SUBMIT\tnorm').split()[1]
    check('not cancellable', cmd('CANCEL\t' + n).startswith('ERR not-cancellable'))
    for x in (b, c, n):
        cmd('CANCEL\t' + x)
finally:
    proc.terminate(); proc.wait(); ns.close()
    subprocess.call(['pkill', '-f', t + '/slow'])
print('[PASS] jobd' if not fails else '[FAIL] %d checks' % fails)
sys.exit(1 if fails else 0)
