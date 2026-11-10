# Licensing

nuubOS is **free and open-source software** under the
[GNU General Public License, version 3 or later](LICENSE) (SPDX: `GPL-3.0-or-later`).
You may use, study, modify and share it for any purpose, commercial use included.
If you distribute nuubOS or a modified version, you must make the complete source
code available under the same license.

## What the license covers

The original nuubOS work in this repository is licensed under the GPL-3.0-or-later.
This covers the nuubOS services, the web administration, scripts, tools, board
configuration and overlays, documentation, translations, and the original sounds and
images.

History: code published in this repository before 2026-10-16 was under the MIT
License; from 2026-10-16 to 2026-11-10 (up to and including 1.0.0-rc.4) under the
PolyForm Noncommercial License 1.0.0. Copies obtained under those terms keep them.
From 2026-11-10 on (1.0.0-rc.5 and later), nuubOS is GPL-3.0-or-later. Contributions made under the
contribution terms in [docs/technical/contributing.md](docs/technical/contributing.md)
are covered by this change.

## Name and logo

The license covers the code, not the name. "nuubOS" and the nuubOS logo identify the
official builds published by the maintainer. You may say that your work is based on
nuubOS, but a modified or rebuilt version must use another name and logo, and must
not suggest that it is an official release.

## What it does not cover

| Path / component | License |
|---|---|
| `package/nuubos/nuubui-home` (nuubUI) and `package/nuubos/nuubos-quick-menu` | GPL-3.0-only. Both are built with the [Slint](https://slint.dev) toolkit, which a console OS can use for free only under the GPL-3.0. |
| `patches/linux/`, `patches/u-boot/`, `patches/buildroot/`, `patches/packages/` | The license of the project each patch applies to (Linux: GPL-2.0; see each patch's SPDX lines) |
| Buildroot recipes (`*.mk`, `Config.in`, `external.*`, `configs/*_defconfig`) | GPL-2.0-or-later, like Buildroot |
| `package/nuubos/labwc` (patched labwc) | GPL-2.0-or-later |
| Third-party assets inside nuubOS packages (icons, fonts, avatars, music, OpenH264 headers) | As stated in the package's `LICENSE_FILES` and the `ATTRIBUTION`/`LICENSE` files next to them (CC-BY-4.0, CC0-1.0, OFL-1.1, BSD-2-Clause, ...) |
| Every other program in a nuubOS image (Linux, U-Boot, RetroArch, libretro cores, Syncthing, Mesa, ...) | Its own license, listed in the image's `legal-info` |

Some bundled libretro cores (FBNeo, Genesis Plus GX, MAME 2003-Plus, PicoDrive,
Snes9x and others) forbid commercial use under their own terms, independently of the
nuubOS license: a nuubOS image that includes them cannot be sold or shipped on
hardware that is sold.

## Images and source code

Every published nuubOS image comes with its `legal-info` archive: the manifest of all
packages with their licenses, license texts, and the complete corresponding source
code of the GPL/LGPL components and the patches applied to them. The archive is
attached to the same GitHub release as the image. Third-party notices for the Rust
crates and Go modules linked into nuubOS programs are installed in the image under
`/usr/share/licenses/`.

nuubOS never ships emulator BIOS or firmware files.
