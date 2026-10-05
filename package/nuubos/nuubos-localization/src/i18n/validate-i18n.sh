#!/bin/sh
DIR="${1:-.}"
BASE="$DIR/en.lang"
TMP="/tmp/nuubos-i18n-keys-$$"
cut -d= -f1 "$BASE" > "$TMP.base" || exit 1
STATUS=0
for f in "$DIR"/*.lang
do
  cut -d= -f1 "$f" > "$TMP.cur" || STATUS=1
  cmp -s "$TMP.base" "$TMP.cur" || { echo "[FAIL] i18n key mismatch: $f"; STATUS=1; }
done
rm -f "$TMP.base" "$TMP.cur"
[ "$STATUS" -eq 0 ] && echo "[PASS] i18n catalogs complete"
exit "$STATUS"
