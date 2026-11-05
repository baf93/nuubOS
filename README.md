# nuubOS

<p align="center"><b>Small System. Big Adventures.</b></p>

<p align="center"><img src="https://raw.githubusercontent.com/wiki/baf93/nuubOS/images/tv_home_recent.png" alt="nuubOS Home" width="820"></p>

A console-style operating system for Anbernic handhelds built on the Allwinner H700.
nuubOS boots straight into a controller-first home screen, plays games through
RetroArch, streams from your PC, plays music and video, and keeps itself up to date
with signed, rollback-safe updates.

> **Status: release candidate (1.0.0-rc.4).** Bug fixes only until 1.0.0; nightly
> images are for testers. Keep a backup of your saves. See [Known issues](https://github.com/baf93/nuubOS/wiki/Known-Issues).

**[Download](https://github.com/baf93/nuubOS/releases)** ·
**[User guide (wiki)](https://github.com/baf93/nuubOS/wiki)** ·
**[Discord](https://discord.gg/VHfUANfVvX)** ·
**[Technical documentation](docs/technical/README.md)** ·
**[Licensing](LICENSING.md)**

> nuubOS does **not** include games, BIOS or any copyrighted files: you bring your own, legally.

## Highlights

- **Console experience**: a Home screen with your recently played games, systems and
  collections, cover art, Find a Game, game tags, and a Quick Menu that works
  everywhere, in games too.
- **Emulation**: about 80 systems through RetroArch and libretro cores tuned for the
  H700. Save states with rotating slots, hotkeys, settings and button layouts per game
  and per system (emulator, picture, accuracy, resolution, frameskip, controls),
  bezels and a CRT look, RetroAchievements with Hardcore Mode and offline unlocks,
  cheats, disc change and LAN netplay.
- **Ports**: PortMaster, downloaded from the console, installs PC games and engines
  ported to handhelds; they play from your library like any other game, each in its
  own sandbox, from the console's card or the SD2 card.
- **Several users**: each person gets their own profile, saves, language, theme
  ("Mood") and settings, Confirm/Back buttons included.
- **PC streaming**: Moonlight for Sunshine and GeForce Experience, and Steam Link (you
  download Valve's app from the console).
- **Media**: a music and video player for local files, network shares and Jellyfin.
  You can turn the screen off and use the console as a music player.
- **Connectivity**: Wi-Fi, Bluetooth audio and controllers, a web control panel over
  HTTPS, SSH/SFTP and a Terminal app, SMB file sharing, Syncthing sync between consoles
  (saves, states, covers and, if you want, games), and screen casting to Chromecast,
  Miracast and DLNA TVs.
- **Two cards**: games on the second card are used as they are, from most other
  firmwares too; saves, states, BIOS files and covers can move there one kind at a
  time when the first card is small.
- **HDMI output** at up to 1080p, with automatic switching between the TV and the
  built-in screen.
- **TV box mode**: runs without its battery and screen, powered from USB on your TV.
- **Updates**: an A/B system with signed updates. If an update does not boot, the
  console rolls back automatically. Pick the Nightly, RC or Stable channel.
- **29 languages.** nuubOS sends no telemetry: see [privacy](docs/privacy.md).

## Supported devices

One image works on every supported device. You choose your model once, when you
prepare the SD card.

| Device | Status |
|---|---|
| RG40XX-V, RG40XX-V V2 | Tested |
| RG CubeXX | Tested |
| RG35XX Pro | Tested (new backlight driver not tested yet) |
| RG35XX H, RG35XX SP, RG40XX H, RG SP | Community tested |
| RG35XX 2024, RG35XX Plus, RG34XX, RG34XX SP, RG28XX | Supported, not tested on hardware yet |

Details and the exact device names: [Supported Devices](https://github.com/baf93/nuubOS/wiki/Supported-Devices).

## Quick start

1. Download `nuubOS-<version>-anbernic-h700.img.xz` from
   [Releases](https://github.com/baf93/nuubOS/releases).
2. Write it to a microSD card (8 GB or larger) with
   [Raspberry Pi Imager](https://www.raspberrypi.com/software/): *Choose OS* →
   *Use custom* → the downloaded file. Answer *No* if Imager offers OS customisation.
3. Open the small `NUUBOS_BOOT` partition on the card, edit `nuubos.cfg` and set your
   device, for example `DEVICE=rg40xx-v` (Windows line endings, spaces and quotes are
   fine).
4. Put the card in slot 1 (SD1) and switch the console on. The first boot sets up the
   storage and opens the setup wizard.

Full instructions: [Installation](https://github.com/baf93/nuubOS/wiki/Installation).

## No games, no BIOS, no copyrighted content

nuubOS is an operating system only. **It does not include, download or link to any
games (ROMs), BIOS or firmware files, or any other copyrighted content** you do not
have the right to use. You add your own files, legally obtained, for example dumped
from cartridges, discs and consoles you own, or free homebrew: see
[Adding Games](https://github.com/baf93/nuubOS/wiki/Adding-Games) and
[BIOS Files](https://github.com/baf93/nuubOS/wiki/BIOS-Files). Issues, pull requests
and discussions asking for or sharing such files are closed and removed.

## Building from source

The build runs in a Docker container with Buildroot. See
[Building](docs/technical/building.md). In short:

```sh
./scripts/bootstrap-buildroot.sh   # Buildroot 2026.08 into external/buildroot
./scripts/dev.sh                   # builds the nuubos-dev:0.5 container, opens a shell in it
./scripts/compile.sh               # inside the container: full image build
```

A first build takes several hours.

## License

nuubOS is **source-available for noncommercial use** under the
[PolyForm Noncommercial License 1.0.0](LICENSE). You may use, study, modify and share
it for any noncommercial purpose. Selling it, or shipping it on hardware that is sold,
is not allowed. nuubUI and the Quick Menu are GPL-3.0, because they are built with the
Slint toolkit. All bundled third-party software keeps its own license. See
[LICENSING.md](LICENSING.md).

nuubOS is an independent project. It is not affiliated with Anbernic, Valve, NVIDIA,
Google, Microsoft or the RetroArch project.

<sub>Screenshots show free homebrew games under their own licenses: *Tobu Tobu Girl* and *Tobu Tobu Girl Deluxe* by Tangram Games ([CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)), *µCity* by Antonio Niño Díaz (GPL-3.0+), *Libbet and the Magic Floor* (zlib), *Concentration Room* and *Thwaite* (GPL-3.0+) by Damian Yerrick. Music: Kevin MacLeod, incompetech.com (CC BY 4.0).</sub>
