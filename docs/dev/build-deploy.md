# Build, deploy, target access

Rules: CLAUDE.md "Build". History: the Dockerized Platform v0.5 build produced rootfs, boot/state images and the universal H700 image; Linux 7.2.8 also completed a full Dockerized build with the H700 patchset applying.

## Targeted build (§37.1)
```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/workspace/.docker-home \
  -e BR2_DL_DIR=/workspace/dl -e BR2_CCACHE_DIR=/workspace/.cache/nuubos/buildroot-ccache \
  -v "$PWD:/workspace" -w /workspace nuubos-dev:0.5 \
  ./scripts/compile.sh <pkg>-rebuild [<pkg>-rebuild ...]
```

`compile.sh` forwards its arguments to Buildroot `make` and restores Buildroot pristine on exit. Gotcha: when several `-rebuild` targets go in one invocation, a package that is also a dependency of an earlier one (e.g. `nuubos-input`) is already "made" before its own clean runs and is silently NOT rebuilt — check for its `>>> <pkg> Building` line, or rebuild it alone. Packages touched most often: `nuubui-home`, `nuubos-quick-menu`, `nuubos-localization`, `nuubos-audio`, `nuubos-system`. Results land in `output/h700/target/`. A Rust UI rebuild takes ~40 s.

## Development target and deploy (§37.2) — ALWAYS deploy after a change when no flash is needed
- Target: RG40XX-V at `root@192.168.5.36`, `scp`/SFTP available. SSH is password-only since 2026-10-07 (device credential, Settings → System → Remote Services; keys are not accepted): the user supplies it. Non-interactive use from the host without sshpass: an askpass script that prints it, `SSH_ASKPASS=<script> SSH_ASKPASS_REQUIRE=force ssh ...` (keep the script outside the repo, e.g. the session scratchpad).
- The target runs the current working tree (all services incl. users/regional/localization/PipeWire).
- Use `tools/deploy-target.sh [-s S50nuubos-ui] [-s S43nuubos-audiod ...] /abs/path ...`: stages files from `output/h700/target`, backs up originals in `/tmp/nuubos-deploy/backup`, verifies md5, stops the given init scripts, installs, restarts them (reverse order).
- Screenshots of the real screen: `tools/grab-target.sh <name>` → `output/grab/<name>-crtcN.png` (reads the scanout framebuffer via DRM GETFB2; dev tool in /tmp only). There is no input injection; ask the user to navigate.
- HDMI density: `nuubos-wl-output-apply` sets the output scale so the UI sees ~720 logical lines (1080p → 1.5, 1440p → 2, 2160p → 3; user found 2.0 at 1080p too large). Home (winit) handles fractional scale natively; the Quick Menu uses wp_fractional_scale_v1 + wp_viewporter and renders at physical resolution.
- Output policy (HDMI only, preferred mode; internal panel otherwise) lives in `nuubos-wl-output-apply`; `nuubos-wl-output-watch` (labwc autostart) applies it at session start only if HDMI is connected and the topology does not already match, then on DRM hotplug. Rootfs-overlay scripts are deployed by copying the file from `board/nuubos/common/rootfs-overlay/` (no package rebuild).
- After restarting `S43nuubos-audiod`, Home Music only restarts when the UI requests it again: restart `S50nuubos-ui` too.
- UI binaries (`/usr/bin/nuubui-home`, `/usr/bin/nuubos-quick-menu`) and `/usr/share/nuubos/i18n/*.lang`: `/etc/init.d/S50nuubos-ui stop`, install, `/etc/init.d/S50nuubos-ui start`, check `/run/nuubos/ui-ready`.
- Services: use their own `/etc/init.d/S??nuubos-*` stop/start.
- NEVER `cat`/`grep` over `/run/nuubos/*`: `/run/nuubos/ui-control` is a FIFO and the command blocks forever (and steals lifecycle messages).
- Target has `wlr-randr`, `wpctl`, `pw-cli`, `pw-dump`; no `grim`, no `pkill` (use `kill`/`killall`). Wayland env: `XDG_RUNTIME_DIR=/run/nuubos/wayland-runtime WAYLAND_DISPLAY=wayland-0`.
- Logs: `/run/nuubos/*.log` (regular files), `labwc.log` holds UI client stderr.
- Bulk deploy (many packages): compare md5 of the target rootfs with `output/h700/target`, stripping ELFs with the toolchain `strip --remove-section=.comment --remove-section=.note` first (target files are unstripped until finalize). `output/h700/target` is pre-finalize: never copy its `etc/passwd|group|shadow|timezone`, `bin/busybox`, or files the rootfs overlay owns (take those from `board/nuubos/*/rootfs-overlay`); `/etc/os-release` is written by post-build. Missing `.so.N` symlinks must be shipped too. Target BusyBox `tar` has no `-z`: `gunzip -c x.tgz | tar xf - -C /`. Back up replaced files to `/userdata` first (done 2026-10-07: `/userdata/nuubos-deploy-backup-20261007.tgz`).
- Kernel: `/boot/Image` lives on the rootfs (mmcblk0p1, rw). Kernel-only change: add the patch under `patches/linux/h700/`, apply it to `output/h700/build/linux-7.2.8` (already-patched tree), `compile.sh linux-rebuild`, scp `output/h700/images/Image` to `/boot/Image` (keep a backup), reboot.
- Target has no `depmod`: after adding a module, run `output/h700/host/sbin/depmod -b output/h700/target 7.2.8` and deploy `modules.*` with the `.ko`.
- New crates (MIT/Apache, Slint femtovg same terms as Slint): `femtovg`, `fnv`, `gbm`, `gbm-sys`, `i-slint-renderer-femtovg`; fetch into the Buildroot cargo cache with `cargo fetch` run in the container with `CARGO_HOME=/workspace/dl/br-cargo-home` and `PATH=/workspace/output/h700/host/bin:...` (local cargo packages build `--offline --locked` from that cache).
- Buildroot package patches must apply with fuzz 0; FFmpeg bump leftovers: streaming-media.md §37.14.

## SSH / SFTP (§29)
Slow SSH on Linux 7.2.8 was reverse DNS, not a kernel regression: the development host mapping on the target fixed it. Don't reopen as a kernel bug without new evidence. The target ships an SFTP server so scp/SFTP work natively (no stdin-over-SSH workarounds); check repo state before reimplementing.
