#!/usr/bin/env python3
"""nuubOS System Sound Set v5 + Test Sound (original synthesis, MIT).

One warm, low family for the whole console (user request 2026-10-10: v4
was too high). Felt-mallet tones (a sine with soft, quickly damped octave
and twelfth partials, 4-6 ms attack, never a click), fundamentals between
D3 and D5, everything through a gentle low-pass, a small shared room.
D major; gestures follow the UI motion: forward/open rise, back and power
off fall. Boot, restart, power off, launch and the test sound share the
"nuubOS motif" D - F# - A - D.

Cues (audiod names): navigation, select, back, quick-settings, launch,
error, screenshot, boot, restart, poweroff; plus the test
sound. Pure Python (no numpy). Writes 48 kHz mono signed 16-bit WAVs into
package/nuubos/nuubos-audio/src/assets/. Re-run after changing a cue:
    tools/sounds/make-system-sounds.py
"""
import math
import os
import random
import struct
import wave

RATE = 48000
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
ASSETS = os.path.join(ROOT, 'package/nuubos/nuubos-audio/src/assets')


def note(name):
    names = {'C': -9, 'C#': -8, 'D': -7, 'E': -5, 'F': -4, 'F#': -3, 'G': -2, 'A': 0, 'B': 2}
    octave = int(name[-1])
    return 440.0 * 2 ** ((names[name[:-1]] + (octave - 4) * 12) / 12)


def silence(seconds):
    return [0.0] * int(seconds * RATE)


def mallet(freq, dur, amp=1.0, attack=0.006, decay=None, bright=1.0):
    """Felt mallet: fundamental, soft octave and twelfth that die out
    faster than the fundamental; raised-cosine attack, 4 ms release."""
    n = int(dur * RATE)
    decay = decay or dur * 0.35
    out = [0.0] * n
    partials = ((1.0, 1.0, 1.0), (2.0, 0.22 * bright, 0.45), (3.0, 0.06 * bright, 0.25))
    na = max(1, int(attack * RATE))
    for i in range(n):
        t = i / RATE
        env_a = 0.5 - 0.5 * math.cos(math.pi * min(1.0, i / na))
        s = 0.0
        for ratio, gain, dscale in partials:
            s += gain * math.exp(-t / (decay * dscale)) * math.sin(2 * math.pi * freq * ratio * t)
        out[i] = amp * env_a * s
    nf = min(n, int(0.004 * RATE))
    for i in range(nf):
        out[n - 1 - i] *= i / nf
    return out


def pad(freqs, dur, amp=0.25, attack=0.25, release=0.5):
    """Slow-attack chord with gentle detune (warm air under the motif)."""
    n = int(dur * RATE)
    out = [0.0] * n
    na, nr = max(1, int(attack * RATE)), max(1, int(release * RATE))
    for f in freqs:
        for det in (-0.5, 0.5):
            ph = random.random() * 2 * math.pi
            w = 2 * math.pi * (f + det) / RATE
            for i in range(n):
                env = min(1.0, i / na)
                if i > n - nr:
                    env *= (n - i) / nr
                out[i] += amp / (2 * len(freqs)) * env * math.sin(w * i + ph)
    return out


def noise(dur, amp=0.05, cutoff=0.06, shape='swell'):
    """Low-passed noise: 'swell' (air) or 'click' (a soft shutter tick)."""
    n = int(dur * RATE)
    out = [0.0] * n
    y = 0.0
    for i in range(n):
        p = i / n
        env = math.sin(math.pi * p) ** 2 if shape == 'swell' else math.exp(-p * 9)
        y += cutoff * (random.uniform(-1, 1) - y)
        out[i] = amp * env * y * 8
    return out


def mix_into(dst, src, at, gain=1.0):
    start = int(at * RATE)
    if start + len(src) > len(dst):
        dst.extend([0.0] * (start + len(src) - len(dst)))
    for i, v in enumerate(src):
        dst[start + i] += v * gain
    return dst


def lowpass(sig, cutoff_hz=2600):
    """One-pole low-pass, run twice (12 dB/oct): takes the edge off."""
    a = 1 - math.exp(-2 * math.pi * cutoff_hz / RATE)
    for _ in range(2):
        y = 0.0
        for i, v in enumerate(sig):
            y += a * (v - y)
            sig[i] = y
    return sig


