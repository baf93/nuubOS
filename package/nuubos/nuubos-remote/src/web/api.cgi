#!/bin/sh
# nuubOS Web administration API (EPIC-038), run by lighttpd after HTTP
# Digest authentication with the device credential.
#
#   GET  ?op=status                    device, storage, library
#   GET  ?op=systems                   systems (upload targets) + game counts
#   GET  ?op=games&system=ID           games of a system (libraryd)
#   GET  ?op=bios                      BIOS Manager status (nuubos-biosctl)
#   POST ?op=scan                      Library scan
#   POST ?op=delete&game=ID            Delete Game (libraryd ownership rules)
#   PUT  ?op=upload&root=roms|bios&path=REL   file body; atomic (.part, rename)
#
# State-changing requests need the header "X-NuubOS: 1" (a cross-site page
# cannot send it), so the browser's cached credential cannot be abused by
# another site. Only /userdata/roms and /userdata/bios are ever written;
# games are deleted only through libraryd. NUUBOS_ROOT prefixes paths for
# the host test.

R="${NUUBOS_ROOT:-}"
ROMS="$R/userdata/roms"
BIOS="$R/userdata/bios"
SYSTEMS_CONF="$R/usr/share/nuubos/systems.conf"
LIBRARYCTL="${NUUBOS_LIBRARYCTL:-/usr/bin/nuubos-libraryctl}"
BIOSCTL="${NUUBOS_BIOSCTL:-/usr/bin/nuubos-biosctl}"

reply()
{
    # $1 status line, $2 JSON body
    printf 'Status: %s\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\n\r\n%s\n' "$1" "$2"
    exit 0
}

fail() { reply "$1" "{\"error\":\"$2\"}"; }

json()
{
    printf '%s' "$1" | tr '\t' ' ' | tr -d '\000-\037' | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}

# Query parameter, URL-decoded (%XX and +).
param()
{
    v="$(printf '%s' "$QUERY_STRING" | tr '&' '\n' | sed -n "s/^$1=//p" | head -n 1)"
    v="$(printf '%s' "$v" | sed 's/+/ /g; s/%\([0-9A-Fa-f][0-9A-Fa-f]\)/\\x\1/g')"
    printf '%b' "$v"
}

require_write()
{
    [ "${HTTP_X_NUUBOS:-}" = 1 ] || fail "403 Forbidden" "csrf"
}

# A relative path inside a root: printable, no "..", no hidden parts,
# bounded depth. Prints nothing when invalid.
safe_rel()
{
    p="$1"
    case "$p" in
        ''|/*|*//*|*/) return 1 ;;
    esac
    printf '%s' "$p" | grep -q '[[:cntrl:]\\]' && return 1
    old_ifs="$IFS"; IFS=/
    depth=0
    for part in $p; do
        depth=$((depth + 1))
        case "$part" in ''|.*) IFS="$old_ifs"; return 1 ;; esac
    done
    IFS="$old_ifs"
    [ "$depth" -le 4 ] || return 1
    printf '%s' "$p"
}

op="$(param op)"
method="${REQUEST_METHOD:-GET}"

case "$op" in
status)
    st="$("$LIBRARYCTL" status 2>/dev/null)"
    games="$(printf '%s\n' "$st" | sed -n 's/^games=//p')"
    scanning="$(printf '%s\n' "$st" | sed -n 's/^scanning=//p')"
    set -- $(df -k "$R/userdata" 2>/dev/null | awk 'NR==2 { print $2, $4 }')
    model="$( (tr -d '\000' < /proc/device-tree/model) 2>/dev/null)"
    version="$(sed -n 's/^VERSION_ID=//p' /etc/os-release 2>/dev/null | tr -d '"')"
    reply "200 OK" "{\"device\":\"$(json "$model")\",\"version\":\"$(json "$version")\",\"games\":${games:-0},\"scanning\":${scanning:-0},\"total_kb\":${1:-0},\"free_kb\":${2:-0}}"
    ;;
