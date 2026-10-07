# Platform: kernel, image, storage, display, power

## Kernel / hardware (§9.1)
Linux mainline 7.2.x. 7.2.8 passed the Dockerized build and the RG40XX-V quick regression: 143 PASS / 0 WARN / 0 FAIL / 9 SKIP (foundation, DRM/Panfrost/display/backlight, analog audio baseline, Cedrus/VPU, RTL8821CS Wi-Fi 5 GHz, Bluetooth, input/rumble capabilities, storage/STATE/USERDATA/USB, RGB). Newer repo/test records are authoritative.

## Universal H700 image (§9.2)
One image, device chosen explicitly by `DEVICE=` in `NUUBOS_BOOT/nuubos.conf`. Selectors: rg-sp rg28xx rg34xx rg34xx-sp rg34xx-sp-v2 rg35xx-2024 rg35xx-2024-v6 rg35xx-h rg35xx-h-v6 rg35xx-plus rg35xx-plus-v6 rg35xx-pro rg35xx-sp rg35xx-sp-v2 rg40xx-h rg40xx-h-v2 rg40xx-v rg40xx-v-v2 rgcubexx. RG40XX-V V2 uses `rg40xx-v-v2` where the repo expects it; never silently substitute another board profile. Empty/invalid DEVICE must fail safely, never boot a random DTB.

## Storage (§9.3)
SYSTEM (boot/root), STATE, USERDATA; SYSTEM/STATE never exposed as user storage. A non-nuubOS-owned TF2 is never formatted automatically; formatting needs explicit user confirmation; migration/backup transactional and verifiable.

## Display (§22)
Initial brightness 60%; user brightness persists across reboot. Display state stays coherent across HDMI/internal panel changes: unrelated setting changes must not resurrect stale internal-panel brightness presentation while the panel is inactive.

## RGB (§27), battery/status (§28)
RGB depends on hardware capability: no controls for unsupported devices (rows show "Unavailable"); integrate with lifecycle/power. Battery/status is event-driven: AXP717 uses gauge events (`GAUGE_NEW_SOC`), not UI polling; keep that unless the driver/API requires otherwise.

## Performance / power principles (§31)
H700 is constrained: optimize intentionally, not prematurely; keep proven hardware decisions unless new measurements justify change. Profiles Auto / Performance / Battery Saver are policy of the system service, never UI pages. Suspend/power changes need regression testing (Wi-Fi, Bluetooth, input wake, display, storage, audio interact). Never invent wake behavior beyond qualified hardware capability.

## Licensing history (§10)
Panel firmware blobs sourced from ROCKNIX had insufficient redistribution provenance: local bring-up only until resolved (check current repo state).

## Kernel, display and suspend notes (§37.4)
- Kernel-stage limitation: with HDMI connected at boot the DRM fbdev is created at the mode common to panel and HDMI (640x480), so the kernel boot/poweroff splash appears small on HDMI. Fixing it needs a KMS-based splash path (platform work).
- Boot-with-HDMI → unplug → black panel (`sun50i-iommu ... Page fault (master 0, dir rd)` on each CRTC switch): root cause is `commit_tail_rpm` (ACTIVE_ONLY) skipping planes disabled together with their CRTC, so sun8i layers kept a freed/unmapped IOVA enabled and the DE faulted on the next CRTC enable. Fix: kernel patch `0026-drm-sun4i-disable-layers-with-their-crtc.patch` (2026-10-05).
- Boot-with-HDMI → panel black for the whole boot (backlight on, every SoC register identical to a good boot; even the TCON colour-bar test pattern stayed black): TCON LCD0 and TCON TV0 share `pll-video0`; all CRTCs are mode set before any is enabled and the TCON took rate exclusivity only at enable, so the HDMI mode set retuned the PLL and the panel was initialised at 99 MHz instead of 27 MHz. Fix: `0027-drm-sun4i-tcon-lock-dclk-rate-at-mode-set.patch` (rate exclusive from mode set; HDMI then moves to `pll-video1`). Hardware-confirmed 2026-10-05. Boot-without-HDMI → plug → unplug always worked because the panel already held the PLL.
- Sleep → spontaneous wake after ~1 min (seen as "Wi-Fi lost" toast + screen back on): the AXP717 IRQ line is wake-armed (power key shares it), so the `GAUGE_NEW_SOC` IRQ added by patch 0100 ended s2idle on every 1% SOC step. Fix: `0101-power-supply-axp717-mask-soc-irq-during-suspend.patch` (SOC IRQ disabled in suspend, one `power_supply_changed` on resume). To find a wake source: diff `/proc/interrupts` across the sleep and read `/sys/class/wakeup/*/last_change_ms`. Wi-Fi deauth during suspend (`DEAUTH_LEAVING`) is normal: rtw88 has no WoWLAN.
- Short black flash on every composition→direct-scan-out switch: DE33 mixer commit writes no `SUN8I_MIXER_GLOBAL_DBUFF`, so layer attributes and address do not latch together; fixed for the bottom layer by `0028-drm-sun4i-ignore-pixel-alpha-on-bottom-layer.patch` (2026-10-06, see §37.12). Any other per-frame attribute change on upper layers can still tear for one frame.
- Display clock check: `/sys/kernel/debug/clk/clk_summary` (pll-video0/tcon-lcd0/tcon-data-clock must read 81/81/27 MHz on the RG40XX-V panel).
- Display register debugging: `/sys/kernel/debug/regmap/{6511000,6515000}.lcd-controller/registers` and `*.mixer-top` are safe to read. NEVER read `1100000.planes-planes` or `*.mixer-mixer` regmaps: the read segfaults or hangs the whole SoC (target unreachable, needs a power cycle). Leave `drm.debug` at 0 on target; a high level during a modeset also locked it up. Single registers can be read with `devmem` (DE33: mixer0 blender 0x1281000, mixer top 0x1008100/0x1008140, channel N at 0x1101000 + N*0x20000, channel map 0x1008024..; TCON0_CTL 0x6511040 — writing source-select 1 shows colour bars). The kernel has no ftrace/kprobes: for call tracing add temporary `dev_info` and remove it after.
