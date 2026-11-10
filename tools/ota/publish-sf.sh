#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Publish a release made by tools/ota/make-update.sh on SourceForge, the
# download and OTA backend of every channel (EPIC-048, maintainer decision
# 2026-11-10).
#
#   NUUBOS_SF_USER=<sourceforge user> tools/ota/publish-sf.sh OUTDIR
#
# Same layout as the GitHub releases, under /home/frs/project/<project>
# (NUUBOS_SF_PROJECT, default nuubos), served by downloads.sourceforge.net:
#   <tag>/                   payload, image, legal-info, SHA256SUMS, README.txt
#   channel-<channel>/       nuubos-update.manifest(.sig)
# The release files go up before the manifest that points to them. Needs
# rsync and the maintainer's SSH key added to the SourceForge account.

PROJECT="${NUUBOS_SF_PROJECT:-nuubos}"
OUT="${1:-}"
[ -f "$OUT/release.env" ] || { echo "Usage: NUUBOS_SF_USER=<user> $0 OUTDIR (made by tools/ota/make-update.sh)" >&2; exit 2; }
[ -n "${NUUBOS_SF_USER:-}" ] || { echo "[FAIL] NUUBOS_SF_USER is not set" >&2; exit 2; }
command -v rsync >/dev/null || { echo "[FAIL] rsync not installed" >&2; exit 1; }
. "$OUT/release.env"
M="$OUT/channel-$CHANNEL/nuubos-update.manifest"
[ -f "$OUT/$TAG/$PAYLOAD" ] && [ -f "$M" ] && [ -f "$M.sig" ] || { echo "[FAIL] $OUT is incomplete" >&2; exit 1; }
DEST="$NUUBOS_SF_USER@frs.sourceforge.net:/home/frs/project/$PROJECT"

# The release notes, shown by SourceForge under the folder's file list.
if [ -f "$OUT/release-notes.txt" ]; then
    { echo "nuubOS $VERSION ($CHANNEL channel)"; echo; cat "$OUT/release-notes.txt"; } > "$OUT/$TAG/README.txt"
fi
echo "Uploading $TAG..."
rsync -rtv --partial --progress -e ssh "$OUT/$TAG/" "$DEST/$TAG/" || exit 1
echo "Pointing channel $CHANNEL to $TAG..."
rsync -rtv -e ssh "$M" "$M.sig" "$DEST/channel-$CHANNEL/" || exit 1
echo "[PASS] $TAG on SourceForge, channel $CHANNEL points to it"
echo "Check: curl -sL https://downloads.sourceforge.net/project/$PROJECT/channel-$CHANNEL/nuubos-update.manifest | grep version"
