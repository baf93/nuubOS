#!/bin/sh

nuubos_userdata_prepare_users()
{
    NUUBOS_USERDATA_ROOT="$1"

    mkdir -p "$NUUBOS_USERDATA_ROOT/users" || return 1

    for NUUBOS_USERDATA_STATE_USER in /state/users/*; do
        [ -d "$NUUBOS_USERDATA_STATE_USER" ] || continue

        NUUBOS_USERDATA_USER_ID="${NUUBOS_USERDATA_STATE_USER##*/}"

        [ -f "$NUUBOS_USERDATA_STATE_USER/profile.conf" ] || return 1

        grep -qx \
            "USER_ID=$NUUBOS_USERDATA_USER_ID" \
            "$NUUBOS_USERDATA_STATE_USER/profile.conf" || return 1

        mkdir -p \
            "$NUUBOS_USERDATA_ROOT/users/$NUUBOS_USERDATA_USER_ID/saves" \
            "$NUUBOS_USERDATA_ROOT/users/$NUUBOS_USERDATA_USER_ID/states" \
            "$NUUBOS_USERDATA_ROOT/users/$NUUBOS_USERDATA_USER_ID/screenshots" \
            "$NUUBOS_USERDATA_ROOT/users/$NUUBOS_USERDATA_USER_ID/recordings" \
            "$NUUBOS_USERDATA_ROOT/users/$NUUBOS_USERDATA_USER_ID/library" \
            "$NUUBOS_USERDATA_ROOT/users/$NUUBOS_USERDATA_USER_ID/appdata" ||
            return 1
    done

    return 0
}
