# OmniBlur — After Effects Plugin

## Overview
OmniBlur is an After Effects plugin built with the Adobe AE SDK (ae25.6_61.64bit), developed in Xcode on Apple Silicon. Started as a proof-of-concept box blur; the long-term goal is a full-featured, GPU-accelerated blur toolkit with multiple algorithms and a luma-based blur map. User is not a developer, so give explicit, step by step instructions. 

## Current Status (POC)
- Plugin registers correctly in AE's Effect Manager via `PluginDataEntryFunction2`
- **Separable box blur implemented** — horizontal pass then vertical pass via an intermediate scratch `PF_EffectWorld`, replacing the original O(radius²) nested-loop blur with O(radius) per pixel. Pulled out of `Render()` into a standalone `DoBlur(in_data, radius, input, output)` helper so both the classic and Smart FX render paths can share it.
- **Converted from classic render architecture to Smart FX.** Added `PF_OutFlag2_SUPPORTS_SMART_RENDER` in `GlobalSetup`, plus `PreRender()` (`PF_Cmd_SMART_PRE_RENDER`) and `SmartRender()` (`PF_Cmd_SMART_RENDER`). `PreRender` grows the requested input checkout rect by `radius` in every direction so edge pixels don't blur against out-of-bounds data. Classic `Render()` kept as a fallback for non-Smart hosts, calling the same `DoBlur` helper. **Build succeeded, edge blurring confirmed correct.** This was the necessary prerequisite for GPU rendering — Metal isn't reachable through the classic Render path.
- **Metal GPU rendering: functionally complete end-to-end** — kernel generation, device lookup, pipeline creation, intermediate buffer, and dispatch are all wired and confirmed working (build 15). See prior session notes for the full history of this path. `extra->input->device_index` on `PF_SmartRenderExtra` and `in16f = 0` are both now **confirmed correct** against the real SDK struct (`PF_SmartRenderInput` carries `device_index`/`gpu_data`/`what_gpu` as of AE 16.0). Remaining polish (not blocking): delete the dead "Files '.cl' using Script" Build Rule — still not done.
- **16-bit and 32-bit support: in progress, CPU path only (GPU path needs no changes — see below).**
  - `DoBlur`'s pixel math is now a template, `DoBlurTyped<PixelT>`, instantiated once each for `PF_Pixel8`, `PF_Pixel16`, and `PF_PixelFloat`. A new non-template `DoBlur(in_data, bitdepth, radius, input, output)` dispatches to the right instantiation via a `switch` on `bitdepth`. Accumulators inside `DoBlurTyped` changed from `long` to `double` so the float instantiation works correctly; the two `static_cast<A_u_char>` sites became `static_cast<decltype(PixelT::alpha)>` (and same for red/green/blue) so each instantiation casts to its own channel type.
  - `SmartRender` (CPU Smart Render path) now reads `A_long bitdepth = extra->input->bitdepth;` and passes it into `DoBlur`. Field name confirmed via Xcode autocomplete.
  - Classic `Render()` (non-Smart fallback) calls `DoBlur(in_data, 8, radius, input, output)` — hardcoded to 8-bit, since modern AE always prefers Smart Render when the effect declares support for it, so this path shouldn't see 16/32-bit in practice. Documented as an accepted limitation rather than chased down.
  - `GlobalSetup` now also sets `PF_OutFlag2_FLOAT_COLOR_AWARE` on `out_data->out_flags2`, alongside the existing `SUPPORTS_SMART_RENDER` / `SUPPORTS_GPU_RENDER_F32` flags — needed to have AE offer 32bpc CPU rendering at all.
  - **Scratch `temp` world creation had to branch by pixel type.** `DoBlurTyped`'s intermediate `PF_EffectWorld` (same role as the horizontal→vertical pass handoff) was always being allocated 8-bit-sized regardless of `PixelT`, which corrupted 16-bit and 32-bit output (writes overflowing into the next row — see the alignment bug below).
    - **16-bit fix:** `PF_WorldSuite1::new_world`'s flags argument now branches at compile time via `std::is_same<PixelT, PF_Pixel16>::value`, OR-ing in `PF_NewWorldFlag_DEEP_PIXELS` when true. Confirmed working — 8-bit and 16-bit both render correctly.
    - **32-bit needed a different suite entirely.** `PF_WorldSuite1::new_world` has no pixel-format argument, so it can't create a float world at all. `PF_WorldSuite2::PF_NewWorld` does take a `PF_PixelFormat` argument directly. `DoBlurTyped` now branches on `std::is_same<PixelT, PF_PixelFloat>::value`: true → `wsP2->PF_NewWorld(effect_ref, width, height, TRUE, PF_PixelFormat_ARGB128, &temp)`; false → the existing `wsP->new_world(...)` call. `PF_PixelFormat_ARGB128` confirmed correct against `AE_EffectPixelFormat.h` (fourcc `'ae32'`, 32-bit float per channel). **Follow-up: could not reproduce the earlier visible corruption** — the original test used AE's built-in Grid effect upstream, which is only 8-bit and may have tainted the source, and clearing the AE disk cache made the errors disappear. Treating this as a likely false alarm / stale-cache artifact rather than a confirmed bug, but not fully trusted yet — needs a regression pass with a genuine 32-bit source and a clean cache (see Suggested Build Order).
  - **`AEGP_SuiteHandler.h` gotcha hit while wiring up `PF_WorldSuite2`:** the shared SDK header already has a `WorldSuite2()` accessor, but it's for the *AEGP* suite family (`AEGP_WorldSuite2`, `kAEGPWorldSuite`), not the *PF* one — same naming collision existed for `WorldSuite3`. `AEGP_SUITE_ACCESS_BOILERPLATE`'s macro definition builds both the accessor's return type AND its method name from the same `SUITE_NAME`+`VERSION_NUMBER` arguments (confirmed by reading the macro body directly), so it can't be reused with a different first argument to dodge the collision — that produces an invalid type name (`PF_PFWorldSuite2` doesn't exist). Fix: wrote the `PF_WorldSuite2` accessor by hand instead of via the macro, copying the macro's expansion exactly but naming the method `PFWorldSuite2()` instead of `WorldSuite2()` so it doesn't collide. Struct member (`world_suite2P`) and the release-boilerplate line didn't have this collision problem and use the macro normally. See "SDK-Level Edit Not Covered By Git" below — like the `PF_GPUDeviceSuite1` fix, this lives in the shared SDK checkout, not in the OmniBlur repo.
- Radius slider works (`PF_ADD_SLIDER`, read via `params[OMNIBLUR_RADIUS]->u.sd.value`)
- Deep color aware flag set (`PF_OutFlag_DEEP_COLOR_AWARE`)
- **CPU render performance: in progress.** Original CPU blur cost 256ms at radius 10 vs. native AE blurs on the same test (CC Cross Blur 22ms, Fast Box Blur 28ms, Gaussian Blur 2ms). Two fixes landed so far, both build-confirmed:
  - Converted both blur passes in `DoBlurTyped` from resumming the full window every pixel (O(radius) per pixel) to a true sliding window (O(1) per pixel): horizontal pass keeps one running sum per row; vertical pass keeps one running sum per column (`std::vector<double>`, one per channel, plus a count vector), stepped forward together as `y` advances, so output is still written row-major/cache-friendly. Added `#include <vector>`.
  - Removed the full-buffer clear on the scratch `temp` world (`flags = 0` instead of `PF_NewWorldFlag_CLEAR_PIXELS`; `clear_pixB = FALSE` instead of `TRUE`) since Pass 1 always overwrites every `temp` pixel anyway (window always includes the pixel itself, so `count >= 1` always).
  - **Remaining gap:** timings are now roughly flat across radius — single-digit radii still over 100ms, radius 10–11 fastest at ~90ms — pointing at `temp`'s per-render allocate+dispose (via `PF_WorldSuite1::new_world`/`PF_WorldSuite2::PF_NewWorld` + `dispose_world`) as the dominant fixed cost, independent of radius. See Roadmap section 1a for the caching plan to address this.
- Current build: v1.0.3, build 17 — **`BUILD_VERSION` bumped (16 → 17) and rebuilt successfully this session, confirmed.**
- **Git + GitHub connected** (repo: `github.com/kimballdenetso/OmniBlur`, public, `main` branch). Set up via Terminal rather than Xcode's Source Control UI. Notable gotchas from setup, worth remembering if repeating this for another project:
  - The `Mac` subfolder (containing the actual `.xcodeproj`) had accidentally been initialized as its own nested git repo at some point — git treats a folder containing its own `.git` as an embedded repo/gitlink, not as regular tracked files, which silently breaks clones. Fix was `rm -rf Mac/.git` before adding it to the outer repo.
  - `.gitignore` needs `build/` (no leading slash, matches at any depth) to exclude Xcode's build output — the default entries alone (`DerivedData/`, `xcuserdata/`) don't catch it, since this project's build folder is just named `build/`.
  - Standard day-to-day workflow: `git add .` → `git status` (sanity check before every commit) → `git commit -m "..."` → `git push`.
- Build succeeds and links cleanly (see Known Issues Resolved below)

## Known Issues Resolved
- **Scratch buffer allocation**: world creation/disposal is NOT on `in_data->utils` — it lives behind `PF_WorldSuite1`, accessed via `AEGP_SuiteHandler`. Correct call pattern:
  ```cpp
  AEGP_SuiteHandler suites(in_data->pica_basicP);
  PF_WorldSuite1 *wsP = suites.WorldSuite1();   // NOT WorldSuite2 — this SDK only has v1
  ERR(wsP->new_world(in_data->effect_ref, width, height, PF_NewWorldFlag_CLEAR_PIXELS, &temp));
  // ...
  ERR(wsP->dispose_world(in_data->effect_ref, &temp));
  ```
  Xcode's autocomplete (type `suites.` and let it suggest) is the most reliable way to confirm exact suite accessor names — more reliable than reading SDK header text directly, since names/versions can be inconsistent across SDK releases.

  **Update:** the comment above ("this SDK only has v1") turned out to be about `AEGP_SuiteHandler`'s *accessor coverage*, not the SDK itself — `PF_WorldSuite2` and `PF_WorldSuite3` do exist in the SDK headers, `AEGP_SuiteHandler` just didn't have working `PF_`-prefixed accessors for them. See the `PF_WorldSuite2` bit-depth work above and the manual-accessor fix under "SDK-Level Edit Not Covered By Git."
- **Linker "undefined symbol" errors for `AEGP_SuiteHandler`**: the header declares the class, but its implementation (`AEGP_SuiteHandler.cpp`) and a helper it calls (`MissingSuiteError.cpp`) are separate files in the SDK's `Examples/Util/` folder that must be explicitly added to the Xcode target — being reachable via Header Search Paths is NOT enough for `.cpp` implementation files, only for `.h` declarations. Both now added to the "Supporting Code" group with target membership checked.
- **`PF_GPUDeviceSuite1` missing from `AEGP_SuiteHandler`**: this SDK's shared `AEGP_SuiteHandler.h` (in `Examples/Util/`, not duplicated inside the OmniBlur repo) predates the GPU device suite and has no accessor for it — confirmed by inspecting the real `Suites` struct and boilerplate macro list, no `GPUDeviceSuite1`/`gpu_device_suite1P` entry anywhere. Fixed by manually adding three lines following the exact pattern already used for `WorldSuite1`: a struct member (`PF_GPUDeviceSuite1 *gpu_device_suite1P;`), a release-boilerplate line inside `ReleaseAllSuites()`, and an access-boilerplate line in the public section (`AEGP_SUITE_ACCESS_BOILERPLATE(GPUDeviceSuite, 1, PF_, gpu_device_suite1P, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);`) — plus `#include "AE_EffectGPUSuites.h"` near the file's other includes. Since this edits the *shared* SDK copy rather than a project-local file, marked with a comment explaining why it's there; the fix will also apply automatically to any other plugin built against this same SDK checkout.
  - Gotcha hit while making this edit: the release-boilerplate line and the access-boilerplate line both get added "near `world_suite1P`," but that name appears in two different sections of the file (private release section vs. public accessor section) — easy to drop both new lines in the same spot by mistake. Doing so puts a full macro-expanded function definition inside `ReleaseAllSuites()`'s body, which fails with a cluster of confusing "undeclared identifier" errors that don't obviously point at "wrong location."
  - Second gotcha: got a duplicate copy of the access-boilerplate line pasted in twice (once during the original edit, again while relocating it to fix the above) — surfaced as "Class member cannot be redeclared." Cmd+F across the file for the new member's name is the fastest way to spot duplicates.
- **Metal kernel build pipeline**: kernel source (`GF_KERNEL_FUNCTION` DSL) must be compiled at build time into a per-platform header via Adobe's `GPUUtils/CreateCString.py` — not written as raw Metal source directly. Requires: a custom Xcode Build Rule matching `*.cl` files, a Custom Path build setting pointing at the SDK's `GPUUtils` folder (`GPUUTILS_BASE_PATH`, following the same pattern as the existing `BOOST_BASE_PATH`), and Header Search Paths including `$(DERIVED_FILE_DIR)/64/PreprocessedMetal` so the generated header can be found by a plain `#include`. Verified the machine's `GPUCompiler.framework` version (`32023`, clang lib path `32023.884`) matches Adobe's reference sample exactly, so the Metal include path in the Build Rule script needed no adjustment.
- **`PF_WorldSuite2` missing a usable `PF_`-prefixed accessor from `AEGP_SuiteHandler`, AND colliding with an existing `AEGP_`-prefixed one of the same name**: unlike the `PF_GPUDeviceSuite1` case above, this wasn't just a missing accessor — `AEGP_SuiteHandler.h` already had a `WorldSuite2()` method (and a `WorldSuite3()` one), but both are for the AEGP suite family, not PF. `AEGP_SUITE_ACCESS_BOILERPLATE`'s macro derives its generated method's name from the same arguments as the type it returns, so a second macro invocation aimed at the PF suite can't reuse the same "WorldSuite"+"2" naming without either colliding with the existing method or producing a type name that doesn't exist. Fixed by hand-writing the accessor (not via the macro) with a differently-named method, `PFWorldSuite2()`, returning the correct `PF_WorldSuite2*` type. See "SDK-Level Edit Not Covered By Git" below for the exact code and why it needs redoing if the SDK checkout is ever replaced.

## SDK-Level Edit Not Covered By Git
**`METAL_STUB_HEADERS_PATH` Custom Path:** like `BOOST_BASE_PATH` and `GPUUTILS_BASE_PATH`, this is an Xcode app-level preference (Xcode → Settings → Locations → Custom Paths), not a project file — so it's **not backed up by git** and won't exist on a fresh machine or after an Xcode reinstall. If the Metal shader ever starts failing again with "redefinition of 'clamp'" (or similar redefinition errors for `metal_compute`/`metal_integer`/`metal_texture`), check that this Custom Path still exists and points at:
```
/Users/kimball/Documents/Dev/AE-SDK/ae25.6_61.64bit.AfterEffectsSDK/Examples/Effect/OmniBlur/MetalStubHeaders
```
That folder itself (4 empty files: `metal_common`, `metal_compute`, `metal_integer`, `metal_texture`) *is* inside the OmniBlur repo path and so *is* tracked by git — only the Xcode-side Custom Path pointer needs manual recreation.

The `PF_GPUDeviceSuite1` fix to `AEGP_SuiteHandler.h` (see Known Issues Resolved above) lives in the shared AE SDK folder, not inside the OmniBlur repo — so it's **not backed up by version control**. If that SDK checkout is ever reinstalled, wiped, or replaced, this fix needs to be redone by hand. Steps to redo:

1. Open `AEGP_SuiteHandler.h` (find it via Cmd+click on `#include "AEGP_SuiteHandler.h"` in `OmniBlur.cpp`, or it's at `Examples/Util/AEGP_SuiteHandler.h` inside the SDK checkout).
2. Add `#include "AE_EffectGPUSuites.h"` near the file's other includes at the top.
3. Inside the private `struct Suites { ... }` block, find `PF_WorldSuite1 *world_suite1P;` and add right after it:
   ```cpp
   PF_GPUDeviceSuite1			*gpu_device_suite1P;
   ```
4. Inside `ReleaseAllSuites()`, find `AEGP_SUITE_RELEASE_BOILERPLATE(world_suite1P, kPFWorldSuite, kPFWorldSuiteVersion1);` and add right after it:
   ```cpp
   AEGP_SUITE_RELEASE_BOILERPLATE(gpu_device_suite1P, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
   ```
5. In the public section, find `AEGP_SUITE_ACCESS_BOILERPLATE(WorldSuite, 1, PF_, world_suite1P, kPFWorldSuite, kPFWorldSuiteVersion1);` and add right after it:
   ```cpp
   AEGP_SUITE_ACCESS_BOILERPLATE(GPUDeviceSuite, 1, PF_, gpu_device_suite1P, kPFGPUDeviceSuite, kPFGPUDeviceSuiteVersion1);
   ```
6. Double check with Cmd+F for `gpu_device_suite1P` — should be exactly 3 matches (struct member, release line, access line), none of them inside `ReleaseAllSuites()`'s body except the release line itself.

**`PF_WorldSuite2` accessor fix — also not backed up by git, also lives in the shared SDK's `AEGP_SuiteHandler.h`.** Steps to redo if the SDK checkout is ever wiped:

1. Open `AEGP_SuiteHandler.h`.
2. Inside the private `struct Suites { ... }` block, find `PF_WorldSuite1 *world_suite1P;` and add right after it:
   ```cpp
   PF_WorldSuite2			*world_suite2P;
   ```
3. Inside `ReleaseAllSuites()`, find `AEGP_SUITE_RELEASE_BOILERPLATE(world_suite1P, kPFWorldSuite, kPFWorldSuiteVersion1);` and add right after it:
   ```cpp
   AEGP_SUITE_RELEASE_BOILERPLATE(world_suite2P, kPFWorldSuite, kPFWorldSuiteVersion2);
   ```
4. In the public section, right after the existing `WorldSuite1()` accessor method, **do not** use `AEGP_SUITE_ACCESS_BOILERPLATE` for this one — it collides with the existing AEGP-family `WorldSuite2()` method already in this file. Write it by hand instead:
   ```cpp
   PF_WorldSuite2 *PFWorldSuite2() const
   {
       if (i_suites.world_suite2P == NULL) {
           i_suites.world_suite2P = (PF_WorldSuite2*)LoadSuite(kPFWorldSuite, kPFWorldSuiteVersion2);
       }
       return i_suites.world_suite2P;
   }
   ```
5. Double check with Cmd+F for `world_suite2P` — should be exactly 3 matches (struct member, release line, access line inside the hand-written method), none inside `ReleaseAllSuites()`'s body except the release line.
6. In `OmniBlur.cpp`, the call site is `suites.PFWorldSuite2()`, not `suites.WorldSuite2()`.

Worth considering longer-term: copying these headers into the OmniBlur repo (alongside the already-copied `AEGP_SuiteHandler.cpp`/`MissingSuiteError.cpp`) so these fixes travel with the project instead of living in a shared, unversioned SDK folder — this is now the second fix living only in the shared SDK copy, which raises the cost of ever losing that checkout.

## Remaining Known Issues
- **Bit depth**: 8-bit and 16-bit CPU rendering confirmed correct. `PF_PixelFormat_ARGB128` for the 32-bit scratch world confirmed correct against `AE_EffectPixelFormat.h`. Earlier "32-bit renders incorrectly" observation could not be reproduced on retest — likely tainted by testing against AE's 8-bit-only built-in Grid effect and/or a stale disk cache. **Not fully trusted either way yet** — still needs one clean regression pass with a genuine 32-bit source and a cleared cache (tracked in Suggested Build Order / Roadmap 2).
- **Metal/GPU acceleration**: kernel generation, device lookup, and full dispatch are done — GPU rendering confirmed working end-to-end since build 15 (see Current Status above). Specifically still open:
  - ~~`extra->input->device_index` on `PF_SmartRenderExtra`~~ **CONFIRMED** — real `PF_SmartRenderInput` struct carries `device_index` (added AE 16.0).
  - ~~`in16f = 0` as the correct "not half-float" value~~ **CONFIRMED** — left as-is, correct for this F32-only GPU path.
  - Delete the dead "Files '.cl' using Script" Build Rule — still not done.
  - No CUDA host-launch wrapper was written for `OmniBlur_Kernel.cu` (intentionally omitted — Mac-only project, `__NVCC__`-guarded code isn't needed)
- **CPU render performance**: sliding-window blur math and scratch-world clear removal both landed and helped substantially, but per-render allocate/dispose of the scratch `temp` world is now the dominant remaining cost (see Current Status and Roadmap 1a). Not yet built: caching `temp` across renders via `PF_Cmd_SEQUENCE_SETUP`/`PF_Cmd_SEQUENCE_SETDOWN`.

## Architecture Notes
- Entry point: `EffectMain`, dispatches on `PF_Cmd` (ABOUT, GLOBAL_SETUP, PARAMS_SETUP, RENDER, and now GPU_DEVICE_SETUP / GPU_DEVICE_SETDOWN / SMART_RENDER_GPU)
- Registration: `PluginDataEntryFunction2` (required — PiPL alone doesn't register the plugin on this SDK setup)
- Header quirk: `entry.h` lives in `Examples/Util/`, not `Examples/Headers/` — added to Xcode Header Search Paths
- `PiPL.h` does not exist in this SDK version
- `DllExport` macro doesn't resolve in this project's include setup — using `__attribute__((visibility("default")))` instead
- Suite access pattern: any AE SDK functionality beyond basic `in_data`/`out_data` fields (e.g. world allocation, pixel iteration, sampling, GPU device info) is reached via a "suite" struct of function pointers, fetched through `AEGP_SuiteHandler`. This pattern held for the GPU device suite and `PF_WorldSuite2` too, once the missing/colliding accessors were added by hand (see Known Issues Resolved).
- `AEGP_SuiteHandler`'s `AEGP_SUITE_ACCESS_BOILERPLATE` macro derives both the generated accessor method's name AND its return type from the same `SUITE_NAME`+`VERSION_NUMBER` arguments — so a `PF_`-prefixed suite sharing a name with an already-present `AEGP_`-prefixed one (e.g. `WorldSuite2`, `WorldSuite3`) can't be added via the macro without either colliding or breaking the type name. Hand-write the accessor in that case, keeping the correct type but giving the method a distinguishing name (e.g. `PFWorldSuite2()`).
- Kernel source split: `OmniBlur_Kernel.cu` (real cross-platform DSL implementation) + `OmniBlur_Kernel.cl` (thin include trigger for the Build Rule) — mirrors Adobe's own `SDK_Invert_ProcAmp` sample structure exactly.
- Pixel-depth-generic CPU blur math lives in a template, `DoBlurTyped<PixelT>`, instantiated for `PF_Pixel8`/`PF_Pixel16`/`PF_PixelFloat`; a plain (non-template) `DoBlur(bitdepth, ...)` dispatches to the right one via a runtime `switch`.

## Roadmap

### 1. Performance: Metal GPU Rendering — **DONE**
- Move render path from CPU (`PF_EffectWorld` pixel loops) to a Metal compute shader
- Requires: Metal device/command queue setup, bridging `PF_EffectWorld` data to `MTLTexture`/`MTLBuffer`, compute pipeline state, threadgroup dispatch sized to frame dimensions
- ~~Consider `PF_OutFlag2_SUPPORTS_GPU_RENDER_F32` / Smart Render pipeline~~ **DONE**
- ~~Interim CPU win before Metal lands: separable blur~~ **DONE**
- ~~Identify the exact GPU device suite accessor and wire up the real MTLDevice/command queue lookup~~ **DONE**
- ~~Kernel build wiring: get `GF_KERNEL_FUNCTION` DSL translating into a usable Metal source string at build time~~ **DONE**
- ~~Intermediate GPU-resident scratch buffer (`CreateGPUWorld`/`DisposeGPUWorld`)~~ **DONE**
- ~~Write the real `MTLComputeCommandEncoder` dispatch in `SmartRenderGPU` (two passes, horizontal then vertical) and test an actual GPU-rendered blur in AE~~ **DONE — build 15, confirmed visually correct in AE**
- Reference: Adobe's `SDK_Invert_ProcAmp` sample implements this full pipeline (CUDA/OpenCL/Metal) and was cross-checked directly for the kernel Build Rule script and the `PF_GPUDeviceSuite1` call shape
- Remaining polish (not blocking): delete the dead "Files '.cl' using Script" Build Rule. `in16f = 0` and `extra->input->device_index` are now both confirmed correct (see Remaining Known Issues) — no longer open.
- **Notable because the GPU-side format decision (`PF_PixelFormat_GPU_BGRA128`, F32-only) turned out to make the GPU path automatically bit-depth-agnostic** — AE converts 8/16-bit source layers to F32 for the GPU world and back, so none of the bit-depth work below touches `SmartRenderGPU` at all.

### 1a. Performance: CPU Render Path — **IN PROGRESS**
Started because the CPU blur was far slower than comparable native AE blurs at the same radius: 256ms at radius 10 vs. CC Cross Blur 22ms, Fast Box Blur 28ms, Gaussian Blur 2ms.

- ~~Sliding-window box sum for both blur passes (O(1)/pixel instead of O(radius)/pixel)~~ **DONE** — horizontal pass: one running sum per row. Vertical pass: one running sum per column (`std::vector<double>` × channel + a count vector), stepped forward as `y` advances, so output stays row-major/cache-friendly instead of switching to column-major traversal.
- ~~Remove the unneeded full-buffer clear on the scratch `temp` world~~ **DONE** — `flags = 0` / `clear_pixB = FALSE`, since Pass 1 always overwrites every pixel.
- **Cache the scratch `temp` world across renders instead of allocate+dispose every call — DRAFTED, NOT YET BUILT.** With the above two fixes in place, timing is now roughly flat across radius (single-digit radii still >100ms, radius 10–11 fastest at ~90ms), which points at the per-render `new_world`/`PF_NewWorld` + `dispose_world` cost as the dominant remaining fixed cost, independent of blur math. Plan:
  1. New struct, e.g. `OmniBlurSequenceData`, holding: a cached `PF_EffectWorld` scratch world, its cached `width`/`height`/`bitdepth`, and a `has_temp` flag.
  2. `PF_Cmd_SEQUENCE_SETUP` (and `PF_Cmd_SEQUENCE_RESETUP` — VERIFY whether these need distinct handling or can share one `switch` case): allocate a handle sized to this struct, zero it (`has_temp = FALSE`), store it as `out_data->sequence_data`. VERIFY exact handle allocation API via autocomplete (candidates: `PF_NEW_HANDLE`/`PF_LOCK_HANDLE`/`PF_UNLOCK_HANDLE`/`PF_DISPOSE_HANDLE` macros — `Param_Utils.h`/`AE_Macros.h` are already included).
  3. `DoBlurTyped`: instead of unconditionally allocating `temp` at the top and disposing it at the bottom, check the cached sequence data — if `has_temp` and cached `width`/`height`/`bitdepth` match the current call, reuse the cached world directly (skip allocation entirely); otherwise dispose the stale cached world (if any), allocate a fresh one via the existing branch (`PF_WorldSuite1::new_world` / `PF_WorldSuite2::PF_NewWorld`), and update the cached width/height/bitdepth + `has_temp = TRUE`. The scratch world's lifetime moves from "one render call" to "the life of the sequence" — `dispose_world` no longer happens at the end of every `DoBlurTyped` call.
  4. `PF_Cmd_SEQUENCE_SETDOWN`: if `has_temp`, `dispose_world` the cached scratch world; then dispose the sequence data handle itself.
  5. VERIFY: is `in_data->sequence_data` already valid/locked to dereference directly inside `SmartRender`/`Render`, or does it need an explicit lock call first? Differs across SDK versions/patterns — confirm via autocomplete/doc comment before writing the real access code.
  6. Reallocation-on-mismatch (step 3) already covers comp resize, bit-depth changes, and proxy toggling — no separate invalidation logic needed beyond the width/height/bitdepth compare.
  - **Confirmed safe from a threading standpoint**: OmniBlur does not set `PF_OutFlag2_SUPPORTS_THREADED_RENDERING`, so `sequence_data` is never accessed by more than one render call at a time — no locking/races to worry about beyond AE's normal handle lock/unlock convention. If MFR (multi-frame rendering) support is ever added later, this whole scheme needs revisiting in favor of something like `PF_EffectSequenceDataSuite1` or a per-thread cache instead of one shared cached world.
  - **Out of scope for this plan**: the GPU path's own intermediate buffer (`CreateGPUWorld`/`DisposeGPUWorld` in `SmartRenderGPU`) is a separate, already-necessary per-render GPU-resident allocation tied to `PF_GPUDeviceSuite1` — not touched by this CPU-path caching work. Could be a future optimization in its own right, not part of this plan.

### 2. Bit Depth Support: 16-bit and 32-bit — **IN PROGRESS**
- ~~8-bit baseline~~ **DONE** (was always the baseline)
- ~~16-bit path (`PF_Pixel16`)~~ **DONE** — confirmed rendering correctly in AE after fixing the scratch-world allocation to request `PF_NewWorldFlag_DEEP_PIXELS` for this instantiation
- **32-bit float path (`PF_PixelFloat`): likely working, not fully confirmed.** Scratch world created via `PF_WorldSuite2::PF_NewWorld` with `PF_PixelFormat_ARGB128` (confirmed correct constant). Earlier visible-corruption observation could not be reproduced on retest (possible tainted 8-bit test source / stale cache — see Remaining Known Issues). Needs one clean regression pass with a genuine 32-bit source before calling this DONE.
- `DoBlur`'s pixel math templated (`DoBlurTyped<PixelT>`) rather than duplicated per depth — see Architecture Notes
- `GlobalSetup` now declares `PF_OutFlag2_FLOAT_COLOR_AWARE` in addition to the existing Smart Render / GPU flags
- Classic (non-Smart) `Render()` fallback intentionally stayed 8-bit-only — not expected to be exercised on any modern host

### 3. Multiple Blur Algorithms
- Add an algorithm selector param (`PF_ADD_POPUP` or similar) to switch between:
  - **Fast Blur** (AE-style approximate/optimized blur)
  - **Box Blur** (current POC baseline)
  - **Lens Blur** (bokeh-style, likely needs a proper circular kernel / possibly depth-aware)
  - (room for more — e.g. Gaussian, directional/motion blur)
- Each algorithm likely wants its own render function, dispatched by the selected popup value, sharing the same param plumbing where possible

### 4. Luma-Based Blur Map (reference layer)
- Add a layer parameter (`PF_ADD_LAYER`) to let users pick a second layer as a blur map input
- Sample the luma of that layer per-pixel to modulate blur radius/intensity spatially (bright = more blur, dark = less, or vice versa — likely worth an invert toggle)
- Needs to handle the reference layer's own resolution/transform relative to the effect layer (checkout point sampling / resizing behavior)

## Suggested Build Order
1. ~~Separable blur (quick CPU perf win, low risk)~~ **DONE**
2. ~~Convert classic render architecture to Smart FX (prerequisite for GPU rendering)~~ **DONE**
3. ~~Metal compute shader for box blur~~ **DONE — build 15, confirmed working in AE**
4. **16/32-bit + GPU on/off full regression pass — IN PROGRESS.** 8-bit and 16-bit confirmed. 32-bit needs one clean pass with a genuine 32-bit source and a cleared cache before calling it done (see Roadmap 2 / Remaining Known Issues). Test all 6 combinations: 8/16/32-bit × GPU on/off. Decided to land this and the performance work below before starting new blur algorithms.
5. **CPU render performance — IN PROGRESS.** Sliding-window blur math and scratch-world clear removal done; sequence-data caching for the scratch `temp` world drafted but not yet built (Roadmap 1a) — needed to close the remaining gap to native AE blur speeds.
6. Additional blur algorithms (fast blur, lens blur) built on top of the working GPU pipeline
7. Luma-based blur map (layered on top, since it modulates whichever algorithm is active)

## Tooling
- Xcode (Apple Silicon), AE SDK ae25.6_61.64bit, checkout at `/Users/kimball/Documents/Dev/AE-SDK/ae25.6_61.64bit.AfterEffectsSDK`
- Git + GitHub for version control
- Custom Xcode paths (Locations → Custom Paths): `BOOST_BASE_PATH` (Boost), `GPUUTILS_BASE_PATH` (SDK's `Examples/GPUUtils` folder, used by the kernel Build Rule)
- **Deployment is automated via an Xcode Run Script build phase** — every build copies, unquarantines, and re-signs the plugin directly into AE's plugin folder. No manual copy step needed:
  ```bash
  PLUGIN_DEST="/Applications/Adobe After Effects 2026/Plug-ins/voidy/OmniBlur.plugin"

  rm -rf "$PLUGIN_DEST"
  cp -R "${TARGET_BUILD_DIR}/${WRAPPER_NAME}" "$PLUGIN_DEST"
  xattr -dr com.apple.quarantine "$PLUGIN_DEST" 2>/dev/null

  codesign --force --deep --sign - "$PLUGIN_DEST"

  echo "Copied and signed OmniBlur.plugin to AE Plug-ins folder"
  ```
- Full quit/relaunch of AE is still required after each build to pick up the updated plugin — AE only scans its plugin folder at launch.
- **Bump the version/build number in the `About()` string (`MAJOR_VERSION`/`MINOR_VERSION`/`BUG_VERSION`/`BUILD_VERSION` at the top of `OmniBlur.cpp`) on every build.** AE caches plugin metadata aggressively; without a version bump, it's easy to be looking at a stale build and mistake it for the current one. Check via Effect > About in AE, or the effect's own About entry, to confirm the version matches what you just built. **`BUILD_VERSION` is currently 17 (bumped and rebuilt this session) — remember to bump again before starting the next session's work.**
- To check whether GPU rendering is active for a given test: File > Project Settings > Video Rendering and Effects, check the Mercury GPU Acceleration setting. If `GPUDeviceSetup` fails, AE shows a warning banner on the effect in Effect Controls ("GPU device setup failed; the effect will render on the CPU") — no banner means the GPU path is running.
