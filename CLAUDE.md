# nuubOS — Claude Code Project Handoff

> Last handoff update: 2026-10-05
> Project: nuubOS
> Primary repository: `~/Projects/nuubOS`
> Primary branch: `main`
> Remote: `git@github.com:baf93/nuubOS.git`

## 1. Mission

nuubOS is a lightweight Linux gaming operating system for Allwinner H700 handhelds. The product goal is a polished, controller-first console experience with a native UI, standard Linux foundations, strong hardware integration, low overhead, predictable behavior, and a clean Product API boundary between UI and system functionality.

The current reference/qualification hardware includes:

- Anbernic RG35XX Pro — original/reference target.
- Anbernic RG40XX-V / RG40XX-V V2 — primary active development/qualification device.
- Anbernic RG CubeXX — available for multi-device qualification.

The platform uses a universal H700 image with explicit device selection. Do not assume one board only.

## 2. Sources of truth and precedence

When sources disagree, use this precedence:

1. Current repository code and tests, after understanding why they differ.
2. Product Epics / frozen product requirements.
3. This `CLAUDE.md`.
4. Architecture/design documentation in the repository.
5. Historical implementation notes.

The Product Epics live in `docs/product/nuubOS-product-epics.md` (EPIC-001..055, ENABLER-001..009, roadmap P0–P10). They are the source of truth for the final product feature set. Do not remove a capability during optimization/slimdown until it has been checked against Product Epics and Technical Enablers.

Never blindly preserve legacy implementation merely because it exists. Conversely, never replace working architecture speculatively. Identify whether code is current architecture, transitional code, or parked legacy code before changing it.

## 3. Current architectural direction — IMPORTANT

The previous nuubUI implementation is legacy/fallback and should remain recoverable in its dedicated branch/history.

The production frontend direction is:

- Rust.
- Slint.
- Wayland/compositor path where appropriate and qualified.
- Hardware accelerated rendering / direct DRM/KMS where practical.
- Event-driven architecture.
- No UI-owned system business logic.
- No routine polling when state can be event-driven.
- Small functional local services rather than a monolithic daemon.

The new frontend starts conceptually from the Platform FINAL baseline rather than carrying forward the old product/frontend implementation wholesale.

### 3.1 Product services

System/product services are the source of truth.

Each capability should expose a typed contract consisting conceptually of:

- commands/actions;
- state queries / snapshots;
- typed change events.

The UI:

1. obtains initial state;
2. subscribes to service events;
3. updates its local model/cache;
4. sends commands to services;
5. never reimplements the service's policy.

Avoid:

- UI polling loops;
- UI calling `iw`, `wpa_cli`, ALSA internals, sysfs, etc. directly for product behavior;
- generic untyped event buses when a capability-specific typed contract is appropriate;
- one giant service owning unrelated capabilities;
- duplicated business logic between Settings, Quick Menu, OOB and Home.

OOB, Settings, Home, Quick Menu and other clients MUST reuse the same Product Services and contracts.

## 4. Development philosophy

Before editing:

1. Inspect the real implementation.
2. Identify the owning component/service.
3. Find existing contracts, tests and adjacent patterns.
4. Determine whether the issue blocks the current milestone or belongs to a future milestone.
5. Make the smallest architectural change that solves the real problem.

Do not perform speculative rewrites.

Do not polish temporary workarounds that will be replaced by an already-planned later milestone.

Fix immediately when required by current exit criteria or when failing to do so would freeze real architectural debt.

Otherwise document/defer cleanly.

Prefer root-cause fixes over sleeps, retries, rebind loops or timing hacks. Sleep/retry/rebind must not become production fixes unless there is a proven hardware/protocol requirement and the design explicitly justifies it.

## 5. Build rules — NON-NEGOTIABLE

The official build is Dockerized and reproducible.

- Buildroot/toolchain dependencies belong in the versioned nuubOS development container.
- Host distributions such as CachyOS/Ubuntu must be interchangeable.
- The target build entry point remains `./scripts/compile.sh` inside the containerized environment.
- NEVER invoke Buildroot `make` directly as the production/project workflow.
- Prefer the smallest targeted rebuild possible.
- Do NOT run a clean/full build merely for confidence.

A full/clean rebuild is justified only when changes can leave stale integration artifacts, e.g.:

- rootfs composition/removal;
- global toolchain/config changes;
- Buildroot/package integration requiring it;
- final integration qualification.

The Dockerized Platform v0.5 build has previously completed successfully and generated rootfs, boot/state images and the universal H700 image. Linux 7.2.8 also completed a full Dockerized build successfully and the H700 patchset remained applicable.

After builds, Buildroot should return to its intended pristine state according to project tooling.

## 6. Shell / command rules

The user's workstation shell is fish.

Commands intended for direct interactive paste MUST be fish-compatible.

Do NOT use Bash heredocs (`<<EOF`) in interactive instructions.

Do NOT use:

- `set -e`
- `set -u`
- `set -o pipefail`
- `set -euo pipefail`

Do NOT use `exit` or `return` in terminal paste blocks to handle errors; this can close the user's interactive shell. Prefer explicit checks, `[FAIL]` output, `&&`, or conditionals that leave the shell open.

When asking the user to run a command, print the COMPLETE copy/pasteable command including `cd`, paths and options.

Default download directory on Thinkerer/CachyOS is:

`/home/baf/Scaricati`

not `/home/baf/Downloads`.

## 7. Git discipline

- Inspect `git status` and diff before and after work.
- Stage selectively.
- Do not include unrelated files.
- Do not commit automatically unless explicitly requested.
- Do not push automatically unless explicitly requested.
- Keep the working tree clean at milestone boundaries.
- After finalization/commit, remove temporary bundles, extracted work dirs and generated artifacts that are not intended to remain.

Do not silently discard user changes.

## 8. Bundle/collector workflow

Historically ChatGPT used a collector/TAR workflow because it could not access the local repository directly. Claude Code normally works directly in the repository, so do NOT create TAR round-trips unnecessarily.

However, preserve the underlying discipline:

- inspect complete relevant source context;
- make changes against actual repository state;
- produce reviewable diffs;
- verify locally;
- then ask for hardware qualification when required.

If a collector/bundle is ever genuinely needed, its `.sh` must clean up temporary files/directories/payloads it created while preserving the output TAR and never deleting unrelated repository/user files.

## 9. Platform baseline

### 9.1 Kernel / hardware

Current qualified direction includes Linux mainline 7.2.x; Linux 7.2.8 has passed the Dockerized build and RG40XX-V quick hardware regression.

A recorded RG40XX-V quick regression on 7.2.8 produced:

- 143 PASS
- 0 WARN
- 0 FAIL
- 9 SKIP

Qualified areas included foundation, DRM/Panfrost/display/backlight, analog audio baseline, Cedrus/VPU, RTL8821CS Wi-Fi 5 GHz, Bluetooth, input/rumble capabilities, storage/STATE/USERDATA/USB and RGB. Treat the repository/current test records as authoritative for newer results.

### 9.2 Universal H700 image

One H700 image is used with explicit device selection via `NUUBOS_BOOT/nuubos.conf` and `DEVICE=`.

Supported device selector values historically include:

- rg-sp
- rg28xx
- rg34xx
- rg34xx-sp
- rg34xx-sp-v2
- rg35xx-2024
- rg35xx-2024-v6
- rg35xx-h
- rg35xx-h-v6
- rg35xx-plus
- rg35xx-plus-v6
- rg35xx-pro
- rg35xx-sp
- rg35xx-sp-v2
- rg40xx-h
- rg40xx-h-v2
- rg40xx-v
- rg40xx-v-v2
- rgcubexx

For RG40XX-V V2 qualification, use the correct `rg40xx-v-v2` selector where the current repository expects it. Never silently substitute another board profile.

The boot selector must fail safely for empty/invalid DEVICE rather than booting a random DTB.

### 9.3 Storage

System concepts include SYSTEM/boot/root, STATE and USERDATA. Do not expose SYSTEM/STATE as ordinary user storage in the product UI.

TF2 policy must avoid destructive surprises. A non-nuubOS-owned TF2 must not be formatted automatically. Formatting requires explicit user confirmation. Migration/backup workflows must be transactional/verifiable where applicable.

## 10. Licensing and redistribution

Licensing is a hard project requirement.

Before including or publishing any redistributed component, verify licensing/provenance for:

- software;
- kernel patches;
- drivers;
- assets;
- fonts;
- themes;
- emulators;
- libraries;
- firmware;
- avatar/image packs;
- other redistributed material.

Track required attribution and notices.

Do not ship firmware/assets merely because they work locally if redistribution provenance is unclear.

Historical example: panel firmware blobs sourced from ROCKNIX had insufficient redistribution provenance and were local-bring-up-only until resolved. Check current repository state before assuming this remains unresolved.

Default user avatars must come from an existing reusable external set whose license and attribution have been verified; do not generate bespoke avatar art merely to fill the feature.

## 11. UI visual direction

Primary visual reference is the nuubOS “Small System. Big Adventures.” mockup/visual language:

- dark/matte console UI;
- hero artwork on Home;
- minimal top bar;
- Recently Played with cover art and blue focus glow;
- selected-game metadata;
- Systems & Collections;
- Applications;
- controller hints at bottom;
- clean console-like presentation.

Do not treat this as a fixed-resolution screenshot to scale.

### 11.1 Responsive layout

UI must be natively adaptive, at minimum for:

- 4:3 / 640×480;
- square / 720×720;
- 16:9 and widescreen/TV/4K.

