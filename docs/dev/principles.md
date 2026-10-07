# nuubOS development principles (read before any code change)

## Sources of truth (precedence)
1. Repo code + tests (after understanding why they differ). 2. Product Epics `docs/product/nuubOS-product-epics.md` (EPIC-001..056, ENABLER-001..009, roadmap P0–P10; 280 KB: grep by EPIC id, never read whole). 3. This file + `docs/dev`. 4. Architecture docs. 5. Historical notes.
Never remove a capability during optimization/slimdown before checking Epics and Enablers. Neither preserve legacy just because it exists nor replace working architecture speculatively: classify code as current / transitional / parked legacy first. When code conflicts with these docs (§35): don't rewrite immediately; determine its status (branch/history/docs/Epics); explain the mismatch; make the minimum migration step (controlled evolution, no flag-day rewrite). Never silently alter decisions frozen here, in `docs/dev` or in the Epics.

## Architecture (§3)
- Legacy nuubUI is a fallback, recoverable in its branch/history. Frontend: Rust + Slint, Wayland/labwc where qualified, GPU rendering / direct scan-out where practical, event-driven; started from the Platform FINAL baseline, not the old frontend.
- Small functional local services (no monolith) are the source of truth. Each capability exposes a typed contract: commands + state snapshot + typed change events. The UI gets the snapshot, subscribes, updates its local model, sends commands, never reimplements policy.
- Forbidden: UI polling; UI calling `iw`/`wpa_cli`/ALSA/sysfs etc. for product behavior; generic untyped event bus where a typed contract fits; one service owning unrelated capabilities; business logic in Slint/UI/compositor; logic duplicated between Settings, Quick Menu, OOB, Home (all reuse the same services/contracts).
- Idle services sleep with no timeout (system.md §37.9).

## Working method (§4, §33)
- Inspect: relevant `docs/dev` file + Epic; `git status`; owning service/component, contracts, tests, adjacent patterns. Decide whether the issue blocks the current milestone or belongs later. Ask only for real product/architecture ambiguity the repo/docs cannot resolve.
- Implement the smallest architectural change that solves the real problem, in the owning layer. No speculative rewrites; don't polish temporary workarounds an already-planned milestone replaces; fix now when current exit criteria need it or deferring would freeze architectural debt, otherwise document/defer. Root cause over sleep/retry/rebind/timing hacks (allowed only for a proven hardware/protocol need, justified in the design).
- Verify economically: static checks → targeted build → automated tests → targeted deploy → hardware qualification when behavior depends on hardware → broader regression only if justified. Always deploy to the dev target when no flash is needed (build-deploy.md); don't reflash full images for component changes. Compile ≠ qualification: never declare FINAL without the required hardware qualification.
- Review: diff, git status, no debug leftovers/temp files, localization, licensing of new deps/assets, affected responsive layouts, service ownership/event flow, sane persistent defaults and reset semantics, remaining hardware testing.
- Hand back: what changed; builds/tests run and results; remaining hardware/manual qualification; deferred items and why.
- When in doubt optimize for architectural consistency, measured behavior, maintainability, low H700 overhead and console polish, not the shortest patch.

## Build (§5) — non-negotiable
Dockerized reproducible build: Buildroot/toolchain deps live in the versioned dev container (`nuubos-dev:0.5`), host distro irrelevant. Entry point `./scripts/compile.sh` inside the container; NEVER Buildroot `make` directly. Smallest targeted rebuild; no clean/full build for confidence, only when stale integration artifacts are possible (rootfs composition/removal, global toolchain/config, package integration requiring it, final integration qualification). Buildroot returns pristine after builds. Command and gotchas: build-deploy.md.

## Git (§7, §8)
Check status/diff before and after; stage selectively; no unrelated files; never commit or push unless asked; never silently discard user changes; clean tree at milestone boundaries; after commit remove temporary bundles/work dirs/artifacts. Work directly in the repo (no TAR collector round-trips); a bundle script, if ever needed, cleans its own temp files, keeps the output TAR and never deletes unrelated files.

## Licensing (§10) — hard requirement
Verify license/provenance before including or publishing anything redistributed (software, kernel patches, drivers, assets, fonts, themes, emulators, libraries, firmware, avatar/image packs) and track attribution/notices. Never ship firmware/assets just because they work locally. Default avatars come from an existing externally licensed set, never bespoke art. nuubOS never ships emulator BIOS/firmware.

## Data safety
Never auto-format a non-nuubOS TF2 (explicit confirmation only). Reset System Settings never erases ROMs, BIOS, saves or USERDATA. SYSTEM/STATE are never shown as user storage.

## Cross-cutting UI rules (§11, §12)
- Localization: every visible string goes through localization and is translated into all languages (de en es fr it nl pt); UI language is a per-user preference. Mechanics: ui.md.
- Responsive: native adaptive layouts for 4:3 640×480, square 720×720, 16:9/TV/4K (breakpoints/variants, no uniform scaling of a master canvas; 640×480 is not the only layout).
- Controller-first: everything usable without touch/mouse; logical actions, never Xbox/Nintendo labels; face-button hints = four dots with the relevant position filled, from the active mapping.
- Efficiency: no redraw, frames, polling or purposeless animation when nothing changes.
- Marquee: relevant/active overflowing text scrolls horizontally (shared `MarqueeText`); fitting text stays static; no continuous redraw when unneeded.
- Settings: same grammar everywhere, no fake previews (ui.md §13).

## Shell (§6) — the user's shell is fish
Commands for the user to paste: fish-compatible and complete (`cd`, paths, options); no heredocs; no `set -e`/`set -u`/`set -o pipefail`; no `exit`/`return` (closes the shell): use `&&`, conditionals, `[FAIL]` output. Downloads dir: `/home/baf/Scaricati`.
Scripts/tests: manual hardware steps in orange (`\033[38;5;208m`) with `[ACTION]`; automatic operations never `[ACTION]` (plain status or `[PASS]`/`[FAIL]`).

## Project scope
Goal: polished controller-first console UX, native UI, standard Linux foundations, strong hardware integration, low overhead, predictable behavior, clean Product API boundary. Repo `~/Projects/nuubOS`, branch `main`, remote `git@github.com:baf93/nuubOS.git`. Devices: RG35XX Pro (original reference), RG40XX-V / V2 (primary dev/qualification), RG CubeXX (multi-device qualification); never assume one board.
