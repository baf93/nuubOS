#!/usr/bin/env python3
"""Generates the built-in nuubOS Moods (theme.conf FORMAT=1) from one accent
colour each: neutrals are the nuubOS dark/matte greys tinted toward the
accent hue, like the hand-made Ember/Mint. Output is committed under
package/nuubos/nuubos-themes/src/themes/<id>/theme.conf (MIT, nuubOS).

Usage: tools/moods/make-moods.py   (rewrites only the generated Moods)
"""
import colorsys, os

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
OUT = os.path.join(ROOT, 'package/nuubos/nuubos-themes/src/themes')

# id, name, description, accent
MOODS = [
    ('sakura',   'Sakura',   'Soft cherry-blossom pink',   '#ff6fae'),
    ('amethyst', 'Amethyst', 'Deep violet accents',        '#a77bff'),
    ('lagoon',   'Lagoon',   'Clear tropical teal',        '#1fc2d6'),
    ('crimson',  'Crimson',  'Bold arcade red',            '#ff4d5e'),
    ('matcha',   'Matcha',   'Fresh green tea',            '#95cf4a'),
    ('gold',     'Gold',     'Warm golden highlights',     '#f2b33d'),
    ('indigo',   'Indigo',   'Night-sky indigo',           '#6f7dff'),
    ('coral',    'Coral',    'Sunset coral',               '#ff8a6b'),
]

# Base neutrals (built-in nuubOS look): value of each grey, 0..1.
NEUTRALS = {
    'text': 0.965, 'text-secondary': 0.68, 'text-dim': 0.58, 'text-faint': 0.52,
    'text-disabled': 0.36, 'background': 0.058, 'surface': 0.165,
    'border': 0.215, 'border-soft': 0.195,
}

def hex2rgb(h):
    return tuple(int(h[i:i + 2], 16) / 255 for i in (1, 3, 5))

def rgb2hex(c):
    return '#' + ''.join('%02x' % max(0, min(255, round(v * 255))) for v in c)

def mix(a, b, t):
    return tuple(x * (1 - t) + y * t for x, y in zip(a, b))

def tinted(hue, value, sat):
    return colorsys.hsv_to_rgb(hue, sat, value)

def mood(accent_hex):
    a = hex2rgb(accent_hex)
    h, s, v = colorsys.rgb_to_hsv(*a)
    white, black = (1, 1, 1), (0, 0, 0)
    t = {
        'accent': accent_hex,
        'accent-light': rgb2hex(mix(a, white, 0.30)),
        'accent-muted': rgb2hex(mix(a, black, 0.18)),
        'accent-text': rgb2hex(mix(a, white, 0.80)),
        'focus-fill': rgb2hex(tinted(h, 0.20, 0.55)),
    }
    for k, val in NEUTRALS.items():
        # Light text barely tinted, dark surfaces a little more.
        sat = 0.03 if val > 0.5 else 0.10 if val > 0.3 else 0.16
        t[k] = rgb2hex(tinted(h, val, sat))
    t['warning'] = '#f2b84b'
    return t

ORDER = ['accent', 'accent-light', 'accent-muted', 'accent-text', 'focus-fill', 'text',
         'text-secondary', 'text-dim', 'text-faint', 'text-disabled', 'background',
         'surface', 'border', 'border-soft', 'warning']

for mid, name, desc, accent in MOODS:
    d = os.path.join(OUT, mid)
    os.makedirs(d, exist_ok=True)
    tokens = mood(accent)
    with open(os.path.join(d, 'theme.conf'), 'w') as f:
        f.write('FORMAT=1\nID=%s\nNAME=%s\nAUTHOR=nuubOS\nVERSION=1.0\nMIN_NUUBUI=0.5\n'
                'DESCRIPTION=%s\nLICENSE=MIT\n' % (mid, name, desc))
        for k in ORDER:
            f.write('%s=%s\n' % (k, tokens[k]))
print(' '.join(m[0] for m in MOODS))