Use proportional/native layout and breakpoints/variants where needed. Do not uniformly scale one master canvas.

The RG40XX-V splash is specifically designed/qualified for native 640×480; avoid oversized typography. Preserve the nuubOS blue accent where appropriate.

### 11.2 Rendering efficiency

When nothing visually/state-wise changes:

- do not continuously redraw;
- do not generate frames unnecessarily;
- do not poll just to refresh unchanged state;
- do not run animations that have no active visual purpose.

Event-driven updates are preferred.

### 11.3 Controller-first

Everything must remain usable without touch/mouse.

Controller hints must be controller-agnostic. Do not hardcode Xbox/Nintendo A/B/X/Y labels as universal semantics.

For face-button hints, use the established logical-action representation (four dots with the relevant position highlighted/filled) derived from the active mapping.

### 11.4 Global marquee behavior

Visible strings that do not fit must not simply be permanently truncated when the text is relevant/active.

Use a reusable global marquee behavior across Home, Settings, dropdowns, User Picker, Quick Menu, OSD, notifications and future screens.

- Text that fits remains static.
- Relevant/active overflowing text scrolls horizontally so the full value can be read.
- Avoid continuous redraw when marquee is unnecessary.

## 12. Localization

nuubOS UI is natively multilingual.

- UI language is a per-user preference managed by General/Localization.
- All user-visible strings must go through localization.
- Do not add hardcoded visible UI strings.
- Every newly added user-visible string must be translated for all currently supported languages.

## 13. Settings architecture

Settings uses a consistent visual grammar across every section.

There is NO separate fake “preview” implementation for category selection.

When a Settings category is highlighted, the right pane renders the REAL page using the same state/model/layout/values as when entered. Entering changes focus/interactivity only, not the displayed content.

Do not duplicate preview/page logic.

There is no generic top-level “Gaming” Settings category. RetroArch remains the primary emulation backend and emulator-specific configuration should not be duplicated globally in Settings.

## 14. General / users

Settings → General is divided conceptually into User and Device areas and should not waste space on a generic category description.

User capabilities include real user management such as:

- username/profile information;
- profile picture;
- default licensed avatar collection;
- user-supplied profile pictures from an appropriate per-user location;
- management of other users;
- user creation;
- deletion with confirmation;
- startup user-selection behavior;
- default boot user where appropriate.

Switch User must perform a REAL user/session switch, not merely change a visual selection.

The User Picker must follow nuubOS visual grammar with coherent top bar and bottom controller-hint bar.

When two or more users exist, product settings determine whether to show user selection at boot or load a configured default user.

Device-side General includes concepts such as:

- date/time;
- timezone;
- automatic time synchronization;
- keyboard layout;
- startup behavior;
- Manage Users.

## 15. OOB / first boot — CURRENT NEAR-TERM WORK

The OOB/first-boot experience is deliberately minimal and controller-first.

Reference flow (EPIC-002, confirmed by the user 2026-10-05; there is NO Language step — language stays a per-user Settings preference):

1. Welcome
2. Date & Time (timezone, automatic time, manual date/time)
3. Wi-Fi (skippable)
4. Users (create one or more users: username + avatar; Startup/Boot User when more than one)
5. Ready
6. Home (or the user picker when the login mode requires it)

Requirements:

- Reuse the exact same Product Services/contracts as Settings.
- No parallel OOB-only business logic/configuration stack.
- Network is skippable.
- Date & Time reuses General timezone/automatic-time capabilities.
- OOB appears only when initial setup is incomplete.
- Completion persists a setup-complete state.
- Visual grammar must be nuubOS-consistent.
- Localization applies from the language step onward as appropriate.
- Controller-first throughout.

At this handoff, OOB design/implementation is one of the immediate work areas after/alongside final Settings polish, followed by Home and emulation integration.

## 16. Home

Home should remain close to the established visual reference while using native adaptive layouts.

Key product concepts include:

- top status bar;
- hero area;
- Recently Played;
- game metadata;
- Systems & Collections;
- Applications;
- controller hint bar.

Home Music:

- plays MP3 files from the active user's `music` directory;
- playback order is random/shuffled;
- has its own persistent volume setting;
- when Home Music is active, Quick Menu exposes a Skip action for the current track.

Do not duplicate audio routing/mixing logic in Home.

## 17. Quick Menu

The Quick Menu is a system-wide contextual overlay opened with the `M` button.

It must be capable of appearing above Home, Settings, applications and games/fullscreen clients.

Wayland/compositor architecture must therefore support:

- overlay/layer rendering over fullscreen clients;
- OSD/notifications over fullscreen clients;
- correct focus/input routing;
- compositing without moving product business logic into the compositor.

Quick Menu is context-sensitive and should expose the most useful actions for the current context.

### 17.1 Audio control in Quick Menu

Quick Menu should expose ONE primary `Audio` control, not separate cluttered controls for Audio Output, Master Volume, System Sounds, Home Music, etc.

Expected behavior:

- left/right changes master volume of the currently selected physical audio output by 1%;
- confirm opens a dropdown of currently available outputs;
- Automatic must be available where supported by the current Product Audio contract;
- changing output must not unnecessarily close Quick Menu;
- stream routing follows Product Audio ownership/policy.

Dedicated System Sounds/Home Music/Application volumes remain in Settings.

### 17.2 Performance

Performance Profile must also be accessible from Quick Menu using the same System/Product service as Settings.

### 17.3 User switching

Switch User is available contextually from Quick Menu when appropriate (e.g. Home/Settings) and must invoke the real user/session mechanism.

## 18. Emulation architecture — CURRENT FROZEN DIRECTION

Use a HYBRID model.

RetroArch remains the primary emulation backend.

nuubOS exposes the common, recurring emulation actions through native controller-first UI/Product APIs.

A separate action named conceptually `RetroArch Advanced` opens RetroArch's native menu for power users and advanced configuration.

Do NOT reimplement the whole RetroArch configuration UI in nuubOS.

Standalone emulators are exceptions, not the default. Introduce one only when H700 qualification/benchmarking demonstrates a concrete advantage over the equivalent RetroArch/libretro solution.

### 18.1 In-game Quick Menu ordering

When a game is running, the GAME section belongs at the TOP of Quick Menu, before system controls.

### 18.2 Hotkeys

Frequent emulation actions must also have controller hotkeys/chords, not only menu entries.

At minimum support:

- Save State;
- Load State;
- change Save Slot;
- Fast Forward.

Use `M` as the modifier layer while preserving `M` alone to open Quick Menu.

Hotkey actions must pass through the SAME Emulation Service/Product API used by menu actions.

Show a global OSD/notification confirming action/state.

Do not create a second direct RetroArch-control path in the input layer.

## 19. Global notifications / OSD

The system architecture must support global notifications/OSD above applications and fullscreen games.

This will be used for concepts including:

- volume changes;
- emulation hotkey feedback;
- save/load state;
- save-slot changes;
- fast-forward state;
- other system status feedback.

Keep product semantics in services/UI layers, not compositor-specific business logic.

## 20. Lifecycle / power

Sleep, Restart and Power Off MUST use one central lifecycle/power path regardless of caller.

Callers include:

- Power Menu;
- Quick Menu;
- Home;
- automatic idle timeout;
- future clients.

The central service owns orchestration such as:

- game save-state/pre-power hooks;
- RGB/display handling;
- other pre-power hooks;
- RTC wake handling where applicable;
- final system action.

The UI only requests the action.

### 20.1 Save state before power actions

When a game is active, automatic game state saving before Sleep/Restart/Power Off belongs in the central lifecycle/power flow, not in individual UI callers.

### 20.2 Confirmation UX

For Restart/Power Off confirmation states, preserve the clean centered action visual and show `Press again to confirm` laterally rather than placing it underneath the title, according to the established UI direction.

### 20.3 Sleep/wake regression

A historical blocker where Sleep → START/POWER could leak into Settings/input behavior was fixed/qualified. Treat regressions in wake input suppression/routing as blockers, not cosmetic issues.

## 21. System Settings

Settings → System product direction includes:

### Performance Profile

- Auto
- Performance
- Battery Saver

Automatic Battery Saver uses a device-global battery threshold.

### Power timeouts

- Sleep After: configurable or Off.
- Power Off After: configurable or Off.

Final semantics involving sleep-before-poweroff and RTC/wakeup must follow actual qualified hardware capability; do not invent wake behavior.

### Storage

- SD1/SD2 overview;
- guided migration/backup between supported storage;
- do not expose SYSTEM/STATE as normal partitions.

### System Information

Show useful reliable live information including:

- battery / charging;
- CPU clock;
- GPU clock;
- temperatures;
- uptime;
- RAM used/total/free;
- build information;
- device information;
- network information;
- storage information.

Prefer event-driven updates where possible and sane low-frequency mechanisms only where hardware data inherently requires sampling.

### Restore Defaults

Do not add a generic Updates & Recovery category merely because it sounds conventional.

Provide Restore Defaults / Reset System Settings with strong confirmation.

Reset MUST NOT erase ROMs, BIOS, saves or USERDATA.

Before final infrastructure freeze, audit ALL sane defaults and reset behavior. Initial/default/runtime/reset values must agree; eliminate legacy divergent defaults.

Controller mapping customization must reset to the approved default mapping as part of Reset System Settings.

## 22. Display

Initial display brightness default is 60%.

User-selected brightness persists across reboot.

Display state must remain coherent when HDMI/internal panel state changes. Do not let unrelated setting changes resurrect stale internal-display brightness presentation when the internal panel is inactive.

