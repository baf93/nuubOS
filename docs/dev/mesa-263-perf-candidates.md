# Mesa 26.3.x performance candidates for H700 / Mali-G31 (2026-11-25)

**Status: measured; patches are applied and deployed in the current test build, pending the user's keep/restore decision. No commit has been made.**
Scope agreed with the user: **only new Mesa patches that improve performance without degrading image quality**, with a general H700 benefit. Dusklight-side changes (port config, shaders, renderer, launcher, resolution scale), presentation shaders, GPU/CPU clocks and the previously rejected levers in `perf-power.md` are out of scope.

## Why this file exists

- The shipped Mesa 26.2.7 upgrade (patches `0004-0011`, LLVM/CLC on, LTO off) was for correctness (issue #44), **not** performance (`perf-power.md` Facts, 2026-11-24).
- The old "no Mesa upgrade for performance" rejection was based on Mesa 25.1–26.0. Mesa 26.3 contains new Bifrost v7 work, so candidates must be validated commit-by-commit instead of assuming the old rejection still holds.
- This file keeps the exact commit set, the applicability test and the reasons for each decision, so a context compaction does not lose the work.

## Method (reproducible)

- GitLab tags: `mesa-26.2.7` target `ec135886cc4fe67fc7795f9cfd5c67bc1762b970`, commit `b69adc2adfbfe3eb985bd66cd06c61ca719ae9ae`; `mesa-26.3.1` target/commit `fec8830e9c94b1d67f3d8469467d5529fb2978cd` (26.3.2 returns 404).
- GitLab commit search is unusable here: `repository/commits?search=` returns unrelated recent commits and `/search?scope=commits` returns 401. `repository/commits?ref_name=` caps at 5900 entries. Use `repository/compare?from=<sha>&to=<sha>` (5975 commits) instead.
- Keyword filter on commit titles (`/tmp/opencode/mesa-after-2627.txt`, 418 commits; broader pass `/tmp/opencode/mesa-all-relevant.txt`, 721 commits). Both are volatile.
- Per-commit `/diff` JSON downloaded to `/tmp/opencode/mesa-diffs/<sha>.diff`, converted to patch files in `/tmp/opencode/mesa-candidates/<sha>.patch`.
- Applicability tested against `output/h700/build/mesa3d-26.2.7` with `patch -p1 --dry-run` forward **and** reverse. `REV:OK` means the change is already present in our tree (typically cherry-picked under a different SHA).
- A patch is only considered a performance candidate if a consumer of the changed data affects generated code or runtime state. Several commits that look promising by title only feed shader-db statistics.

## Classification

### Already present in our 26.2.7 tree (no action)

| SHA | Title |
|---|---|
| `b84ccacd` | panfrost: restore max retry count for bifrost LCRA spills (400 -> 2000) |
| `6f9da7ba` | pan/bifrost: Fix shader clock selection |
| `d0379e79` | pan/bi: Fix spill cost overestimation |
| `f82fcf30` | panfrost,nir: Get rid of `nir_downgrade_pls_vars()` |
| `298d75bb` | pan/perf: Fix swapped block counts |
| `33e67876` | pan/bi: Restore `VA_MEMORY_ACCESS_FORCE` for spills and refills |
| `8a70efc3` | panfrost: free correct handle |
| `8ecd9488` | pan/bi: handle tls-stores in helper analysis |
| `d00b719c` | pan/bi: Fix vector phis from spilling |
| `3111d15c` | pan/bi: Propagate swizzle in `bi_optimizer_result_type` |
| `829e6d16` | nir/opt_vectorize: Remove the phis that were combined |

Note: `git` metadata is absent from the build tree, so "present" is established from the source contents and the reverse dry-run, not from history.

### Rejected — no runtime effect, correctness/feature only, or explicit visual risk

| SHA | Title | Reason |
|---|---|---|
| `847fac45` | panfrost/model: Add Bifrost pixel/texel/FMA rates | Feeds only `bi_gather_stats()` (shader-db/debug) and Vulkan `pixelRate`/`texelRate`/`fmaRate` (informational). Verified: no consumer changes generated code. |
| `27b3c0e7` | pan/bi: Make `bi_gather_stats` arith and texel rates model-aware | Same: statistics only. |
| `f572b93c` | pan/bi: Calculate spills/fills in `compute_spill_cost()` | Verified: `ctx->spills`, `ctx->fills`, `ctx->spill_cost` are consumed only by `bifrost_compile.c` stats output. Statistics only, no codegen effect. |
| `af01ef6c` | pan/perf: Adjust varying rate calculations | Valhall profiling counters only. |
| `5cb7f1a1` | pan/perf: fix external bus width | Fixes a profiling constant (`/8`), not runtime. Useful for measurement accuracy, not a performance patch. |
| `007c20df` | vbuf: rename `fallback_always` to `needs_fallback` | Rename only. |
| `19bbed7c` | panfrost: remove redundant `util_dynarray_init` | Cleanup only. |
| `3c13a7bb` | pan/compiler: drop needless temporary | Cleanup only. |
| `05019db8` | panfrost: use `nir_i2iN()` in `lower_bool_to_bitsize_instr()` | Replaces a local switch with the equivalent helper; no functional change. |
| `44c6486a` | bifrost/nir: Lower `fexp2`/`flog2`/`fpow` to scalar | Commit message: "Bifrost doesn't hit this but Kraid does" (v11+ only). |
| `d49ecd54` | bifrost/nir: Lower `fpow16` to `fexp2`+`flog2` | Message: lowering does not trigger on the old (v7) compiler. |
| `cec0f455` | pan/bi: Lower `imul_32x16`/`umul_32x16` in NIR | Added for `VK_INTEL_shader_integer_functions2`; costs one 16->32 conversion per multiply. Not a win for GL ES. |
| `bab52852` | pan/bi/nir: Scalarize the rest of `b(any|all)` | Only subgroup ballot ops; not used by our GL ES clients. |
| `835cde31` | panfrost/shader: Only lower IO when needed | Only for TTN/PanVK (`io_lowered == false`); gallium already lowers IO. |
| `df23de8a` | pan/midgard: Fix 16-bit varying lowering | Midgard only, not Bifrost v7. |
| `dbda1473` | panfrost/mid: Disable mediump lowering entirely | Midgard only. |
| `b1e091fb` | st/mesa: relax TexSubImage/ReadPixels fast path NPOT requirements | Panfrost advertises `npot_textures = true`; no behaviour change for us. |
| `f9970996` | st/mesa,nir: optimize `GL_CLAMP` lowering | Touches LOD saturation/texture sampling semantics — visual risk; not a general win. |
| `834517d3` | nir: Skip `fminmax` preserve_sz lowering if non-zero consts are used | Signed-zero correctness fix, not performance. |
| `e429c980` | nir: reduce regs inserted in `nir_lower_ssa_defs_to_regs_block` | Compile-time lowering cleanup, not a runtime change for Panfrost. |
| `63942887` | nir: optimize large constants in all functions | Constant dedup at link time; no runtime effect. |
| `9a2b3746` | nir: skip mov/vec in LSB/UUB analysis | Compile-time analysis only. |
| `e7e1d372` | nir: pass to optimize scalar arrays to vec arrays | New pass is enabled only by radv; no Panfrost consumer. |
| `f19d6046` | pan/bi: Vectorize load/store before `lower_mem_access_bit_sizes()` | Reordering is a correctness fix for Kraid (v11 subgroup broadcast), with 1 of 4 hunks conflicting. Not a performance patch. |
| `a311d430`, `7677a163`, `dbd4e604`, `10de23e1`, `77f680eb`, `6671fc22`, `ed3906dc`, `b28ac47d`, `ca509ccf`, `151ff104` | EGL/DRI swap-buffers-with-damage refactors | High risk: our custom patches `0005`–`0007` hook exactly this path; dry-runs conflict. Deprioritized. |
| `62393b8b` | gallium: default `max_texture_upload_memory_budget` | Upload budget policy change, not a G31 raster fix; dry-run fails. |
| `827d538e` | panfrost: add ai-policy | Feature, not an optimization. |
| `e6912be8`, `d5052234` | NULL-batch handling / PLS render target mask | Correctness fixes, not performance. |
| `cdd80dca` | panfrost: Preserve BO flags during linear conversion | Correctness; dry-run fails. |

### Candidates still under evaluation (genuine codegen/perf changes)

| SHA | Title | Dry-run | Notes |
|---|---|---|---|
| `d8c17bac` | pan/bi: run `nir_opt_gcm` to hoist loop-invariant code | FWD OK | **Top candidate.** Real optimization: the Bifrost scheduler is local and cannot move code across blocks, so loop-invariant expressions (worst case: modulo by a loop-invariant divisor -> reciprocal-multiply) are recomputed every iteration. Semantics preserving. The commit reports 2.09x on a G310/Rusticl CL kernel and -0.06 to -0.10 % instructions in Valhall shader-db with **HURT: 0**; no Bifrost v7 numbers were published, so the G31 gain must be measured. Later nir/gcm fixes (`0159ac73`, `cfb36311`, `7b587186`, `bad339cf`, `db47c799`, `417e761f`) apply cleanly but are refinements of the pass; `03da5874`, `7d64f0d5`, `fa855fea`, `859fcc3b` conflict heavily. |
| `86e10b67` | panfrost: enable ASTC decode mode on Bifrost (v7) | 1 of 1 hunk fails | Real v7 power win, **no visual degradation**: fp32-FBO readback confirms the 8-bit intermediate and the commit reports texture-cache miss 0.32 % -> 0.003 % on a bilinear-bound ASTC workload at unchanged traffic/shader cycles. Tested on G52 (also v7). Payoff only if a client uses ASTC compressed textures; probably rare in our emulators. Needs manual adaptation of `pan_texture.c`. |
| `45006301` | panfrost/algebraic: use `u2uN` instead of `i2iN` for downcasts | FWD OK | Codegen improvement avoiding signed conversions; applies to v7 (`f2i8` and `ldexp16` changes) while the other hunks are gated `is_kraid` / `gpu_arch >= 11`. Semantics preserving, small. |
| `8d09120d` | pan/bifrost: Re-vectorize loads in the compiler | 1 of 2 hunks fail | Adds `nir_lower_io_to_scalar` + `nir_opt_vectorize_io` on shader inputs; part of a large MR whose prerequisite refactors (`35077121`, `b2c14af8`) also conflict. Potential win from fewer scalar loads on v7, but requires the whole series. |
| `b2c14af8` | pan/compiler: Lower mediump varying floats to 16-bits early | 1 of 1 hunk fails | Large atomic change (95 KB diff) that makes gallium's varying memory layout 16-bit for mediump. Correctness-sensitive (smooth/interpolated mediump), needs careful image checks; also requires the MR's prerequisite refactors. High risk. |
| `263eb470` | pan/compiler: Optimize flat 16-bit load on linked shaders | 1 of 1 hunk fails | Touches `pan_nir_resize_varying_io`; our tree has an older/different signature (`pan_nir_resize_varying_io(nir, layout)`). Needs manual adaptation. |
| `29e0483c` | panfrost: accelerate buffer->buffer copies with a compute shader | 1 of 3 hunks fail | Real optimization, but compute path on v7 and conflicts need review. |
| `d32b763d` | cso,st/mesa: send vertex formats that need translation to u_vbuf | 1 of 2 hunks fail | Relevant to the Saturn vertex-copy cost (~11.8 % of the VDP thread is u_vbuf client arrays), but a generic GL path change; needs review. |
| `5799705d` | cso,u_vbuf: threaded drivers skip u_vbuf when unneeded | dry-run text mismatch | Likely aimed at threaded drivers (radeonsi), not Panfrost; verify before considering. |
| `51e6ee6c` | panfrost: no-op texture framebuffer barriers | FWD OK | Skips the full batch flush for `PIPE_TEXTURE_BARRIER_FRAMEBUFFER`. Zero visual risk; payoff depends on whether any GL client uses framebuffer-fetch barriers (probably rare). Low priority. |

## Prepared patch batch (built, deployed, measured, restored)

Minimal first batch, both semantics-preserving. The patches are kept in the repo:

- `patches/packages/mesa3d/26.2.7/0012-pan-bi-run-nir-opt-gcm-to-hoist-loop-invariant-code.patch` (upstream `d8c17bac`, 2026-09-08):
  `bi_optimize_late()` now runs `nir_opt_gcm` + `nir_opt_dce`. Affects `src/panfrost/compiler/bifrost/bifrost_nir.c` only.
  **26.2.7 adaptation:** the first build failed because `nir_opt_gcm()` still has three arguments in 26.2.7 (`shader, value_number, hoist_tex_from_loops`); upstream 26.3 removed the third. The backport passes `true` for `hoist_tex_from_loops` to match 26.3 behaviour (NIR protects texture instructions with implicit derivatives in both versions). Hunk count corrected to `+11`.
- `patches/packages/mesa3d/26.2.7/0013-panfrost-algebraic-use-u2uN-for-downcasts.patch` (upstream `45006301`, 2026-09-25):
  `src/panfrost/compiler/bifrost/bifrost_nir_algebraic.py`: bit-identical `u2uN` downcasts, plus related `b2f`/`b2i` and `ldexp16_pan` patterns.

They touch different files, so there is no interaction. `0013` edits a generator input; Meson regenerates `bifrost_nir_algebraic.h` at build time.

Not included yet: `86e10b67` (ASTC narrow decode v7) and `51e6ee6c` (no-op texture framebuffer barriers) — keep separate until a client that exercises them is identified. Not included: any GCM refinement commit, since the base pass is already an improvement over no pass.

### Build / deploy facts (2026-11-25)

- No other build was running (`ps` check); targeted `./scripts/compile.sh mesa3d-rebuild` in Docker, no `--clean`, no deletion under `output/`. Build completed successfully.
- Buildroot `-rebuild` does **not** re-apply package patches: both patches were applied by hand to `output/h700/build/mesa3d-26.2.7` and appended to `.applied_patches_list` (needed by `legal-info`) before the rebuild.
- Rebuilt `libgallium-26.2.7.so` md5 `5f2dcbf458e75bc78a674ee4c3b19d3d` (target original `483263e26ce64deb5ae0afb4d0902045`, backed up in `/userdata/nuubos-deploy-backup-20261125/`). Deployed with the UI stopped, then restarted `S50nuubos-ui`.
- After the A/B, the original library/cache/launcher were restored *for that experiment*. Later in this session (2026-11-26), for the gl4es/Mesa validation round, both patches were applied by hand to the extracted tree and a targeted `mesa3d-rebuild` + deploy put `libgallium-26.2.7.so` md5 `5f2dcbf458e75bc78a674ee4c3b19d3d` back on the device. The baseline library remains backed up in `/userdata/nuubos-deploy-backup-20261125/` (md5 `483263e26ce64deb5ae0afb4d0902045`). `config.json` md5 stayed `6970749990b086abb4f5097d959b9589`; no Dusklight file was changed.

## A/B result on H700 / Dusklight (2026-11-25)

Bench: Dusklight at its unmodified default scale 0.6667, same disc/save. Presented frames were recorded by a temporary `LD_PRELOAD` hook on `SDL_GL_SwapWindow` (u32 µs per presented frame); each run had a 65 s warm-up followed by one 60 s window. GPU OPP residency came from `/sys/class/devfreq/1800000.gpu/trans_stat`; fragment busy from fdinfo `drm-engine-fragment` (`dusk-gpubusy.sh` prints percent × 100 — divide by 100). Order A (baseline library) → B (patched) → A′ (baseline) → B′ (patched).

| run | lib | presented fps | p50 / p90 / p99 (ms) | GPU residency | fragment busy |
|---|---|---|---|---|---|
| A | baseline `483263e2` | 29.67 | 33.34 / 33.97 / 51.34 | 600 MHz 99 % | 38.8 % |
| B | patched `5f2dcb45` | 28.09 | 33.44 / 43.67 / 53.72 | 600 MHz 99 % | 47.3 % |
| A′ | baseline `483263e2` | 26.79 | 33.55 / 51.62 / 54.92 | 432 MHz 62 %, 552 MHz 25 % | 55.1 % |
| B′ | patched `5f2dcb45` | 26.92 | 33.55 / 51.41 / 55.73 | 432 MHz 60 %, 552 MHz 27 % | 54.5 % |

- A′ and B′ were run back-to-back at the same throttled OPPs and are **within 0.5 %** in fps with an identical p50: no measurable gain and no regression. The A vs B difference is not reproducible and coincides with different DVFS/thermal state (and possibly different in-game animation), so it is not evidence of a slowdown from the patches.
- `drm-grab` framebuffers: the green/blue channels are pixel-identical between A′ and B′ (100 %), only the animated red-noise layer differs. No visual degradation attributable to either patch was observed.
- Attempted deterministic shader comparison via `BIFROST_MESA_DUMP_DIR` + `BIFROST_MESA_DEBUG=shaders`: Dusklight produced only three internal `pan_preload` shaders without a source hash; its own program cache bypasses recompilation, so no compiled game shaders could be dumped. Clearing the game/Mesa caches did not change this. This avenue is closed for Dusklight; a G31 shader-db-style harness would be needed instead.
- Conclusion: **both patches are safe but show no measurable improvement in the chosen workload.** Do not ship them on performance grounds without a better-controlled benchmark (stable clocks, deterministic scene) or evidence from a workload whose shaders actually contain loop-invariant math / narrow downcasts.

## Baseline facts

- Dusklight (2026-11-25, H700): presented fps at internal scale 0.5 / 0.6667 / 0.75 / 1.0 = 29.0 / 20.4 / 15.9 / 9.7; frame time ≈ `12 ms + 91 ms * scale^2`; GPU ~99 % at 600 MHz; both threads dominated by `DRM_IOCTL_SYNCOBJ_WAIT`. This is GPU raster/fill bound — a compiler patch can only help if it reduces shader work per pixel. Default config was restored (`md5 6970749990b086abb4f5097d959b9589`); no Dusklight file is part of this work.
- General baselines (`perf-power.md`): heavy systems already at 56–78 % GPU fragment busy, `GET_FPS` includes skipped frames, so use presented frames (`tools/perf-power/framebench.sh`), GPU busy, `pcsample` and `drm-grab` image diffs.
- Session-to-session fps in Dusklight varies by >10 % with GPU OPP state (600 MHz vs 432/552 MHz); single-run A/B is not sufficient here. Always record residency with the frame numbers.

## Next steps

1. **Decision closed (2026-11-26):** the user chose to **keep** `0012`+`0013` applied and deployed (`libgallium-26.2.7.so` md5 `5f2dcbf458e75bc78a674ee4c3b19d3d`). The baseline backup remains at `/userdata/nuubos-deploy-backup-20261125/` (md5 `483263e26ce64deb5ae0afb4d0902045`). They are **not** presented as a performance win: the matched-clock A/B was 26.79 vs 26.92 fps with identical p50.
2. If revisiting, first find a workload/benchmark with deterministic shaders and stable GPU clocks (a G31 shader-db harness, or a scene with loop-invariant divisions), then re-run A/B/A/B; otherwise drop the batch.
3. Keep `86e10b67` and `51e6ee6c` as separate, low-risk additions if a client actually exercises them; do not bundle them into a first batch without evidence.
4. Drop anything that is stats-only, feature-only, or needs an unsafe refactor (table above).
5. If building again: check no other build is running, apply new patches by hand to the already-extracted tree (and append to `.applied_patches_list`), then a targeted `mesa3d-rebuild` via `./scripts/compile.sh` in Docker; deploy afterwards. Never delete `output/` or use `--clean`.
6. Record any future A/B here and in `perf-power.md`. No commit unless explicitly asked.

## Temporary artifacts (volatile, /tmp)

- `/tmp/opencode/relnotes-26.3.{0,1}.html`, `/tmp/opencode/compare-2627-2631.json`
- `/tmp/opencode/mesa-after-2627.txt` (418 commits), `/tmp/opencode/mesa-all-relevant.txt` (721 commits)
- `/tmp/opencode/mesa-diffs/*.diff`, `*.msg`, `/tmp/opencode/mesa-candidates/*.patch`
- `/tmp/opencode/mesa-candidate-messages.txt`, `/tmp/opencode/mesa-diffs-clean{,2}.txt`, `/tmp/opencode/mesa-shortlist-diffs.txt`, `/tmp/opencode/mesa-new-diffs.txt`
- A/B: `/tmp/opencode/dusk-ab-run.sh`, `dusk-metric.sh`, `dusk-dump-shaders.sh`, `dssh.sh`, `dscp.sh`, `sdl-frametime.c/.so`, `ft-{baseline,baseline2,candidate,candidate2}.bin`, `{base,baseline2,cand,candidate2}.ppm`, `mesa-rebuild{,2}.log`
