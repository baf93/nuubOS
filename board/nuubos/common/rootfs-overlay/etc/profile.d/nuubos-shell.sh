# nuubOS interactive shell environment

case "$-" in
    *i*)
        ;;
    *)
        return 0 2>/dev/null || exit 0
        ;;
esac

export EDITOR=vi
export VISUAL=vi
export PAGER=more

if [ -t 1 ] &&
   [ "${TERM:-dumb}" != "dumb" ] &&
   [ -z "${NO_COLOR:-}" ]; then

    C_RESET="$(printf '\033[0m')"
    C_BOLD="$(printf '\033[1m')"
    C_CYAN="$(printf '\033[36m')"
    C_GREEN="$(printf '\033[32m')"
    C_BLUE="$(printf '\033[34m')"
else
    C_RESET=""
    C_BOLD=""
    C_CYAN=""
    C_GREEN=""
    C_BLUE=""
fi

HOST_SHORT="$(hostname 2>/dev/null || printf 'nuubos')"

#
# Keep ${PWD} literal here: ash expands it whenever the prompt is drawn,
# therefore the current directory follows cd without requiring bash.
#
PS1="${C_BOLD}${C_CYAN}nuubOS${C_RESET} ${C_GREEN}root@${HOST_SHORT}${C_RESET}:${C_BLUE}"'${PWD}'"${C_RESET} # "
export PS1

alias ll='ls -lah'
alias la='ls -A'
alias l='ls -lh'
alias cls='clear'
alias dfh='df -hT'

ninfo()
{
    /usr/bin/nuubos-welcome
}

nstorage()
{
    if [ -r /run/nuubos/storage/tf2.status ]; then
        cat /run/nuubos/storage/tf2.status
    else
        echo "nuubOS storage state unavailable"
    fi
}