## 23. Product Audio — TARGET ARCHITECTURE

Final audio direction is standard Linux audio based on:

- PipeWire;
- minimal/headless WirePlumber.

`nuubos-audio-service` remains the sole Product API/policy owner.

The standard graph should ultimately unify:

- Speaker;
- Headphones;
- HDMI;
- Bluetooth;
- System Sounds;
- Home Music;
- Test Sound;
- application streams.

ALSA `default` and native PipeWire clients should be able to use the stack without needing nuubOS-specific knowledge.

Remove the custom `dmix` production path in the final architecture.

Migration may be capability-by-capability. BlueALSA may temporarily remain only to avoid breaking already-qualified Bluetooth while migrating toward native BlueZ monitoring through WirePlumber.

Do not prematurely delete a working transitional backend until the replacement is qualified.

### 23.1 Performance qualification

PipeWire/WirePlumber must be qualified on H700 rather than assumed acceptable.

Compare against the existing/previous ALSA/BlueALSA path for:

- idle CPU/wakeups;
- RAM;
- playback CPU;
- xrun/underrun;
- latency/responsiveness;
- Bluetooth;
- suspend/resume;
- stability.

If overhead is disproportionate, evaluate a smaller nuubOS proxy/router before freezing architecture.

### 23.2 Routing and priority

Automatic output priority direction:

1. Bluetooth audio
2. Headphone jack
3. HDMI audio
4. Internal speaker

Physical hardware constraints for speaker/jack routing must be respected rather than pretending software controls hardware that it cannot.

HDMI media volume is controlled by TV/receiver; do not present a fake adjustable physical HDMI master if hardware/path does not support it.

### 23.3 Stream behavior

All Product Audio streams, including Home Music and System Sounds, must mix correctly in the same graph, including Bluetooth. Short cues must not be truncated.

Product-owned streams such as Test Sound, System Sounds and Home Music must follow an output change while playing rather than remaining stuck on the previous output.

### 23.4 Settings volumes

Keep distinct settings for:

- Applications: 0–100 global gain for application streams;
- System Sounds: 0–100 common system-cue volume;
- Home Music: 0–100;
- physical-output master volume where meaningful.

System Sounds default: 60%.

0% means hard mute.

System Sounds toggles:

- Navigation Sounds — controls Navigation, Select, Back and Quick Menu/Settings cues.
- Power Sounds — controls Boot, Restart and Power Off cues.

There is no separate Startup Sound volume. Power cues use System Sounds volume and are simply not scheduled when Power Sounds is Off.

## 24. Wi-Fi

Wi-Fi UX should feel smartphone-like while remaining controller-first.

Home/top-bar Wi-Fi status must distinguish at least:

- off;
- on/not connected;
- connecting;
- connected.

State/SSID updates should be live/event-driven.

Product capabilities include:

- scan;
- connect;
- disconnect;
- saved networks;
- forget;
- hidden SSID;
- autoreconnect;
- per-network IP configuration.

Per-network IPv4 modes:

- Automatic/DHCP;
- Manual/static.

Manual supports address/prefix or netmask, gateway and DNS. Gateway may legitimately be empty for local networks with no default route. Never invent a gateway.

Automatic/DHCP view should expose the runtime-assigned configuration read-only, including address, derived subnet mask/prefix, gateway, DNS and other useful runtime values.

The UI must not expose kernel/driver/wpa_supplicant/udhcpc implementation detail in normal product UX.

Wi-Fi password entry uses a complete classic QWERTY keyboard as a full-width bottom sheet, controller-first, with physical-keyboard support and modes/pages for normal QWERTY characters.

## 25. Bluetooth

Bluetooth is a product capability and should integrate with Product Audio rather than remain a special UI-side path.

Historically Beats Studio 3 A2DP was qualified via BlueALSA using 44.1 kHz compatibility and a fixed-format ALSA PCM conversion path. Treat this as transitional qualification context; inspect current code before changing it.

When Bluetooth headphones are connected, top-bar status should expose an appropriate headphones indication according to current UI design.

## 26. Input / controller

Input is controller-first and should use logical actions rather than hardcoded controller-brand semantics.

Roadmap/product requirements include:

- Controller Mapping;
- Input Tester;
- RGB Lighting controls where supported.

These must be planned/implemented before the relevant final product milestone closes.

Controller mapping reset belongs in Reset System Settings.

## 27. RGB

RGB behavior is hardware-capability-dependent. Do not expose unsupported controls on devices that lack them.

Keep RGB handling integrated with lifecycle/power behavior where appropriate.

## 28. Battery/status

Prefer event-driven battery/status handling. AXP717 battery state has previously been implemented using gauge events (`GAUGE_NEW_SOC`) rather than UI polling. Preserve the event-driven principle unless the current driver/API requires otherwise.

## 29. SSH / deployment

A previously observed slow SSH connection on Linux 7.2.8 was traced to reverse DNS, not a kernel regression. Adding the development host mapping on target reduced connection time dramatically. Do not reopen this as a kernel performance bug without new evidence.

The target should include an SFTP server so modern SCP/SFTP deployment works natively rather than requiring stdin-over-SSH workarounds. Check current repo status before implementing again.

## 30. Testing and hardware qualification

A successful compile is NOT equivalent to hardware qualification.

Use the smallest useful verification sequence:

1. static/source checks;
2. targeted component build;
3. relevant automated tests;
4. targeted deploy;
5. hardware/runtime qualification where behavior depends on real hardware;
6. broader regression only when justified.

Do not repeatedly flash full images for changes that can be deployed/tested safely as individual components.

When a hardware/manual step is required in a script/test, print it clearly in actual orange terminal text (ANSI 256-color orange, e.g. `\033[38;5;208m`) and include `[ACTION]`/`[Action]` in that line.

Automatic operations must NOT be labeled `[ACTION]`; use ordinary status or `[PASS]`/`[FAIL]`.

Never declare a capability FINAL based only on build success when hardware qualification is part of its exit criteria.

## 31. Performance / power principles

H700 is resource constrained. Optimize intentionally, not prematurely.

Preserve proven hardware decisions unless new measurements justify change.

Power/performance modes include Auto / Performance / Battery Saver. Policy belongs in Product/System services, not individual UI pages.

Suspend/power changes require regression testing because Wi-Fi, Bluetooth, input wake, display, storage and audio interact with suspend/resume.

## 32. Current product sequence / handoff state

As of 2026-10-05, the intended near-term sequence is:

1. Finish Settings polish/uniformity and remaining correctness issues.
2. Implement/finish minimal OOB using existing Product Services.
3. Return to/finish Home against the established visual reference.
4. Implement emulation integration around RetroArch using the hybrid model.
5. Add/finish global notifications/OSD and in-game Quick Menu/hotkey integration as required by that flow.

Do not interpret this as permission to skip blockers discovered in the current step.

The most recent frozen emulation decision is the hybrid RetroArch model described above.

The most recent frozen OOB flow is Welcome → Date & Time → Wi-Fi [Skip] → Users → Ready → Home (EPIC-002). The OOB is implemented (2026-10-05); see §37.10.

## 33. How Claude Code should work on every task

For each task:

### A. Inspect

- Read this file.
- Read the relevant Product Epic/requirements.
- Inspect `git status`.
- Locate the owning service/component and relevant tests.
- Read enough adjacent code to understand established patterns.

### B. State the intended change

Before broad edits, summarize internally/for the user:

- root cause or requested behavior;
- owning layer;
- files/components expected to change;
- verification plan.

Ask only when a real product/architecture ambiguity cannot be resolved from repository/docs. Do not ask questions merely to avoid inspecting the code.

### C. Implement narrowly

- Preserve service boundaries.
- Reuse contracts/components.
- Avoid duplicate state/business logic.
- Localize every visible string.
- Preserve responsive/controller-first behavior.
- Avoid polling/redraw regressions.

### D. Verify economically

Use the smallest build/test that proves the change first. Escalate to full integration only when technically necessary.

### E. Review

Before handing back:

- inspect diff;
- inspect git status;
- remove debug leftovers/temp files;
- check localization;
- check license implications for new dependencies/assets;
- check responsive layouts affected;
- check service ownership/event flow;
- identify whether hardware testing is still required.

### F. Do not commit unless asked

Leave a concise summary of:

- what changed;
- tests/builds run and results;
- remaining hardware/manual qualification;
- any deferred issue and why it is deferred.

## 34. Things Claude must NOT do

Do NOT:

- run direct Buildroot `make` as the project build workflow;
- perform full builds by default;
- commit/push without explicit instruction;
- discard unrelated local changes;
- add UI polling because it is easy;
- put system business logic in Slint/UI/compositor;
- duplicate Product Service behavior in OOB/Settings/Quick Menu/Home;
- introduce a generic Gaming Settings section;
- recreate all RetroArch settings in nuubOS;
- add standalone emulators without measured justification;
- hardcode user-visible English strings;
- assume 640×480 is the only layout;
- assume Xbox/Nintendo button labels;
- ship assets/firmware with unclear redistribution rights;
- auto-format unknown TF2 media;
- erase ROM/BIOS/save/USERDATA during Reset System Settings;
- treat compilation alone as hardware qualification;
- add sleep/retry/rebind hacks instead of finding root cause;
- use Bash-only interactive command snippets for the user's fish shell;
- use `set -e`, `set -u`, `pipefail`, `exit` or `return` in user paste blocks;
- silently alter architectural decisions frozen in this document/Product Epics.

