# nuubOS themes (format 1)

A nuubOS theme changes how nuubUI looks without changing what it does. It is
a folder (or a `.tar` holding one folder) with a single text file,
`theme.conf`, and an optional preview image. Themes contain no code.

## Installing

- Copy the folder or `.tar` to the SD card or a USB drive, open **Files**,
  select it and choose **Install Theme**. The package is checked first; an
  invalid package never replaces a working theme.
- Each user picks a theme in **Settings → General → Theme**. Moving through
  the list previews each theme on the real interface; **Back** keeps the
  current one.
- Installed themes are shared by every user (`/userdata/themes/<id>`). The
  built-in **nuubOS** theme cannot be removed and is used whenever a theme is
  missing, invalid or incompatible, and always in safe mode.

## theme.conf

```
FORMAT=1
ID=neon                 # lowercase letters, digits, "-"; at most 32; also the folder name
NAME=Neon               # shown in the theme list
AUTHOR=Someone
VERSION=1.0
MIN_NUUBUI=0.5          # oldest nuubUI version the theme supports
DESCRIPTION=Pink accents
LICENSE=CC-BY-4.0       # license of the theme and of any image it ships
PREVIEW=preview.png     # optional, a file in the same folder

accent=#ff2fa0
text=#f5f6f8
```

Only the keys above and the color tokens below are accepted; colors are
`#RRGGBB`. A token that is not set keeps the built-in value.

| Token | Used for | Built-in |
|---|---|---|
| `accent` | focus rings, selected items, highlights | `#3a86ff` |
| `accent-light` | accent text on dark backgrounds | `#73a8ff` |
| `accent-muted` | secondary accent elements | `#5b86d6` |
| `accent-text` | text on accent fills | `#cfe2ff` |
| `focus-fill` | background of the focused row | `#18283c` |
| `text` | primary text | `#f5f6f8` |
| `text-secondary` | values and secondary text | `#a8adb5` |
| `text-dim` | detail lines | `#8e949d` |
| `text-faint` | hints | `#7f858e` |
| `text-disabled` | unavailable items | `#555b64` |
| `background` | screen background | `#0d0e10` |
| `surface` | rows, cards, panels | `#24272d` |
| `border` | outlines | `#30343b` |
| `border-soft` | separators | `#2c3036` |
| `warning` | warnings, favorites star | `#f2b84b` |

Keep enough contrast between `text`, `surface` and `background`: a theme
must keep every screen readable and the focus visible on the handheld
screen and on a TV.
