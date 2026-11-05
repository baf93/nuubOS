# Releasing

How the maintainer publishes a nuubOS image and its over-the-air update. The same
steps serve the Nightly, RC and Stable channels.

## Before you start

- The tree is committed and pushed: the image records `git describe`, and a
  `-dirty` build must not be published.
- No other build runs on `output/`.
- GitHub CLI installed and signed in with write access: `gh auth status`.
- The release passphrase is at hand (the maintainer's password manager).

## 1. Build the image for the channel

The device's default channel is compiled into `nuubos-update`. Build it for the
channel you publish on, then the image:

```sh
./scripts/compile.sh NUUBOS_UPDATE_DEFAULT_CHANNEL=nightly nuubos-update-rebuild
./scripts/compile.sh
./scripts/compile.sh legal-info
```

(in the development container, see [building.md](building.md)). The build serial
(`/usr/share/nuubos/build-serial`) is set when the rootfs is generated; it orders
releases on the devices.

## 2. Qualify

No separate pass (maintainer decision, 2026-10-24): every change is deployed and
checked on the development console while it is made (RG CubeXX), so a Nightly or an
RC is the tree already tested there, and a Stable release follows the release
candidates that the community has tested on their devices.

## 3. Create the release files

```sh
tools/ota/make-update.sh ~/nuubos-release nightly ~/nuubos-release-notes.txt
tools/ota/add-image.sh ~/nuubos-release
```

- `make-update.sh` asks for the passphrase, checks it against `update-key.pub`,
  compresses `rootfs.ext4` into the payload and signs the manifest. The notes file
  is plain text, at most 4000 bytes, one item per line: it appears on the console
  under Software Update → What's New.
- `add-image.sh` adds the flashable `.img.xz`, `legal-info` (split into 1900 MB
  parts, below GitHub's 2 GiB asset limit; without `host-sources`, the build tools
  that never reach the image, and without the linux-firmware source tarball: only binary
  firmware ships, its licenses stay), the third-party notices and
  `SHA256SUMS`.
- Optional: `~/nuubos-release/github-notes.md` becomes the GitHub release text
  (installation steps, checksums). Without it, the console notes are used.

## 4. Publish

```sh
git push
tools/ota/publish.sh ~/nuubos-release
```

`publish.sh` creates the versioned release `v<version>` with every file
(a pre-release unless the channel is stable), then replaces the manifest and
signature in `channel-<channel>`. Devices on that channel see the update at their
next check (within a day, or at once with Check).

## 5. After publishing

- Check from a device on the channel: Settings → System → Software Update → Check.
- Update the wiki pages that changed (Known Issues, Release Notes).

## Rejoining legal-info

```sh
cat nuubOS-<version>-legal-info.tar.part* | tar -xf -
```

## Versions and channels

nuubOS uses [Semantic Versioning](https://semver.org): `MAJOR.MINOR.PATCH`.

| Channel | Version | Example | Meaning |
|---|---|---|---|
| Stable | `X.Y.Z` | `1.0.0`, `1.0.1` | qualified on every Tested device (README) |
| RC | `X.Y.Z-rc.N` | `1.0.0-rc.1` | release candidate N of `X.Y.Z`: bug fixes only |
| Nightly | `X.Y.Z-nightly.YYYYMMDD` | `1.0.0-nightly.20261020` | development build towards `X.Y.Z`, UTC build date |

- Exception for 1.0.0 (maintainer decision, 2026-10-24): `1.0.0-rc.3` also brings
  PortMaster and the A/B button swap in the menus, the most requested features
  (per-system settings, planned too, moved after 1.0.0); `1.0.0-rc.4` (maintainer
  decision, 2026-10-26) brings the bug fixes from RC3 plus four small community
  requests: power key short press = Sleep, a terminal app, hiding RetroArch from the
  Quick Menu, disc change for multi-disc games in the Quick Menu. While it was being
  prepared the maintainer added more community requests (2026-10-27..30): system
  settings and controls per game and per system, frameskip and resolution per game,
  data and ports on SD2, offline achievements, PSP games in a folder, advanced
  metadata search, cover region, volume button step, vibration intensity, picture
  position, Wrap Around Rows, Syncthing ROM folders, OpenSSL 4 and the melonDS
  performance work. `1.0.0-rc.4` was released ahead of its 2026-11-02 target
  (maintainer decision, 2026-10-30); features freeze after it. `1.0.0-rc.5`, the
  last RC, brings bug fixes plus one exception: a reorganization of Settings
  (docs/dev/open-items.md); then `1.0.0`.
- Qualification (maintainer decision, 2026-10-27): "Tested" = the maintainer's
  RG40XX-V, RG CubeXX and RG35XX Pro; every other board stays "Community tested"
  or "Supported, not tested", also for 1.0.0, until more people officially join
  the development team.
- `PATCH` = bug fixes only, `MINOR` = new features, `MAJOR` = an incompatible change
  (a new SD card layout, a reflash needed).
- `VERSION` holds the release in preparation: `1.0.0-rc.1` while RC 1 of 1.0.0 is
  being prepared, `1.0.0` for the stable release. Bump it in the commit that cuts
  the release (`1.0.0-rc.1` → `1.0.0-rc.2` → `1.0.0` → `1.0.1-rc.1` or `1.1.0-rc.1`).
- The image's channel (`NUUBOS_UPDATE_DEFAULT_CHANNEL`, installed as
  `/usr/share/nuubos/release-channel`) decides the version written to
  `/etc/os-release`: a nightly build takes `X.Y.Z` from `VERSION` and adds the build
  date; an RC build needs `VERSION` = `X.Y.Z-rc.N`, a stable build `X.Y.Z`. A
  mismatch stops the image build, so an image cannot carry the wrong version.
- The release tag is `v<version>` (`v1.0.0-rc.1`, `v1.0.0-nightly.20261020`), the
  files `nuubOS-<version>-…`. One nightly per day; a second one the same day needs a
  new build the next day or a `VERSION` bump.
- Devices order releases by the build serial (`/usr/share/nuubos/build-serial`,
  seconds since 1970 at image build), never by the version text: a nightly built
  after an RC is newer for the nightly channel.
- Settings → System → Software Update shows the version and the build (`git describe`).
- Before 2026-10-20 every image was `0.5` (tags `v0.5-<serial>`).