## 35. When architecture appears inconsistent

The repository has evolved through multiple frontend/audio/platform phases. If you encounter code that conflicts with this handoff:

1. Do not immediately rewrite it.
2. Determine whether it is legacy, transitional, or current.
3. Check branch/history/docs/Product Epics.
4. Explain the mismatch.
5. Make the minimum migration step appropriate to the current capability.

The goal is controlled evolution, not a flag-day rewrite.

## 36. Definition of a good nuubOS change

A good change:

- solves the requested product behavior;
- lives in the correct owning layer;
- uses typed Product APIs/events;
- remains controller-first;
- is localized;
- is responsive across supported aspect ratios;
- avoids unnecessary polling/rendering;
- has sane persistent defaults;
- respects reset semantics;
- has verified licensing for new redistributed material;
- builds with the official workflow;
- is tested at the smallest appropriate scope;
- is hardware-qualified when required;
- leaves a clean, understandable diff.

When in doubt, optimize for architectural consistency, measured behavior, maintainability, low H700 overhead and a polished console experience — not for the shortest patch.

## 37. Operational quick reference (keep this section current)

Update this section whenever you learn something that would otherwise have to be rediscovered.

### 37.1 Targeted build (official, containerized)

```sh
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/workspace/.docker-home \
  -e BR2_DL_DIR=/workspace/dl -e BR2_CCACHE_DIR=/workspace/.cache/nuubos/buildroot-ccache \
  -v "$PWD:/workspace" -w /workspace nuubos-dev:0.5 \
  ./scripts/compile.sh <pkg>-rebuild [<pkg>-rebuild ...]
```

`compile.sh` forwards its arguments to Buildroot `make` and restores Buildroot pristine on exit. Gotcha: when several `-rebuild` targets go in one invocation, a package that is also a dependency of an earlier one (e.g. `nuubos-input`) is already "made" before its own clean runs and is silently NOT rebuilt — check for its `>>> <pkg> Building` line, or rebuild it alone. Packages touched most often: `nuubui-home`, `nuubos-quick-menu`, `nuubos-localization`, `nuubos-audio`, `nuubos-system`. Results land in `output/h700/target/`. A Rust UI rebuild takes ~40 s.

### 37.2 Development target and deploy (ALWAYS deploy after a change when no flash is needed)

- Target: RG40XX-V at `root@192.168.5.36`, local SSH key, `scp`/SFTP available.
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
- Kernel: `/boot/Image` lives on the rootfs (mmcblk0p1, rw). Kernel-only change: add the patch under `patches/linux/h700/`, apply it to `output/h700/build/linux-7.2.8` (already-patched tree), `compile.sh linux-rebuild`, scp `output/h700/images/Image` to `/boot/Image` (keep a backup), reboot.
- Built-in avatars: 24 DiceBear PNGs (01-08 Big Smile, 09-16 Fun Emoji = CC BY 4.0; 17-24 Open Peeps = CC0) regenerated by `tools/avatars/generate.sh`; attribution in `nuubos-users/src/avatars/README.txt` + `LICENSE-DiceBear.txt`.

### 37.3 UI architecture notes

- Settings live inside `package/nuubos/nuubui-home/src/ui/home.slint` (no separate package). Navigation/state in `src/main.rs`; `settings-view` integers are documented in home.slint.
- General has four sections (PROFILE/USER, USERS, DATE & TIME, KEYBOARD/DEVICE) placed with `general-row-y`/`general-row-visible`; every page shows section label + USER/DEVICE chip.
- Settings visual grammar: components `SettingsCategory`, `SettingsSectionHeader` (label + localized USER/DEVICE chip), `SettingsSelectableRow` (value | toggle | chevron | avatar preview, optional detail line), `SettingsActionButton`, `SettingsHint`. All rows are placed with `srow-y(k, scroll, split)` and shown with `srow-visible(...)`; metrics (`settings-row-height`, `settings-row-step`, `settings-content-x/width`, visible-row counts) are derived from the logical window size. Never hard-code row y positions.
- Scroll offsets are computed in Rust with `guarded_scroll_offset(index, count, visible, current)`; row counts there must match the Slint page.
- Slint 1.18 codegen bug: very large array-of-struct literals in `for` with many conditional fields can produce Rust "value moved" errors; split into explicit elements.
- `renderer-software` ignores `border-radius` when clipping (rectangular clip only). Round shapes made of images must be masked in Rust: avatars go through `round_avatar()` in nuubui-home `main.rs` (centre square crop, downscale ≤384 px, anti-aliased circle alpha) and are placed inside the ring inset by `border-width`.
- Headless snapshots: `tools/ui-shot/run.sh [en it ...]` renders Settings and Quick Menu scenes at 640x480/720x720/1280x720/1920x1080 into `output/ui-shot/<lang>/`. Use it to check layout before deploying.
- Quick Menu (`package/nuubos/nuubos-quick-menu`) is a separate layer-shell client with its own Slint platform. Default: GPU (`src/gpu.rs`, `GpuWindow` = WindowAdapter + `FemtoVGRenderer`, one GLES context per window via glutin EGL on the libwayland `client_system` backend, `wl_egl_window`, swap interval 0, EGL surface detached before the wl_surface is destroyed). Fallback (no EGL or `NUUBOS_UI_RENDERER=software`): software renderer into two persistent wl_shm buffers (BGRA premultiplied `TargetPixel`, `RepaintBufferType::SwappedBuffers`, only dirty rows written + `damage_buffer`). The volume OSD alone uses a small bottom-anchored surface (`create_overlay(qh, true)`). All QM geometry/scroll lives in quick_menu.slint (`QmRow`, `QmSliderRow`, `QmSectionHeader`); Rust must not duplicate pixel positions.
- QM also draws the restart/poweroff curtain over fullscreen clients; Home draws the boot curtain.
- Controller mapping (owner `nuubos-controllersd`, D-Bus `org.nuubOS.Controllers1`): 17 button controls + `left_x/left_y/right_x/right_y`; each bound to one source `key:CODE`, `abs:CODE:+|-` (half axis; on a stick axis = normal/inverted), `abs:CODE:>|<` (full trigger travel from min/max) or `none`. Defaults are derived from the device's evdev capabilities (D-pad keys or hat, L2/R2 keys/Z-RZ/BRAKE-GAS, right stick RX/RY or Z/RZ); `/state/users/<u>/controllers/<hex-id>.conf` stores only differences (legacy `action=305` still read). Capture: `BeginRemap` → inputd sends `RAWBASE`/`RAWREADY` baselines (normalized -100..100) → stick vs trigger is told apart by rest position; 5 s timeout (`RemapCancelled`), any button cancels a stick capture; a source already in use is swapped (same kind) or unassigned (other kind). inputd only consumes the navigation actions + `left_x/left_y` (stick navigation) and parses the same format. Input Tester `InputEvent` carries the resolved logical control and value (buttons 0..100, sticks -100..100).

### 37.4 Localization mechanics

- Catalogs: `package/nuubos/nuubos-localization/src/i18n/<lang>.lang` (`NNN=text`, index based) + `keys.txt`; validate with `sh validate-i18n.sh .` in that directory. Languages: de en es fr it nl pt.
- The Slint fallback arrays (`i18n-strings` in home.slint and quick_menu.slint) MUST match `en.lang` index-for-index; new strings are appended at the end of all catalogs and both arrays.
- Never compare service state against a translated string (`i18n-strings[n]`); compare raw service values.

