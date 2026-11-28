# armhf runtime — provenance and licences

`armhf-runtime.tar.xz` is a **minimal, prebuilt** 32-bit ARM userland, not a
source release of nuubOS. It exists because nuubOS is an aarch64-only
Buildroot image and PortMaster still publishes ports with `*.armhf` ELF
binaries (for example Apotris and Abe's Adventure). Those binaries carry
`/lib/ld-linux-armhf.so.3` in `PT_INTERP`, so without this package they cannot
even be started on an aarch64-only rootfs.

## What is inside

Only the dependency closure of a real 32-bit PortMaster port (Apotris, SDL2
renderer) plus `libstdc++`/`libgcc` for C++ ports:

- glibc 2.39 (`libc6:armhf`, `libgcc-s1:armhf`, `libstdc++6:armhf`)
- SDL2 2.30.0 (`libsdl2-2.0-0:armhf`) and its X11/Wayland/ALSA/PulseAudio
  dependencies
- zlib 1.3 (`zlib1g:armhf`)

Mesa 25.2.8 (`mesa-libgallium`, `libegl-mesa0`, `libglvnd0`, `libglapi-mesa`,
`libgles2`) is also included, with the complete `DT_NEEDED` closure
(`libLLVM.so.20.1` ≈124 MB, `libGLdispatch.so.0`, libdrm, X11/glvnd) and the
`panfrost_dri.so` / `swrast_dri.so` / `kms_swrast_dri.so` DRI drivers. The
`50_mesa.json` glvnd vendor file is installed in
`/usr/share/glvnd/egl_vendor.d/` so 32-bit EGL/GLES contexts work. Native
aarch64 ports keep using the image's Mesa 26.2.7 and the preloaded GL shim;
the 32-bit `libGL.so.1` remains the multiarch GL shim.

## How it was produced

Inside `nuubos-dev:0.5` (Ubuntu 24.04):

1. `dpkg --add-architecture armhf`
2. Add a deb822 source for `http://ports.ubuntu.com/ubuntu-ports`, suites
   `noble noble-updates noble-security`, components `main universe`,
   `Architectures: armhf`; restrict the existing amd64 sources to
   `Architectures: amd64` so apt does not request armhf indexes from
   `archive.ubuntu.com` (they do not exist).
3. `apt-get install --download-only --no-install-recommends libc6:armhf
   libgcc-s1:armhf libstdc++6:armhf zlib1g:armhf libsdl2-2.0-0:armhf`
4. `dpkg-deb -x` each `.deb`, then copy only the ELF dependency closure
   (following `DT_NEEDED` and symlink chains, plus
   `pulseaudio/libpulsecommon-16.1.so`) into `usr/lib/arm-linux-gnueabihf/`.
5. The same extraction and `DT_NEEDED` closure was applied to the armhf Mesa
   packages (`mesa-libgallium_25.2.8-0ubuntu0.24.04.5_armhf.deb`,
   `libegl-mesa0`, `libglvnd0`, `libglapi-mesa`, `libgles2`, `libllvm20`,
   `libdrm2`, `libgl1-mesa-dri`), adding `/usr/share/glvnd/egl_vendor.d/50_mesa.json`.
6. `tar -cJf armhf-runtime.tar.xz usr/lib/arm-linux-gnueabihf usr/share/glvnd`

The package installs that tree verbatim and creates
`/lib/ld-linux-armhf.so.3` -> `/usr/lib/arm-linux-gnueabihf/ld-linux-armhf.so.3`.
PortMaster already recognises `/usr/lib/arm-linux-gnueabihf` as
`DEVICE_HAS_ARMHF=Y` (`PortMaster/device_info.txt`) and appends its own
`libs.armhf` to `LD_LIBRARY_PATH` for 32-bit ports.

## Licences

The binaries are unmodified Ubuntu noble armhf packages. Their upstream
licences include LGPL-2.1+ (glibc, SDL2, PulseAudio, libgcrypt, libsystemd),
GPL-2.0+/GPL-3.0+ with the GCC Runtime Library Exception (libgcc, libstdc++),
MIT/X11 (X11, xkbcommon, libdrm, Wayland, Mesa), zlib (zlib, libsdl2), and
BSD-like (libbsd, libexpat). LLVM is Apache-2.0 WITH LLVM-exception. The full
per-package copyright and licence text
is in `/usr/share/doc/<package>/copyright` of each source deb and in the
Ubuntu archive metadata for the exact versions listed above. nuubOS ships no
game data, firmware or BIOS with this runtime.