systems)
    counts="$("$LIBRARYCTL" status 2>/dev/null | sed -n 's/^system=//p')"
    out=""
    while IFS='|' read -r id name folder exts rest; do
        case "$id" in ''|\#*) continue ;; esac
        n="$(printf '%s\n' "$counts" | awk -F'\t' -v id="$id" '$1 == id { print $3 }')"
        out="$out{\"id\":\"$(json "$id")\",\"name\":\"$(json "$name")\",\"folder\":\"$(json "$folder")\",\"extensions\":\"$(json "$exts")\",\"games\":${n:-0}},"
    done < "$SYSTEMS_CONF"
    reply "200 OK" "[${out%,}]"
    ;;
games)
    sys="$(param system)"
    printf '%s' "$sys" | grep -q '^[a-z0-9_-]\{1,32\}$' || fail "400 Bad Request" "system"
    out=""
    tmp="$(mktemp)" || fail "500 Internal Server Error" "tmp"
    "$LIBRARYCTL" games "system:$sys" 2>/dev/null | sed -n 's/^game=//p' > "$tmp"
    while IFS="$(printf '\t')" read -r id system title cover aspect last time avail fav; do
        out="$out{\"id\":\"$(json "$id")\",\"title\":\"$(json "$title")\",\"available\":${avail:-0}},"
    done < "$tmp"
    rm -f "$tmp"
    reply "200 OK" "[${out%,}]"
    ;;
bios)
    out=""
    tmp="$(mktemp)" || fail "500 Internal Server Error" "tmp"
    "$BIOSCTL" status 2>/dev/null > "$tmp"
    while IFS="$(printf '\t')" read -r a b c d e f; do
        case "$a" in
            system=*) out="$out{\"type\":\"system\",\"id\":\"$(json "${a#system=}")\",\"name\":\"$(json "$b")\",\"state\":\"$(json "$d")\"}," ;;
            file=*) out="$out{\"type\":\"file\",\"system\":\"$(json "${a#file=}")\",\"path\":\"$(json "$b")\",\"required\":${c:-0},\"state\":\"$(json "$d")\",\"description\":\"$(json "$e")\"}," ;;
        esac
    done < "$tmp"
    rm -f "$tmp"
    reply "200 OK" "[${out%,}]"
    ;;
scan)
    [ "$method" = POST ] || fail "405 Method Not Allowed" "method"
    require_write
    "$LIBRARYCTL" scan >/dev/null 2>&1 || fail "503 Service Unavailable" "library"
    reply "200 OK" '{"ok":true}'
    ;;
delete)
    [ "$method" = POST ] || fail "405 Method Not Allowed" "method"
    require_write
    game="$(param game)"
    printf '%s' "$game" | grep -q '^[0-9a-f]\{16\}$' || fail "400 Bad Request" "game"
    if "$LIBRARYCTL" delete "$game" confirm >/dev/null 2>&1; then
        reply "200 OK" '{"ok":true}'
    fi
    fail "409 Conflict" "delete"
    ;;
upload)
    [ "$method" = PUT ] || fail "405 Method Not Allowed" "method"
    require_write
    case "$(param root)" in
        roms) base="$ROMS" ;;
        bios) base="$BIOS" ;;
        *) fail "400 Bad Request" "root" ;;
    esac
    rel="$(safe_rel "$(param path)")" || fail "400 Bad Request" "path"
    [ -n "$rel" ] || fail "400 Bad Request" "path"
    dest="$base/$rel"
    mkdir -p "${dest%/*}" || fail "507 Insufficient Storage" "storage"
    part="$dest.part.$$"
    # Never leave a partial file behind, even if the client disconnects.
    trap 'rm -f "$part"' EXIT INT TERM
    if ! cat > "$part"; then
        fail "507 Insufficient Storage" "storage"
    fi
    if [ -n "${CONTENT_LENGTH:-}" ] && [ "$(wc -c < "$part")" != "$CONTENT_LENGTH" ]; then
        fail "400 Bad Request" "incomplete"
    fi
    mv "$part" "$dest" || fail "507 Insufficient Storage" "storage"
    trap - EXIT INT TERM
    sync
    [ "$(param root)" = roms ] && "$LIBRARYCTL" scan >/dev/null 2>&1
    reply "200 OK" '{"ok":true}'
    ;;
*)
    fail "404 Not Found" "op"
    ;;
esac