- Lifecycle splash: one procedural design rendered by three renderers that MUST stay identical: `nuubos-splash.c` (kernel fbdev), the labwc first-frame patch, and `src/splash.rs` (duplicated in nuubui-home and nuubos-quick-menu; glyph table generated from the C source). Home/QM render it at the exact physical surface size and draw it 1:1 (no PNG assets, no per-frame scaling); the curtain splits on the splash accent line (`splash::geometry`).
- `MarqueeText` (identical copy in home.slint and quick_menu.slint) scrolls at a constant ~60 px/s (16 ms/px, min 1250 ms); `passes` = 2 (there and back) by default, 1 in notifications. The QM restarts a notification's marquee by blanking the labels and reading `toast-marquee-offset` before setting new text, and extends the card deadline to `toast-scroll-time` + 1.5 s.
- Bottom bars use the `ActionHint` component (keycap + label) inside HorizontalLayouts (`horizontal-stretch: 0` on hints, one stretching spacer) so spacing follows real label widths.
- Kernel-stage limitation: with HDMI connected at boot the DRM fbdev is created at the mode common to panel and HDMI (640x480), so the kernel boot/poweroff splash appears small on HDMI. Fixing it needs a KMS-based splash path (platform work).
- Boot-with-HDMI → unplug → black panel (`sun50i-iommu ... Page fault (master 0, dir rd)` on each CRTC switch): root cause is `commit_tail_rpm` (ACTIVE_ONLY) skipping planes disabled together with their CRTC, so sun8i layers kept a freed/unmapped IOVA enabled and the DE faulted on the next CRTC enable. Fix: kernel patch `0026-drm-sun4i-disable-layers-with-their-crtc.patch` (2026-10-05).
- Boot-with-HDMI → panel black for the whole boot (backlight on, every SoC register identical to a good boot; even the TCON colour-bar test pattern stayed black): TCON LCD0 and TCON TV0 share `pll-video0`; all CRTCs are mode set before any is enabled and the TCON took rate exclusivity only at enable, so the HDMI mode set retuned the PLL and the panel was initialised at 99 MHz instead of 27 MHz. Fix: `0027-drm-sun4i-tcon-lock-dclk-rate-at-mode-set.patch` (rate exclusive from mode set; HDMI then moves to `pll-video1`). Hardware-confirmed 2026-10-05. Boot-without-HDMI → plug → unplug always worked because the panel already held the PLL.
- Sleep → spontaneous wake after ~1 min (seen as "Wi-Fi lost" toast + screen back on): the AXP717 IRQ line is wake-armed (power key shares it), so the `GAUGE_NEW_SOC` IRQ added by patch 0100 ended s2idle on every 1% SOC step. Fix: `0101-power-supply-axp717-mask-soc-irq-during-suspend.patch` (SOC IRQ disabled in suspend, one `power_supply_changed` on resume). To find a wake source: diff `/proc/interrupts` across the sleep and read `/sys/class/wakeup/*/last_change_ms`. Wi-Fi deauth during suspend (`DEAUTH_LEAVING`) is normal: rtw88 has no WoWLAN.
- Short black flash on every composition→direct-scan-out switch: DE33 mixer commit writes no `SUN8I_MIXER_GLOBAL_DBUFF`, so layer attributes and address do not latch together; fixed for the bottom layer by `0028-drm-sun4i-ignore-pixel-alpha-on-bottom-layer.patch` (2026-10-06, see §37.12). Any other per-frame attribute change on upper layers can still tear for one frame.
- Display clock check: `/sys/kernel/debug/clk/clk_summary` (pll-video0/tcon-lcd0/tcon-data-clock must read 81/81/27 MHz on the RG40XX-V panel).
- Display register debugging: `/sys/kernel/debug/regmap/{6511000,6515000}.lcd-controller/registers` and `*.mixer-top` are safe to read. NEVER read `1100000.planes-planes` or `*.mixer-mixer` regmaps: the read segfaults or hangs the whole SoC (target unreachable, needs a power cycle). Leave `drm.debug` at 0 on target; a high level during a modeset also locked it up. Single registers can be read with `devmem` (DE33: mixer0 blender 0x1281000, mixer top 0x1008100/0x1008140, channel N at 0x1101000 + N*0x20000, channel map 0x1008024..; TCON0_CTL 0x6511040 — writing source-select 1 shows colour bars). The kernel has no ftrace/kprobes: for call tracing add temporary `dev_info` and remove it after.

### 37.5 Audio mechanics

- `nuubos-audiod` owns policy; players are child processes (`nuubos-audio-wav-player`, `nuubos-audio-mp3-player`). System sounds use ONE persistent `nuubos-audio-wav-player … sfx-server -` (started on the first cue, commands `PLAY <vol> <path>` on its stdin pipe, a new cue interrupts the current one, restarted on reroute/mute): a player per cue made WirePlumber set up and link a new stream node every time (~60 ms WirePlumber CPU per navigation cue, ~20% of a core while navigating). Between cues the PCM is dropped/prepared = inactive stream, 0 wakeups. Never use `snd_pcm_drain` on the PipeWire ALSA plugin in a long-lived player (it keeps the stream and thread waking) and never wait for `snd_pcm_delay` to reach 0 (it includes graph latency): read the delay once, wait, `snd_pcm_drop`. Home Music volume changes are sent live over a stdin pipe to the running MP3 player (gain ramp per block); never restart the stream for a volume step.
- Master volume ticks are coalesced and applied via `wpctl` once the burst settles.
- The analog sink `nuubos_analog` is a static PipeWire adapter (`91-nuubos-analog-sink.conf`) with `session.suspend-timeout-seconds = 0`: an idle open PCM costs 0 wakeups; suspending it caused first-cue delay + pop.
- Default Home Music: package `nuubos-home-music` downloads 8 Kevin MacLeod (incompetech.com) tracks, CC BY 4.0, unmodified, sha256-pinned, into `/usr/share/nuubos/home-music` with ATTRIBUTION.txt. `audiod` uses them only when the active user's `music` folder has no MP3. Skip Track (`MUSIC NEXT`) is fire-and-forget from the Quick Menu (a slow switch must not close it).
- PipeWire env on target: `XDG_RUNTIME_DIR=/run/nuubos/pipewire` for `pw-cli`/`pw-dump`/`wpctl`.
- Route apply: `reconcile_selection()` runs `nuubos-pw-route` + master volume only when the selected route differs from `applied_route` (last successful apply). BlueZ reports a device connected before WirePlumber creates its sink, so audiod keeps a libpipewire registry listener (`sink_watch`, fd in the main poll, 0 idle wakeups) and retries the apply when a new `Audio/Sink` global appears. Never re-apply on unrelated Bluetooth events: it overwrote the headset's own volume.
- Bluetooth AVRCP absolute volume: headset buttons change `org.bluez.MediaTransport1.Volume` (0..127); audiod adopts it as the Bluetooth master (`round(v*100/127)`, persisted, no sink write) once the route is applied and no own write is pending, then pushes `volume origin=device output=… volume=…` to `SUBSCRIBE` clients on `audiod.sock` (max 4). The Quick Menu subscribes and shows the volume OSD. The Beats headset sends no AVRCP passthrough keys.
- HFP needs `CONFIG_BT_RFCOMM=m` (autoloaded as `bt-proto-3`); PipeWire's native backend then registers HFP AG/HF and reads headset battery from `AT+IPHONEACCEV`/HF indicators into `org.bluez.Battery1`. Beats Studio³ open HFP (`AT+BRSF=155`, no HF indicators, no `AT+XAPL`) but report battery only over Apple's proprietary AAP: no battery for them on Linux.
- Target has no `depmod`: after adding a module, run `output/h700/host/sbin/depmod -b output/h700/target 7.2.8` and deploy `modules.*` with the `.ko`.

### 37.6 Defaults and Reset System Settings

- Every setting default lives once, in its owning service: `systemd.c` (`DEFAULT_*`: profile Auto, Auto Battery Saver ≤20%, Screensaver Off, Sleep After 10 min, Power Off Off, backup WEEKLY), `audiod.c` (`DEFAULT_*`: output Auto, speaker 70, headphones 50, Bluetooth 50, System Sounds 60, Home Music 60, Applications 100, both cue toggles On), `displayctl.c` (`DEFAULT_BRIGHTNESS` 60), `controllersd.c` (`mapping_defaults`, deadzones 20), `/etc/nuubos/input-bindings.conf`. `S03nuubos-state`/`provision-state.sh` must not write these keys.
- `RESET SYSTEM SETTINGS` (systemd) resets its own config, then calls each owner: `nuubos-audioctl reset-defaults`, `nuubos-displayctl reset`, `nuubos-inputctl bind reset`, D-Bus `org.nuubOS.Controllers1.ResetMappings` (removes `/state/users/*/controllers/*.conf` for ALL users; player assignments kept), `nuubos-storagectl backup-policy WEEKLY`. Replies `ERR reset incomplete` if any step fails (details in `/run/nuubos/systemd.log`). Not reset: users/login mode, per-user language/rumble/RGB, Wi-Fi/Bluetooth pairings, timezone/time/keyboard, storage mode, USERDATA.

### 37.7 Notifications (EPIC-006)

- Router `nuubos-notifyd` (package `nuubos-notify`, `S40nuubos-notifyd`, socket `/run/nuubos/notifyd.sock`, log `/run/nuubos/notifyd.log`): validates, drops identical messages within 3 s, fans out to subscribers, replays running Live Notifications on `SUBSCRIBE`. No history.
- Producers include the staging header `<nuubos/notify.h>` (add `nuubos-notify` to the package DEPENDENCIES + `select BR2_PACKAGE_NUUBOS_NOTIFY`). Messages are typed events with raw percent-encoded values (`POST|UPDATE|DISMISS id=… event=… key=value`); the event list lives in `notify.h`. Producers never send display text.
- Owners: audiod `music.track` (ID3v2.2/2.3/2.4/ID3v1 title/artist, file name fallback; only on a newly chosen track); controllersd `controller.connected/disconnected` (non-built-in, diff on `/dev/input` rescans; HID battery from Device-scope `power_supply` uevents matched by sysfs parent; a late battery UPDATEs the connect toast for 15 s; Bluetooth controllers (address from the HID uniq) take the level from `org.bluez.Battery1` — Get on appearance + PropertiesChanged/InterfacesAdded — which wins over the kernel value, and the kernel HID battery of uhid (BLE/HOGP) devices is ignored because it is synthetic and reads 0% (8BitDo Ultimate 2C); `accessory.battery.low` ≤15%, re-armed >20%); bluetoothd `headphones.*` for Audio-kind devices + `Battery1` low battery for non-controllers; statusd `battery.low/critical/charging/full` (15/5%, re-arm 20%, once per charge session); systemd `battery.saver` (automatic switch only) and `storage.job` Live Notification; wifid `wifi.connected/lost` (lost = association gone without a user action, posted only if not reconnected to the same SSID within 15 s — `LOSS_GRACE_MS`; the window restarts after a resume, a reconnect inside it posts neither toast, so AP roaming and post-sleep reassociation are silent; any user Wi-Fi command cancels it).
- Renderer: Quick Menu process. One card at a time (info 4 s, warning/error 6 s), queue of 6, same id replaces in place. The card has its own persistent layer surface (`Notifier`: second Slint window `NotificationWindow`, second `WaylandState` on its own event queue, 416x88 anchored top-right, card 14 px from top/right) that opening/closing the Quick Menu never touches; the open menu draws an identical copy at the same position so neither stacking order nor remapping flashes. Only the menu `WaylandState` publishes the mapped markers. Event → title/icon/severity mapping is `toast_view()` in `main.rs`. Hidden during the lifecycle curtain and cleared before Sleep; notifications received before a resume are dropped. Frames are produced only while `has_active_animations()` (marquee). Unmapping a layer surface (null-buffer commit) must go through `WaylandState::commit_unmap()`: wlroots resets the surface and forgets configures already sent, so acking one during the following roundtrip is a fatal "wrong configure serial" that kills the whole QM connection (seen 2026-10-05 as a frozen QM/Home after a volume OSD over a notification).
- Test on target: `nuubos-notifyctl post <id> <event> key=value …`, `nuubos-notifyctl watch`.
- Headless: `tools/ui-shot/run.sh` renders `notif_*` scenes.

