# libretro packages (EPIC-013)

RetroArch is the nuubOS emulation backend; `nuubos-emud`
(`package/nuubos/nuubos-emulation`) owns game sessions. These packages build
RetroArch and the default core matrix for Allwinner H700 (Cortex-A53 4x1.5 GHz,
Mali-G31 MP2 on Mesa Panfrost, GLES only).

## Default core matrix

The order in `nuubos-emulation/src/cores.conf` decides which core a system
uses (bundled cores first, then cores downloaded from RetroArch Advanced into
`/userdata/retroarch/cores`). Systems marked *partial* run only part of their
library at full speed on H700. *BIOS* means the user must provide the
console's own firmware in `/userdata/bios` (not redistributable); without it
the game does not start.

| Systems (folders) | Core | Licence | Notes |
|---|---|---|---|
| NES, FDS | FCEUmm | GPL-2.0+ | FDS: BIOS `disksys.rom` |
| SNES, Satellaview, Sufami Turbo | Snes9x | **non-commercial** | BS-X / Sufami: BIOS |
| Game Boy / Color | Gambatte | GPL-2.0 | |
| Game Boy Advance | mGBA | MPL-2.0 | |
| Nintendo 64 | Mupen64Plus-Next (GLES3, 320x240) | GPL-2.0 | partial; homebrew built with libdragon's open-source IPL3 (2023+) does not boot (the ARM64 dynarec exits on it) |
| Nintendo DS | melonDS (ARM64 JIT, touch on right stick) | GPL-3.0 | partial |
| Virtual Boy | Beetle VB | GPL-2.0 | |
| Pokémon Mini | PokeMini | GPL-3.0 | free BIOS built in |
| SG-1000, Master System, Game Gear, Mega Drive, Mega-CD | Genesis Plus GX | **non-commercial** | Mega-CD: BIOS |
| 32X | PicoDrive | **non-commercial** | |
| Saturn | YabaSanshiro (A53/GLES3 profile) | GPL-2.0 | partial; BIOS `saturn_bios.bin` (the core refuses to start without one, even with HLE forced) |
| Dreamcast, NAOMI, Atomiswave | Flycast (A53/GLES profile) | GPL-2.0 | partial; NAOMI/Atomiswave: BIOS |
| PlayStation | PCSX ReARMed | GPL-2.0 | HLE BIOS |
| PSP | PPSSPP | GPL-2.0+ | partial; assets shipped |
| PC Engine / CD | Beetle PCE Fast | GPL-2.0 | CD: BIOS `syscard3.pce` |
| Arcade, FBNeo, CPS-1/2/3, Neo Geo | FinalBurn Neo | **non-commercial** | Neo Geo: `neogeo.zip` |
| Neo Geo (Geolith) | Geolith | BSD-3-Clause | BIOS |
| Neo Geo CD | NeoCD | LGPL-3.0 | BIOS |
| MAME | MAME 2003-Plus (0.78 sets) | **non-commercial** | |
| Neo Geo Pocket / Color | Beetle NeoPop | GPL-2.0 | |
| WonderSwan / Color | Beetle Cygne | GPL-2.0 | |
| Lynx | Handy | Zlib | |
| Atari 2600 | Stella 2014 | GPL-2.0 | |
| Atari 5200 | a5200 | GPL-2.0 | BIOS `5200.rom` |
| Atari 7800 | ProSystem | GPL-2.0 | |
| Atari 800 / XE | Atari800 | GPL-2.0 | free Altirra OS built in |
| Atari ST | Hatari + EmuTOS | GPL-2.0+ | EmuTOS shipped as `tos.img` |
| Channel F | FreeChaF | GPL-3.0 | BIOS |
| ColecoVision | Gearcoleco | GPL-3.0 | BIOS `colecovision.rom` |
| Intellivision | FreeIntv | GPL-3.0 | BIOS `exec.bin`, `grom.bin` |
| Odyssey² | O2EM | Artistic-2.0 | BIOS `o2rom.bin` |
| Supervision | Potator | Unlicense | |
| Mega Duck | SameDuck | MIT | |
| Vectrex | vecx (software renderer) | GPL-3.0 | built-in system ROM |
| C64, C128, Plus/4, VIC-20 | VICE x64 / x128 / xplus4 / xvic | GPL-2.0 | ROMs embedded by VICE, see below |
| Amiga | PUAE | GPL-2.0 | AROS Kickstart replacement built in (boots, but most floppy games need the real Kickstart `kick34005.A500` / `kick40068.A1200`) |
| Amstrad CPC | Caprice32 | GPL-2.0 | Amstrad ROMs embedded (redistribution permitted by Amstrad) |
| ZX Spectrum | Fuse | GPL-3.0 | Amstrad ROMs embedded (permitted) |
| SAM Coupé | SimCoupé | GPL-2.0 | ROM embedded (permitted); patched to insert the loaded disk |
| MSX / MSX2 / MSX2+ | blueMSX + C-BIOS | GPL-2.0, C-BIOS BSD | only C-BIOS machines shipped |
| Macintosh | Mini vMac | GPL-2.0 | BIOS `MacII.ROM` / `vMac.ROM` |
| PC-88 | QUASI88 | BSD-3-Clause | BIOS |
| PC-98 | Neko Project II kai | MIT | BIOS |
| Sharp X1 | X Millennium | BSD-3-Clause | BIOS |
| Sharp X68000 | PX68K | GPL-2.0 | BIOS |
| Palm OS | Mu | **CC BY-NC 3.0** | BIOS `palmos41-en-m515.rom` |
| MS-DOS | DOSBox Pure | GPL-2.0 | |
| ScummVM | ScummVM (lite engine set) | GPL-3.0+ | engine data and themes shipped |
| PICO-8 | FAKE-08 | MIT | |
| TIC-80 | TIC-80 | MIT | |
| Lutro | Lutro | MIT | |

