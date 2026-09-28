# nuubOS root login profile

if [ -r /etc/profile.d/nuubos-shell.sh ]; then
    . /etc/profile.d/nuubos-shell.sh
fi

#
# Dynamic welcome is intentionally SSH + interactive only.
# Automated SSH commands must produce no unsolicited output.
#
if [ -n "${SSH_CONNECTION:-}" ] &&
   [ -t 0 ] &&
   [ -t 1 ]; then
    /usr/bin/nuubos-welcome
fi