### 37.8 Known open items (from the 2026-10-05 review)

- GPU rendering (2026-10-06, see §37.12) still needs interactive A/B CPU measurements (navigation, QM open, HDMI 1080p) and qualification of QM menu/OSD/curtain, sleep/wake and HDMI hotplug.
- Screensaver repaints ~12 fps; display state is not event-driven (forks `displayctl`); test-tone end uses a fixed sleep; listener reconnects use fixed 250/500 ms retries.
- Wi-Fi scan completion never reaches wifid: `wpa_cli -a` does not forward `CTRL-EVENT-SCAN-RESULTS` to `nuubos-wifi-event`, so `ScanStateChanged false` and scan-session rescans never happen (pre-existing, confirmed 2026-10-05 with the old binary). Root fix: wifid attaches to the wpa_supplicant control socket (`ATTACH`) instead of the action script.
- `wifid.c` start path waits a fixed `usleep(150000)` after `wpa_supplicant -B` before `wpa_ping()`; remove it with the control-socket rework above.
- Automatic time does not sync when the network comes up: regionald only syncs every 6 h (`RESYNC_SEC`) or on `SYNC_NOW`. EPIC-002 US-ONB-002 / EPIC-053 expect NTP as soon as connectivity appears (e.g. right after the OOB Wi-Fi step). Needs a network-up event into regionald (udhcpc `bound` or wifid state), not a UI-side sync.
- Emulation (2026-10-06, §37.13) needs hardware qualification of the whole flow with real input (launch from Home, play, Quick Menu GAME actions, hotkeys, sleep/wake and power off during a game, HDMI); rumble does not reach games yet (the game gamepads have no EV_FF; forwarding to the pwm-vibrator/rumbled is still to do); direct scan-out of RetroArch not yet verified (`/sys/kernel/debug/dri/0/state` showed XR24 640x480); Home keeps its window under RetroArch and may block in eglSwapBuffers while occluded if it redraws (self-heals when visible again; measure); the RAW stream inputd sends to controllersd/systemd for activity is per event even while gaming (rate-limit candidate).
- controllersd/rumbled/lightingd store preferences under the pseudo-user `/state/users/default` while no user is active (user picker, OOB); those values never migrate to the user created afterwards. S03 ignores non-UUID directories since 2026-10-05 (it used to abort `start_state` on them, skipping network/Bluetooth/SSH setup).

### 37.9 Idle wakeup discipline

- Services must sleep with no timeout when idle; timers are armed only while work is pending (deadline computed, not a fixed tick). Measured 2026-10-05: lightingd/audiod/wifid/bluetoothd/statusd went from ~20 to ~0 wakeups/s.
- Measure on target via `voluntary_ctxt_switches` in `/proc/<pid>/status` over 30 s.
- libdbus `dbus_connection_read_write(conn, ms)` restarts its poll on EINTR: a signal does not wake it. Use own `poll()` on the D-Bus fd + a self-pipe written by the signal handler, then `read_write(conn, 0)`, pop messages, `dbus_connection_flush()` (wifid/bluetoothd pattern). Use `SA_RESTART` and handle `SIGCHLD` the same way instead of periodic `waitpid` (audiod).
- glibc `time()` reads the coarse clock and lags the timerfd by a tick; use `clock_gettime(CLOCK_REALTIME)` for wall-clock display. statusd's minute tick is a `CLOCK_REALTIME` absolute timerfd with `TFD_TIMER_CANCEL_ON_SET`; zone changes come from an inotify watch on `/etc/localtime` + `tzset()`.
- System service supports only `auto`/`battery-saver` (no Performance profile).
- Unsupported rumble/RGB rows are shown as "Unavailable" instead of hidden.

### 37.10 Initial setup / OOB (EPIC-002)

- State owner: `nuubos-usersd`. `SETUP_COMPLETE=0|1` in `/state/config/nuubos.conf`; STATUS/SUBSCRIBE snapshots carry `setup_complete=`; `selection_required` is 0 while setup is pending. `COMPLETE_SETUP` (≥1 user, else `ERR no users`) fixes DEFAULT_USER if invalid, activates the default user when there is one user or login mode DEFAULT (before committing the flag, so a failed activation stays resumable), then writes the flag. `nuubos-usersctl complete-setup` does the same.
- S03: config v6. New configs get `SETUP_COMPLETE=0`; v1–v5 migrate to v6 with `SETUP_COMPLETE=1` only if a user profile exists (dev cards provisioned by `scripts/provision-state.sh` without users still get the OOB). While pending, S03 creates no "Player 1" and publishes no active user. A missing key in usersd counts as complete.
- UI (nuubui-home): `apply_users()` enters/leaves the OOB from the usersd snapshot (`enter_oob`/`leave_oob`, static `OOB_ACTIVE` routes input to `handle_oob_action`, Start is ignored, ui-context `oob` hides QM Switch User, inputd settings capture on). Steps: 0 Welcome and 4 Ready full-screen (`oob-hero`); 1 Date & Time = settings-view 30, 2 Wi-Fi = 31 (+ reused 2/4/6), 3 Users = 32 (+ reused 26 user detail, keyboard purpose 9). The Settings rail shows the steps. Date & Time rows share `date_time_row_action()` with General rows 6–9. Every step commits immediately through the owning service, so an interrupted OOB restarts at Welcome with committed values kept.
- Creating a user (OOB or Settings → Manage Users) opens the avatar picker on the new user.
- Test on target without reflashing: `sed -i 's/^SETUP_COMPLETE=.*/SETUP_COMPLETE=0/' /state/config/nuubos.conf` then `reboot` (existing users stay and are listed in the Users step). Headless: `tools/ui-shot/run.sh` renders `oob_*` scenes.

### 37.11 Home and Game Library (EPIC-001 Home, EPIC-011/012/020 baseline)

- Owner `nuubos-libraryd` (package `nuubos-library`, `S47nuubos-library`, socket `/run/nuubos/libraryd.sock`, log `/run/nuubos/libraryd.log`, CLI `nuubos-libraryctl`). Same text protocol as usersd: `STATUS`/`SUBSCRIBE` snapshot ending in `end=1` (`recent=`, `collection=`, `system=`, `app=` lines), `GAMES\t<scope>` (`system:<id>`, `favorites`, `collection:<id>`, `recent`), `SESSION_BEGIN/END\t<game>` (Last Played/Time Played, sessions capped at 12 h), `SESSION_RECORD\t<game>\t<s>`, `FAVORITE\t<game>\t0|1`, `COLLECTION_CREATE/RENAME/DELETE/ADD/REMOVE`, `SCAN`. Idle = 0 wakeups (poll without timeout, scan on a thread, active user via inotify on `/run/nuubos/user`).
- System registry (single source): `/usr/share/nuubos/systems.conf` (`id|name|folder|exts|cover aspect x1000|colour|folder aliases`), icons `/usr/share/nuubos/systems/<id>.png` (libretro retroarch-assets XMB Monochrome, CC BY 4.0, attribution in `nuubos-library/src/systems/ATTRIBUTION.txt`; pico8 has none → gamepad glyph).
- ROMs are read ONLY under `/userdata/roms`, loose or in any sub-folder (depth 6). System = nearest folder matching a system id/folder/name/alias (case/punctuation-insensitive, e.g. `Super Famicom`, `PS1`, `Genesis`) if it takes the extension, else the only system taking the extension, else header sniff (`.cue`/`.m3u` → PLAYSTATION / SEGASATURN / SEGADISCSYSTEM / SEGAKATANA; `.bin` with `SEGA` at 0x100 → Mega Drive). Still ambiguous (`.zip`, `.chd`, `.7z` loose) → skipped and counted in the log; needs a system folder. libraryd creates `/userdata/roms/<folder>` per system. `.m3u`/`.cue` hide the files they reference; `media`, `covers`, `bios`… folders and dot-files are skipped.
- Shared catalog `/userdata/library/catalog.tsv` (v2: paths relative to roms/, identity = FNV-1a 64 of system + file name + size, so moving a ROM keeps history; same name+size twice = one game; older catalogs are rebuilt). Scans never delete: missing content becomes `available=0`. ROMs, BIOS and cover art are shared; history/favorites/collections are per user in `/userdata/users/<id>/library/{history.tsv,favorites.txt,collections.tsv}`.
- Covers (shared, until EPIC-015 scraping writes the same store): `/userdata/library/covers/<system id>/<rom name>.png|jpg`, else `media/covers/<rom name>` next to the ROM. The UI decodes them on a worker thread, downscales to the card height in physical px with rounded corners (`library.rs` `shape_cover`), keeps an LRU of 64 and loads grid covers only for rows near the focus. Card width = height x real cover aspect (clamped 0.5–1.8), else the system aspect.
- Applications: `*.app` manifests (`ID=`, `NAME=`, `ICON=`, `ORDER=`) in `/usr/share/nuubos/applications` and `/userdata/applications`; the row is hidden when empty. A on a game launches it through nuubos-emud (§37.13); launching applications is NOT implemented yet (localized "Launching is not available yet" notice) — it needs a future app session service, which must call `SESSION_BEGIN/END`.
- `RESOLVE\t<game>` (libraryd) returns `OK <system>\t<absolute content path>` for an available game (used by emud).
- UI: `package/nuubos/nuubui-home/src/src/library.rs` (models, navigation, cover loader) + `HomeCard`/`HomeCardView`/`HomeSectionTitle`/`HomeFocusInfo`/`HomeRow` and the `home-view`/`library-grid` blocks in home.slint. Rows: 0 Recently Played (10 most recent, per user), 1 Systems & Collections (Favorites when non-empty, custom collections, systems with ≥1 available game; tiles = icon + name), 2 Applications. The selected row title carries the blue bar and the focused card's title + metadata are shown under its covers (under the grid in a system/collection). A on a system/collection opens the grid, B closes it, `face_north` toggles Favorite. Home keeps the inputd UI capture (`set_ui_capture`, inputd "SETTINGS OPEN") whenever nuubUI is foreground; release it when a game/app takes over.
- inputd: new logical action `face_north` (default BTN_NORTH 307, remappable through the controllersd `face_north` control), routed like the navigation actions.
- Demo data on the dev target only (NOT in the repo; copyrighted box art for preview): placeholder ROM files named after real games in `/userdata/roms` (some loose, `My Collection/Super Famicom`, `Genesis`, multi-disc FF7), box art from libretro-thumbnails in `/userdata/library/covers/<id>/`, fake app manifests + Flathub icons in `/userdata/applications/` (`demo.*`). Remove (fish-safe): `ssh root@192.168.5.36 '/etc/init.d/S47nuubos-library stop; rm -rf /userdata/library/covers /userdata/applications/*.app /userdata/applications/icons /userdata/library/catalog.tsv /userdata/users/*/library/*; find /userdata/roms -type f -delete; /etc/init.d/S47nuubos-library start'`.
- Headless: `tools/ui-shot/run.sh` renders `home_*` scenes (recent/shelf/apps/grid/notice).