def reverb(sig, wet=0.16, room=0.74, tail=0.5):
    """Small Schroeder room: 4 damped combs + 2 all-passes."""
    out = sig + [0.0] * int(tail * RATE)
    wet_sig = [0.0] * len(out)
    for d in (0.0297, 0.0371, 0.0411, 0.0437):
        delay = int(RATE * d)
        buf = [0.0] * delay
        idx = 0
        lp = 0.0
        for i in range(len(out)):
            y = buf[idx]
            lp = y * 0.55 + lp * 0.45
            buf[idx] = out[i] + lp * room
            idx = (idx + 1) % delay
            wet_sig[i] += y * 0.25
    for d in (0.005, 0.0017):
        delay = int(RATE * d)
        buf = [0.0] * delay
        idx = 0
        for i in range(len(wet_sig)):
            b = buf[idx]
            y = -0.5 * wet_sig[i] + b
            buf[idx] = wet_sig[i] + 0.5 * y
            idx = (idx + 1) % delay
            wet_sig[i] = y
    res = [out[i] * (1 - wet) + wet_sig[i] * wet for i in range(len(out))]
    thr = 10 ** (-70 / 20)
    end = len(res)
    while end > 1 and abs(res[end - 1]) < thr:
        end -= 1
    res = res[:end]
    nf = min(len(res), int(0.02 * RATE))
    for i in range(nf):
        res[len(res) - 1 - i] *= i / nf
    return res


def normalize(sig, peak_db):
    pk = max(abs(v) for v in sig) or 1.0
    g = 10 ** (peak_db / 20) / pk
    return [v * g for v in sig]


def finish(sig, peak_db, cutoff=2600, wet=0.0, tail=0.0):
    sig = lowpass(sig, cutoff)
    if wet > 0:
        sig = reverb(sig, wet=wet, tail=tail)
    return normalize(sig, peak_db)


def write(name, sig):
    path = os.path.join(ASSETS, name)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b''.join(struct.pack('<h', max(-32767, min(32767, int(round(v * 32767))))) for v in sig))
    print('%-28s %.3f s' % (name, len(sig) / RATE))


def motif(base_octave, step, amp=0.85, decay=0.24, length=0.8):
    """D - F# - A - D from D<base_octave>, one note every `step` s."""
    out = []
    names = ('D%d' % base_octave, 'F#%d' % base_octave, 'A%d' % base_octave, 'D%d' % (base_octave + 1))
    for k, n in enumerate(names):
        mix_into(out, mallet(note(n), length, amp=amp, decay=decay, bright=0.8), step * k)
    return out


