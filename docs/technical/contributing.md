# Contributing

Bug reports, translations and patches are welcome.

## Reporting a bug

Open an issue at <https://github.com/baf93/nuubOS/issues> with:

- device model and `DEVICE` value, nuubOS version (Settings → System → System
  Information);
- what you did, what you expected, what happened;
- a support bundle when possible: Settings → System → Troubleshooting →
  Diagnostics → Save Diagnostic Report. It is saved in `/userdata/diagnostics/`.
  Passwords, keys and tokens are removed from it; ROMs, BIOS and saves are never
  included.

## Code

Read [docs/dev/principles.md](../dev/principles.md) and the area file in
[docs/dev/](../dev/README.md) first. The short version:

- Put behaviour in the service that owns the capability, never in the UI.
- No polling, no timers while idle, no sleeps or retries in place of a root cause.
- Every visible string goes through localization and exists in all 29 catalogs.
- Layouts must work at 640x480, 720x720, 720x480 and on a 1080p TV.
- Controller first: everything must work with buttons only.
- Build with `./scripts/compile.sh` in the container, rebuilding only what changed.
- Licenses: check the license of every new dependency or asset before adding it.
  nuubOS never ships emulator BIOS or firmware.

## Translations

Catalogs are `package/nuubos/nuubos-localization/src/i18n/<lang>.lang`, one
`NNN=text` line per string, indexed like `en.lang`. To fix a translation, change the
line in that language's file and run `sh validate-i18n.sh .` in that folder. The
web administration has a few strings of its own in
`package/nuubos/nuubos-remote/src/web/www/i18n.js`.

Many of the 29 languages were machine-written and need a review by native
speakers: corrections are especially welcome.

Adding a language touches several places (catalog, UI language list, setup
wizard greeting, services that map language codes): see "Catalogs" in
[docs/dev/ui.md](../dev/ui.md).

## License of contributions

nuubOS is licensed under the GPL-3.0-or-later, with nuubUI and the Quick Menu
under GPL-3.0-only (see [LICENSING.md](../../LICENSING.md)). By
submitting a contribution you agree that it is licensed under the license of the
files it changes, and that the maintainer may also offer it under other terms (for
example a commercial license), so the project can keep a single licensor.