### 37.12 GPU rendering and direct scan-out (2026-10-06)

- nuubui-home (Home + Settings + OOB) selects winit + `femtovg` (OpenGL ES 2 on Mesa Panfrost) in `select_renderer()`; the Quick Menu uses `gpu.rs` (§37.3). Both fall back to the software renderer if EGL fails; `NUUBOS_UI_RENDERER=software` forces it (export it in `nuubos-ui-launch` for an A/B). Skia is excluded (C++ build / prebuilt download, not reproducible offline). `tools/ui-shot` stays on the software renderer.
- Frames reach labwc as dmabufs (no wl_shm copy). A fullscreen Home with nothing above it is scanned out directly on the primary plane; any visible QM/OSD/notification surface means composition (labwc has no overlay-plane offload). Requirements, all needed: (1) labwc patch `0002-nuubos-release-boot-splash-on-first-map.patch` (the 0001 first-frame splash is ~250 scene rects that otherwise stay under every client); (2) `HideCursor` window rule in `/etc/xdg/labwc/rc.xml` (`WLR_NO_HARDWARE_CURSORS=1` makes even the 1 px `nuubos-hidden` cursor a software cursor, which forbids scan-out; a real pointer motion shows it again); (3) linux-dmabuf feedback: Panfrost first allocates Mali 16x16 U-interleaved tiled buffers (`0x0810000000000001`) the DE33 cannot scan; after wlroots' debounce frames the client reallocates LINEAR and scan-out starts (so it engages after ~10 rendered frames, not on the very first).
- Check: `cat /sys/kernel/debug/dri/0/state` — the active plane shows `format=AR24` `modifier=0x0` `size=` the client size when scanning out. For reasons, temporarily run `labwc -d` (edit `exec /usr/bin/labwc` in `/usr/bin/nuubos-ui-launch`, restore after) and grep `scan-out` in `/run/nuubos/labwc.log` (`disabled by software cursor`, `cannot be scanned out`, `Direct scan-out enabled`).
- Measured on RG40XX-V 640x480 (idle Home, QM closed): both processes still 0 wakeups/0 CPU. Private dirty memory GPU vs software: Home 57 vs 35 MB, QM 42 vs 9.5 MB (two GL contexts); system used 211 vs 163 MB of 983. Sharing one context between the two QM windows is a possible saving (FemtoVG assumes it owns its context: test before doing it).
- Measuring: `tools/ui-nav-bench.sh [pattern] [repeat] [interval]` replays a fixed navigation on the target through a virtual pad (`tools/uinput-nav`, /dev/uinput; also `M` and volume `+`/`-`) and prints per-thread CPU; compare runs of freshly restarted UIs only (a long-running instance measures ~5% lower). Profiling: a minimal aarch64 `perf` can be built from `output/h700/build/linux-7.2.8/tools/perf` (NO_LIBELF etc.) and samples resolved on the host with the toolchain `nm` against an unstripped build (`CARGO_PROFILE_RELEASE_STRIP=false`, `DEBUG=line-tables-only`, separate `--target-dir`).
- Measured 2026-10-06 (RG40XX-V, 48 moves): Home UI thread ~25% of a core on both renderers; FemtoVG re-tessellates the whole window every animation frame. Replacing the card strokes with stacked fills, or removing the 1 px tile border, made FemtoVG ~5% SLOWER: do not repeat. Black flash when QM/OSD/notifications close (composition→direct scan-out, user-confirmed by `WLR_SCENE_DISABLE_DIRECT_SCANOUT=1`): DE33 has no DBUFF latch, so the new ARGB format applied before the new address and labwc's XRGB buffer (X byte 0) was fetched as alpha 0 = black. Fix: kernel `0028-drm-sun4i-ignore-pixel-alpha-on-bottom-layer.patch` (zpos-0 layer uses layer alpha 0xff; identical output for premultiplied content over the black background). Hardware confirmation pending.
- Software-renderer workarounds (`round_avatar()`, rounded covers in `library.rs`) stay: the fallback and ui-shot still need them and they cost nothing on the GPU.
- New crates (MIT/Apache, Slint femtovg same terms as Slint): `femtovg`, `fnv`, `gbm`, `gbm-sys`, `i-slint-renderer-femtovg`; fetch into the Buildroot cargo cache with `cargo fetch` run in the container with `CARGO_HOME=/workspace/dl/br-cargo-home` and `PATH=/workspace/output/h700/host/bin:...` (local cargo packages build `--offline --locked` from that cache).

### 37.13 Emulation: RetroArch backend (EPIC-013/019, 2026-10-06)

