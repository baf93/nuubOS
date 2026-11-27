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

# Only the SDL3->SDL2 shim is forced onto Wayland. Native SDL2 ports get the
# hint from PortMaster's own mod_nuubOS.txt; keep SDL_VIDEODRIVER writable and
# do not add Wayland scaling or S3TC decompression here (see the 2026-11-26
# regression note in docs/dev/thin-shim-gl-implementacion.md).
export SDL3SHIM_SDL2_VIDEODRIVER=wayland
readonly SDL3SHIM_SDL2_VIDEODRIVER

# Universal Desktop GL -> GLES thin shim. The bare SONAME is resolved by each
# ABI's own ld.so: /usr/lib on aarch64, /usr/lib/arm-linux-gnueabihf on armhf
# (also /usr/lib32 for older launchers). It absorbs the old Mono/LLVM fix and
# redirects PortMaster's bundled gl4es to Mesa GLES (its desktop GL path drew
# a black window on nuubOS); native GLES programs stay untouched. Nested bash
# shells re-read BASH_ENV, so never add the preload twice. BASH_ENV is only
# set for game sessions.
case ":${LD_PRELOAD:-}:" in
  *libgl_nuubos_shim.so*) ;;
  *) export LD_PRELOAD="libgl_nuubos_shim.so${LD_PRELOAD:+:$LD_PRELOAD}" ;;
esac
