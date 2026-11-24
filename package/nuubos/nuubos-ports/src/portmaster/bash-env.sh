#!/bin/bash
#
# SPDX-License-Identifier: GPL-3.0+
#
# BASH_ENV for port sessions (set by nuubos-portsd). PortMaster launchers
# export graphics hints *after* mod_nuubOS.txt, which is sourced too early to
# help. Declaring these two names readonly without a value lets the launcher
# continue while its late exports fail; children inherit neither variable.
# The SDL3 -> SDL2 shim only uses SDL2's Wayland backend when hinted, instead
# of letting Mesa pick surfaceless EGL configs it cannot satisfy.

readonly EGL_PLATFORM
readonly SDL_RENDER_DRIVER
export SDL3SHIM_SDL2_VIDEODRIVER=wayland
readonly SDL3SHIM_SDL2_VIDEODRIVER

# Mesa 26 ships libLLVM.so.22.1; mono-sgen has a static LLVM with the same
# symbols, so Mesa's dlopen() needs RTLD_DEEPBIND or a shader compile writes
# into mono's uninitialized data. MonoGame also needs desktop-GL extension
# names that GLES3 has as core. The shim only takes effect when /proc/self/exe
# starts with mono or MONOGAME_PATCH is set; other ports get an inert preload.
# BASH_ENV is set only for game sessions, never for PortMaster.sh itself.
export LD_PRELOAD="/usr/lib/nuubos/ports/mono-gl-fix.so${LD_PRELOAD:+:$LD_PRELOAD}"