- Packages: `package/libretro/` — `retroarch` 1.22.2 (Wayland + EGL/GLES3 on Panfrost, udev joypads, ALSA + PipeWire, stdin commands; no X11/SDL/Vulkan/FFmpeg/KMS/slang), `retroarch-assets` (Ozone subset), `libretro-core-info`, 15 cores (matrix, licences and the non-commercial decision in `package/libretro/README.md`). `BR2_PACKAGE_NUUBOS_EMULATION` selects them all. Core build quirks live in `libretro.mk` (GCC 15 flags), `libretro-mgba` (CMake), `libretro-picodrive` (`-U_LARGEFILE64_SOURCE`). RetroArch patches: `0001` stdin `SAVE_STATE_SLOT n` / `SET_PAUSED 0|1` / `LOAD_STATE_SLOT n` with `OK|FAILED` newline-terminated replies (hotkey commands are edge-triggered bits that collapse within one frame and report nothing); `0002` Wayland EGL config for GLES2 contexts in a GLES3-only build (without it RetroArch segfaults at start).
- Service `nuubos-emud` (package `nuubos-emulation`, `S49nuubos-emud`, socket `/run/nuubos/emud.sock`, log `/run/nuubos/emud.log`, RetroArch stderr in `/run/nuubos/retroarch.log`, CLI `nuubos-emuctl status|watch|launch ID|pause|resume|save|load|slot N|+1|-1|reset|fast-forward|advanced|quit|pre-power ACTION`). Protocol like libraryd: `STATUS`/`SUBSCRIBE` snapshot (`state=idle|running|exiting`, `game=`, `system=`, `core=`, `paused=`, `slot=`, `fast_forward=`, `state_busy=`, `end=1`); `LAUNCH\t<game>` replies `OK` or `ERR busy|no-user|game|unavailable|no-core|library|storage|spawn`. One session at a time; idle = poll without timeout (timers only while a save/quit is pending); child exit via SIGCHLD self-pipe.
- Launch: libraryd `RESOLVE` → `cores.conf` (`/usr/share/nuubos/emulation/cores.conf`, `system|core,...`: first bundled core in `/usr/lib/libretro`, else first downloaded one in `/userdata/retroarch/cores`) → per-user dirs → `retroarch --config <user cfg> --appendconfig /run/nuubos/emulation/session.cfg -L core content` with stdin/stdout pipes, `setsid`, env `XDG_RUNTIME_DIR=/run/nuubos/wayland-runtime`, `HOME=<ra dir>`, `XDG_CONFIG_HOME=<user>/appdata`. libraryd `SESSION_BEGIN/END` around the process (Last/Time Played).
- Configuration layers: per-user `/userdata/users/<id>/appdata/retroarch/retroarch.cfg` seeded once from `/usr/share/nuubos/emulation/retroarch.cfg` (H700 defaults: vsync, no threaded video, core aspect, sharp-bilinear `config/global.glslp`, ALSA 64 ms, SRAM flush 10 s, no rewind/run-ahead/auto states, analog→D-pad, Ozone, Reboot/Shut Down hidden, aarch64 buildbot core URL), `retroarch-core-options.cfg` seeded from the core option defaults (global core options), user edits in RetroArch Advanced kept (`config_save_on_exit`). The session config (written every launch) enforces paths, drivers, ports, stdin commands (UDP network commands never), `quit_on_close_content`, OK = RetroPad A, language from the user's nuubOS language. Saves `/userdata/users/<id>/saves/<system>`, states `.../states/<system>` (`<rom name>.state[N]`, automatic slot `.state.auto`), screenshots `.../screenshots`, BIOS `/userdata/bios` (shared), cache `/userdata/cache/retroarch`.
- Controllers: games only see `nuubOS Gamepad` uinput devices from nuubos-inputd (one per player, product id = player, vendor 0, `/devices/virtual/input`; RetroArch reserves port N for `0000:000N`; autoconfig `/usr/share/retroarch/autoconfig/udev/nuubOS Gamepad.cfg`: SOUTH..THUMBR = buttons 0..11, D-pad hat 0, axes X/Y/RX/RY 0..3). controllersd pushes the resolved table on every device/user/mapping/deadzone/assignment change (`PADMAP BEGIN`, `PADMAP <id> player=N left_deadzone= right_deadzone= <control>=<source>...`, `PADMAP END`). emud sends `GAMEPADS ON` and keeps that connection: gamepads go off when it closes. While on, inputd grabs every controller; game controls go to the player's game gamepad (nothing while the Quick Menu is open, everything released when it opens), no nuubUI navigation leaks to Home. udev rule `71-nuubos-game-gamepads.rules` clears `ID_INPUT_JOYSTICK` (and sets `LIBINPUT_IGNORE_DEVICE`) on physical controllers so RetroArch/SDL never take them as extra players.
- Hotkeys (CLAUDE §18.2): in a game the Quick Menu button is a modifier; alone it opens the Quick Menu on release. M+R1 save state, M+L1 load state, M+Right/Left slot +/-, M+R2 fast-forward (controls are logical, follow the user's mapping). inputd writes `HOTKEY <action>` on emud's gamepads connection; emud runs the same code as the Quick Menu commands.
- Quick Menu over a game: GAME section first (Resume, Save State, Load State, State Slot ↔, Restart Game, RetroArch Advanced, Quit Game; Load/Restart/Quit need a second press); opening pauses (`PAUSE`), every close resumes (`RESUME`) except Quit and RetroArch Advanced. Switch User is hidden over a game. The power key opens the Quick Menu on Sleep while a game runs (Home ignores it: its Power menu would be hidden under RetroArch).
- Outcomes are notifications (`game.state.saved|loaded|empty|failed`, `game.slot`, `game.fastforward`, `game.failed reason=start|crash`); state saves are confirmed only when the file is closed (inotify `IN_CLOSE_WRITE`/`IN_MOVED_TO` on the state dir, 10 s timeout).
- Lifecycle: `/usr/lib/nuubos/lifecycle-hooks/pre-power.d/50-emulation` → `nuubos-emuctl pre-power <action>`: saves the automatic state slot before Sleep/Restart/Power Off and, for Restart/Power Off, quits RetroArch (SRAM written) before replying (quit timeout 10 s, then SIGTERM, then SIGKILL of the process group). The CLI gives up after 25 s.
- Home: `library.rs` `launch_game` / `start_game_listener` (`GAME_RUNNING`); `on_game_session` stops Home Music while a game runs and restarts it after. Home keeps its inputd UI capture: inputd routing ignores it while gamepads are on.
- Test without the UI: `nuubos-emuctl launch <game id>` (ids from `nuubos-libraryctl games system:<id>`). Before a scripted test check `nuubos-emuctl status` is idle: a scripted `quit` closes a game the user is playing. Dev target test content (free homebrew/redistributable, NOT in the repo, downloaded 2026-10-06): GitHub releases of pinobatch (libbet, gb240p, 240pee_mb, thwaite, croom), AntonioND/ucity, NovaSquirrel/NovaTheSquirrel, andwn/cave-story-md, mick-schroeder/gba-cascade7, abhuva/dustline, haroldo-ok (skyhaul/astro-blaster SMS, pacamaze 32X), liamhays/tunnel (GG), 1r3n33/bomberworld + undisbeliever/space-rescue-squad (SNES), trapexit/chipce8 (PCE), xdanieldzd/WS-Maze (WS), VUEngine/Capitan-Sevilla-3D (VB), ikromin/atarilynx (Lynx), vandalton/BertaAndButterflies (2600), Logan-Campbell/Tetrade (PS1), Celeste Classic (PICO-8 BBS), mamedev.org free ROMs gridlee/robby/targ/teetert (alienar and sidetrac sets do not match FBNeo). No free content found for NGP; Neo Geo and Mega-CD need BIOS images that cannot be redistributed. The 28-byte demo placeholders exercise the `game.failed reason=start` path. Smoke test: launch, `tools/grab-target.sh`, quit; check `nm -D --undefined-only` on new cores (a missing non-weak symbol only shows at load time).
- Coverage (2026-10-06, extended the same day): 84 systems in `systems.conf` (libraryd `MAX_SYSTEMS` 128), ~56 cores; matrix, licences and BIOS needs in `package/libretro/README.md`. Folder ids follow the common handheld-CFW names (e.g. `cps2`, `pcecd`, `atarist`, `zxspectrum`). Cores that need system files get them from `/usr/share/nuubos/emulation/system/` (PPSSPP assets, ScummVM data/themes, EmuTOS `tos.img`, blueMSX Databases + C-BIOS machines), copied by emud into `/userdata/bios` once per service start when missing. New core option defaults reach existing users: emud appends keys missing from their `retroarch-core-options.cfg` (values the user has are never changed).
- Whole-matrix qualification (2026-10-06): all 84 systems resolve a core that loads (`coreprobe`); every system without a BIOS requirement was launched through `nuubos-emuctl` with real content and checked running + RetroArch stream `[active]` in `wpctl status` + real screen grab (48 systems pass, Amiga partial). Not provable without user firmware: Saturn, Mega-CD, PCE-CD, FDS, Satellaview, Sufami, Neo Geo/Geolith/Neo Geo CD, NAOMI/Atomiswave, 5200 (a5200 has Altirra built in but no free content was found), Channel F, ColecoVision, Intellivision, Odyssey², Macintosh, PC-88/98, X1, X68000, Palm and the 13 MAME machines; Amiga boots AROS but the tested floppy games need a real Kickstart. Fixed then: arcade folder aliases `mame,fbneo` stole those systems' folders (first registry match wins: keep folder keys unique), blueMSX C-BIOS machines must keep their upstream `… - C-BIOS` names (their config.ini paths; the renamed copies made every MSX game segfault), SimCoupé never inserted the content (patch 0001), VICE `vice_autoloadwarp` default enabled. Harness: launch, wait, `nuubos-emuctl status`, grab, quit, wait idle — never while the user plays. RetroArch verbose repro: same env as emud (`clearenv`, `HOME`, `XDG_CONFIG_HOME`, Wayland/PipeWire runtime) + `--verbose`, user cfg + `/run/nuubos/emulation/session.cfg`. Dev target test content added: Icarus (ZX), Deathtrap (Pokémon Mini), XOR (SAM), Waternet (Mega Duck), True Colors (CPC), Chase (7800), The Race (C128), Quikman (VIC-20), Intervallo (Plus/4), Hello Dino (TIC-80), NGPC template, Cavit (SG-1000), voxel-st in an AUTO folder `.st` (ST), XRacing (MSX1), PD N64 ROMs from DerekTurtleRoe/N64-PD-ROMS.
- MAME-only machines (Apple II, CoCo, Dragon, Gamate, Game.com, Sord M5, SV8000, TI-99, TRS-80, V.Smile, SCV, Oric, FM Towns): reduced current-MAME core (`libretro-mame`, SUBTARGET nuubos, only those drivers). `cores.conf` adds `|machine|ext:switch,...`; emud writes `/run/nuubos/emulation/<game>.cmd` = `<machine> <switch> "<file>"` and launches that. Their ROMs: `/userdata/bios/mame/bios/<machine>.zip`.
- Core load check without disturbing the user (no window): `coreprobe` (dlopen + `retro_get_system_info` of every `/usr/lib/libretro/*.so`, source in the session scratchpad; rebuild with the toolchain gcc). Typical packaging faults it caught: C++ core linked with gcc (`__gxx_personality_v0`), missing `-lm` (`log10l`), CMake MODULE ignoring shared linker flags (use `CMAKE_C_STANDARD_LIBRARIES`), shared helper libs not installed (build them static), prebuilt x86 objects or hardcoded `CC=gcc` in upstream tarballs, `-rebuild` not relinking (use `-dirclean`).
- Never restart emud, the UI or run scripted launches while the user may be playing: check `nuubos-emuctl status` is idle first (a deploy on 2026-10-06 closed a running game).
- Debug a RetroArch crash: `ulimit -c unlimited` on target (core in cwd), copy the core, then on the host `gdb -batch -ex "set sysroot output/h700/staging" -ex bt output/h700/build/retroarch-1.22.2/retroarch core` (host gdb handles aarch64).