Decision (2026-10-06, project owner): the non-commercial cores are included
because nuubOS is distributed free of charge; they must be removed or replaced
before any commercial distribution. Every package records its licence and
licence file, so `make legal-info` lists them.

Open licensing item: VICE embeds the Commodore KERNAL/BASIC/character ROMs
as upstream VICE distributes them. Their redistribution provenance must be
confirmed before a public release (CLAUDE §10); if it cannot be, ship VICE
without ROMs and ask users for them like other BIOS files.

Not shipped on purpose: blueMSX's proprietary MSX/ColecoVision BIOS images,
Hatari's capsimage (IPF) library (not free software), Drastic (closed source,
no redistribution licence). Systems still missing: EasyRPG (needs liblcf and
an ICU stack), the MAME-only machines (Apple II, CoCo, Dragon, Gamate,
Game.com, Sord M5, TI-99, TRS-80, VTech, Cassette Vision, Oric, FM Towns,
LaserActive), Moonlight and PortMaster (not emulators: product services of
their own), OpenBOR (standalone engine).

Other redistributed material:

- RetroArch 1.22.2: GPL-3.0+, with two nuubOS patches (stdin commands with
  results, Wayland EGL config for GLES2 contexts in GLES3 builds).
- `retroarch-assets` subset (Ozone, XMB monochrome icons, fallback/OSD fonts):
  CC-BY-4.0, `COPYING` installed in `/usr/share/retroarch/assets`.
- `libretro-core-info`: MIT.
- `nuubos-emulation` data (defaults, sharp-bilinear shader, gamepad
  autoconfig): MIT, written for nuubOS.

## Adding a core

1. Qualify it on H700 (full-speed library coverage, save states, input,
   suspend/resume) before bundling it; otherwise list it in `cores.conf` only
   as a downloadable alternative.
2. Add `package/libretro/libretro-<name>/` (pinned commit, licence, licence
   file), source it in `Config.in` and select it from
   `BR2_PACKAGE_NUUBOS_EMULATION`.
3. Add or reorder the system line in `cores.conf` and the row above.

Build notes: `libretro.mk` builds Makefile-based cores with `platform=unix`
and GCC 15 compatibility flags (C pinned to gnu11, old pointer/int warnings not
fatal). mGBA builds as a CMake target; PicoDrive builds without
`_LARGEFILE64_SOURCE` (it would bypass the libretro VFS); PCSX ReARMed builds
with `HAVE_PHYSICAL_CDROM=0` (its physical CD code references `dir_list_new()`
without building it, so the core failed to load).