def main():
    random.seed(5)  # reproducible pad phases and noise
    D3, Fs3, A3 = note('D3'), note('F#3'), note('A3')
    D4, E4, Fs4, A4, B4 = note('D4'), note('E4'), note('F#4'), note('A4'), note('B4')
    D5 = note('D5')
    Cs4 = note('C#4')
    D2, A2, B2, G2 = note('D2'), note('A2'), note('B2'), note('G2')

    # Navigation: a short soft wooden "tok" on A4 with a little body below.
    nav = mallet(A4, 0.06, amp=0.8, attack=0.004, decay=0.016, bright=0.5)
    mix_into(nav, mallet(D4, 0.06, amp=0.25, attack=0.004, decay=0.014, bright=0.2), 0)
    write('system/navigation.wav', finish(nav, -10, cutoff=2200))

    # Select: forward = a rising fifth, D4 -> A4.
    sel = mix_into(mallet(D4, 0.11, decay=0.04), mallet(A4, 0.16, decay=0.06), 0.05)
    write('system/select.wav', finish(sel, -5, wet=0.10, tail=0.12))

    # Back: the same gesture falling, A4 -> D4, softer.
    back = mix_into(mallet(A4, 0.11, amp=0.85, decay=0.04, bright=0.7),
                    mallet(D4, 0.16, amp=0.9, decay=0.06, bright=0.6), 0.05)
    write('system/back.wav', finish(back, -6.5, wet=0.10, tail=0.12))

    # Quick Menu / Settings open: a breath of air and a quick rising
    # D major shimmer, F#4 - A4 - D5.
    qs = noise(0.18, amp=0.05, cutoff=0.04)
    for k, f in enumerate((Fs4, A4, D5)):
        mix_into(qs, mallet(f, 0.16, amp=0.7, decay=0.06, bright=0.7), 0.04 * k)
    write('system/quick-settings.wav', finish(qs, -5.5, wet=0.14, tail=0.18))

    # Launch: a game starts. The motif rushes up (D4 F#4 A4 D5) over a
    # swell: "here we go".
    ln = noise(0.5, amp=0.06, cutoff=0.03)
    mix_into(ln, pad((D4, A4), 0.7, amp=0.22, attack=0.25, release=0.35), 0.0)
    for k, f in enumerate((D4, Fs4, A4, D5)):
        mix_into(ln, mallet(f, 0.45, amp=0.75, decay=0.13, bright=0.8), 0.07 * k)
    write('system/launch.wav', finish(ln, -5, wet=0.18, tail=0.35))

    # Error / warning: two low muted notes falling a semitone (D4 -> C#4),
    # a soft "uh-oh", never alarming.
    er = mix_into(mallet(D4, 0.16, amp=0.9, decay=0.05, bright=0.4),
                  mallet(Cs4, 0.24, amp=0.9, decay=0.08, bright=0.35), 0.1)
    write('system/error.wav', finish(er, -6, cutoff=1800, wet=0.08, tail=0.12))

    # Screenshot: a soft shutter (two muted clicks) with a faint tone.
    sh = noise(0.05, amp=0.12, cutoff=0.18, shape='click')
    mix_into(sh, noise(0.06, amp=0.10, cutoff=0.12, shape='click'), 0.07)
    mix_into(sh, mallet(D5, 0.15, amp=0.25, decay=0.05, bright=0.3), 0.07)
    write('system/screenshot.wav', finish(sh, -8, cutoff=3200, wet=0.06, tail=0.08))

    # Boot: the nuubOS motif in the low octave over a warm D major pad,
    # closing on a soft high D.
    boot = pad((D3, Fs3, A3, D4), 1.7, amp=0.4, attack=0.4, release=0.8)
    mix_into(boot, motif(3, 0.14, amp=0.9, decay=0.3, length=0.9), 0.12)
    mix_into(boot, mallet(D5, 0.8, amp=0.22, decay=0.3, bright=0.5), 0.12 + 0.14 * 3 + 0.04)
    write('system/boot.wav', finish(boot, -3.5, cutoff=2400, wet=0.22, tail=0.6))

    # Restart: the motif turns back on itself (D4 - A3 - D4).
    rs = pad((D3, A3), 1.0, amp=0.3, attack=0.15, release=0.5)
    for k, f in enumerate((D4, A3, D4)):
        mix_into(rs, mallet(f, 0.5, amp=0.85, decay=0.17, bright=0.7), 0.05 + 0.15 * k)
    write('system/restart.wav', finish(rs, -4.5, wet=0.2, tail=0.45))

    # Power off: the motif falling to rest (D4 - A3 - F#3 - D3), the pad
    # fading out.
    po = pad((D3, Fs3, A3), 1.2, amp=0.35, attack=0.05, release=1.0)
    for k, f in enumerate((D4, A3, Fs3, D3)):
        mix_into(po, mallet(f, 0.65, amp=0.85 - 0.06 * k, decay=0.22, bright=0.7 - 0.1 * k), 0.04 + 0.16 * k)
    write('system/poweroff.wav', finish(po, -4, cutoff=2200, wet=0.22, tail=0.6))

    # Test Sound: a 4-bar tune on the motif, lead around D4-D5, chords and
    # a bass doubled an octave up so small speakers still carry it; ~10 s,
    # ending on the boot cadence.
    beat = 60 / 108
    song = silence(0.1)
    chords = [(D2, (D3, Fs3, A3)), (B2, (D3, Fs3, B2 * 2)), (G2, (D3, G2 * 2, B2 * 2)), (A2, (E4 / 2, A3, Cs4))]
    lead = [
        (D4, 0, 1), (Fs4, 1, 1), (A4, 2, 1.5), (D5, 3.5, 0.5),
        (B4, 4, 1), (A4, 5, 1), (Fs4, 6, 2),
        (D4, 8, 1), (E4, 9, 1), (Fs4, 10, 1), (A4, 11, 1),
        (B4, 12, 1.5), (A4, 13.5, 0.5), (E4, 14, 2),
    ]
    start = 0.1
    for bar in range(4):
        bass, chord = chords[bar]
        t = start + bar * 4 * beat
        mix_into(song, pad(chord, 4 * beat + 0.05, amp=0.32, attack=0.06, release=0.25), t)
        for b in (0, 2):
            mix_into(song, mallet(bass, 1.6 * beat, amp=0.7, decay=0.4, bright=0.6), t + b * beat)
            mix_into(song, mallet(bass * 2, 1.2 * beat, amp=0.35, decay=0.25, bright=0.3), t + b * beat)
        for b in (1, 3):
            mix_into(song, mallet(bass * 2, 0.6 * beat, amp=0.3, decay=0.08, bright=0.3), t + b * beat)
    for f, b, length in lead:
        mix_into(song, mallet(f, max(0.3, length * beat * 1.4), amp=0.85, decay=0.2 + 0.06 * length), start + b * beat)
    end = start + 16 * beat
    mix_into(song, pad((D3, Fs3, A3, D4), 1.6, amp=0.4, attack=0.05, release=1.0), end)
    mix_into(song, motif(4, 0.12, amp=0.85, decay=0.26, length=0.8), end)
    mix_into(song, mallet(D2, 1.4, amp=0.6, decay=0.5, bright=0.6), end)
    write('nuubos-test-jingle.wav', finish(song, -3, cutoff=3000, wet=0.18, tail=0.8))


if __name__ == '__main__':
    main()
